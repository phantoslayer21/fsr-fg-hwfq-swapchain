// media_probe: does a flip-model swapchain expose IDXGISwapChainMedia (present duration)? D3D11 and D3D12, on a
// window that is never shown. Nothing is presented. Own code; Windows SDK only.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static void Probe(const char* name, IUnknown* deviceOrQueue, IDXGIFactory2* f, HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 256; sd.Height = 256; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    HRESULT hr = f->CreateSwapChainForHwnd(deviceOrQueue, hwnd, &sd, nullptr, nullptr, &sc);
    ComPtr<IDXGISwapChainMedia> media;
    HRESULT hq = sc ? sc.As(&media) : E_FAIL;
    printf("%s: CreateSwapChainForHwnd hr=0x%08lx, QueryInterface(IDXGISwapChainMedia) hr=0x%08lx\n", name, hr, hq);
    if (media) {
        const UINT want[] = { 27778, 41667, 55556, 83333, 166667 };
        for (UINT w : want) {
            UINT s = 0, l = 0;
            HRESULT hc = media->CheckPresentDurationSupport(w, &s, &l);
            printf("  CheckPresentDurationSupport(%u) hr=0x%08lx smaller=%u larger=%u\n", w, hc, s, l);
        }
    }
}

int main()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"media_probe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"media_probe", WS_POPUP, 0, 0, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    ComPtr<IDXGIFactory6> f;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a));
    ComPtr<ID3D11Device> d11;
    D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d11,
                      nullptr, nullptr);
    Probe("D3D11", d11.Get(), f.Get(), hwnd);
    HWND hwnd2 = CreateWindowExW(0, wc.lpszClassName, L"media_probe2", WS_POPUP, 0, 0, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    ComPtr<ID3D12Device> d12;
    D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> q;
    d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    Probe("D3D12", q.Get(), f.Get(), hwnd2);
    return 0;
}
