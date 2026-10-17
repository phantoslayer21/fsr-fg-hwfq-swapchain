// fg_host: a minimal "game" that uses FSR frame generation through the FFX API exactly like a title does: it creates the
// frame-generation swapchain and the frame-generation context through the FFX DLL next to it, renders a moving bar with
// depth and motion vectors, dispatches prepare + configure every frame and presents. Run it once with AMD's original DLL
// and once with fgswap in its place to compare.
// Usage: fg_host [--dll name] [--seconds S] [--base-hz F] [--w W --h H] [--borderless] [--vsync] [--ui] [--off] [--load N] [--mode-hz H] [--game-above-normal]
//   --ui         register a UI resource (a small overlay rectangle) through the swapchain configure call
//   --off        frame generation disabled (swapchain passthrough)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include "ffx_abi.h"

using Microsoft::WRL::ComPtr;
#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("FAILED 0x%08lx %s line %d\n", h_, #x, __LINE__); return 2; } } while (0)

static bool g_quit;
static std::atomic<bool> g_stopLoad{false};
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_KEYDOWN && w == VK_ESCAPE) || m == WM_DESTROY) { g_quit = true; return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

static ffx::PfnDispatch g_dispatch;
// What games pass as frameGenerationCallback: dispatch the generated frame on their frame-generation context.
static ffx::ReturnCode FgCallback(ffx::DispatchFrameGeneration* params, void* userCtx)
{
    return g_dispatch((ffx::Context*)userCtx, &params->header);
}

static void Msg(uint32_t type, const wchar_t* m) { wprintf(L"  [ffx %s] %s\n", type == 0 ? L"error" : L"warn", m); }

int main(int argc, char** argv)
{
    std::string dll = "amd_fidelityfx_framegeneration_dx12.dll";
    double seconds = 5, baseHz = 60;
    UINT W = 1280, H = 720;
    bool borderless = false, vsync = false, useUi = false, fgOn = true;
    int load = 0, modeHz = 0;
    bool gameAbove = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--dll") dll = next();
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--base-hz") baseHz = atof(next().c_str());
        else if (a == "--w") W = atoi(next().c_str());
        else if (a == "--h") H = atoi(next().c_str());
        else if (a == "--borderless") borderless = true;
        else if (a == "--vsync") vsync = true;
        else if (a == "--ui") useUi = true;
        else if (a == "--off") fgOn = false;
        else if (a == "--load") load = atoi(next().c_str());
        else if (a == "--mode-hz") modeHz = atoi(next().c_str());
        else if (a == "--game-above-normal") gameAbove = true;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HMODULE m = LoadLibraryA(dll.c_str());
    if (!m) { printf("LoadLibrary(%s) failed %lu\n", dll.c_str(), GetLastError()); return 1; }
    auto create = (ffx::PfnCreateContext)GetProcAddress(m, "ffxCreateContext");
    auto destroy = (ffx::PfnDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    auto configure = (ffx::PfnConfigure)GetProcAddress(m, "ffxConfigure");
    auto query = (ffx::PfnQuery)GetProcAddress(m, "ffxQuery");
    auto dispatch = (ffx::PfnDispatch)GetProcAddress(m, "ffxDispatch");
    g_dispatch = dispatch;
    ffx::ConfigureGlobalDebug1 dbg = { { ffx::CONFIGURE_GLOBALDEBUG1, nullptr }, Msg, 2 };
    configure(nullptr, &dbg.header);

    if (modeHz) {  // temporary, process-only display mode (like a full-screen game); restored at exit
        DEVMODEW dm = { sizeof(dm) };
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
        dm.dmDisplayFrequency = (DWORD)modeHz;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        LONG r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        printf("display mode %d Hz: %ld\n", modeHz, r);
        Sleep(1500);
    }
    if (borderless) {
        HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        W = mi.rcMonitor.right - mi.rcMonitor.left;
        H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"fg_host";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    DWORD style = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, (LONG)W, (LONG)H };
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"fg_host", style | WS_VISIBLE, borderless ? 0 : 100, borderless ? 0 : 100,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> dev;
    CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));

    // Frame-generation swapchain through the FFX API.
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    IDXGISwapChain4* sc = nullptr;
    ffx::CreateFgSwapChainVersionDX12 scv = { { ffx::CREATE_FGSWAPCHAIN_VERSION_DX12, nullptr }, ffx::FGSWAPCHAIN_VERSION };
    ffx::CreateFgSwapChainForHwndDX12 scd = { { ffx::CREATE_FGSWAPCHAIN_FOR_HWND_DX12, &scv.header }, &sc, hwnd, &sd, nullptr,
                                             factory.Get(), queue.Get() };
    ffx::Context scCtx = nullptr;
    ffx::ReturnCode rc = create(&scCtx, &scd.header, nullptr);
    printf("swapchain context rc=%u sc=%p\n", rc, (void*)sc);
    if (rc || !sc) return 3;

    // Frame-generation context.
    ffx::CreateFrameGenerationVersion ver = { { ffx::CREATE_FRAMEGENERATION_VERSION, nullptr }, ffx::FRAMEGENERATION_VERSION };
    ffx::CreateBackendDX12 be = { { ffx::CREATE_BACKEND_DX12, &ver.header }, dev.Get() };
    ffx::CreateFrameGeneration cfg = { { ffx::CREATE_FRAMEGENERATION, &be.header }, 0, { W, H }, { W, H }, ffx::FMT_R8G8B8A8_UNORM };
    ffx::Context fgCtx = nullptr;
    rc = create(&fgCtx, &cfg.header, nullptr);
    ffx::QueryGetProviderVersion pv = { { ffx::QUERY_GET_PROVIDER_VERSION, nullptr }, 0, nullptr };
    query(&fgCtx, &pv.header);
    ffx::QueryGetProviderVersion spv = { { ffx::QUERY_GET_PROVIDER_VERSION, nullptr }, 0, nullptr };
    query(&scCtx, &spv.header);
    printf("FG context rc=%u provider %s; swapchain provider %s\n", rc, pv.versionName ? pv.versionName : "?",
           spv.versionName ? spv.versionName : "?");
    if (rc) return 3;

    // Game resources.
    ComPtr<ID3D12CommandAllocator> alloc[3];
    for (auto& a : alloc) CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list)));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 fv = 0, allocFv[3] = {};
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto tex = [&](DXGI_FORMAT f, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = f; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> t;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&t));
        return t;
    };
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ComPtr<ID3D12Resource> depth = tex(DXGI_FORMAT_R32_FLOAT, SRV), mv = tex(DXGI_FORMAT_R16G16_FLOAT, SRV);
    ComPtr<ID3D12Resource> ui = tex(DXGI_FORMAT_R8G8B8A8_UNORM, SRV);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 + 3 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc; return h; };
    dev->CreateRenderTargetView(depth.Get(), nullptr, rtv(3));
    dev->CreateRenderTargetView(mv.Get(), nullptr, rtv(4));
    dev->CreateRenderTargetView(ui.Get(), nullptr, rtv(5));
    if (useUi) {
        ffx::ConfigureFgSwapChainRegisterUiResourceDX12 u = { { ffx::CONFIGURE_FGSWAPCHAIN_REGISTERUIRESOURCE_DX12, nullptr },
                                                             ffx::MakeResource(ui.Get(), ffx::STATE_PIXEL_COMPUTE_READ), 0 };
        printf("register UI rc=%u\n", configure(&scCtx, &u.header));
    }

    // games usually run their render thread above normal; with --load the background then cannot starve it
    if (gameAbove) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    // --load N: N busy threads at normal priority (a CPU-bound game)
    std::vector<std::thread> loaders;
    for (int i = 0; i < load; ++i)
        loaders.emplace_back([] { volatile double x = 1; while (!g_stopLoad) for (int j = 0; j < 100000; ++j) x = x * 1.0000001 + 1e-9; });
    LARGE_INTEGER f, t0, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const int64_t period = (int64_t)(f.QuadPart / baseHz);
    int64_t next = t0.QuadPart;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int SPEED = 12;
    uint64_t k = 0;
    MSG msg;
    while (!g_quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - t0.QuadPart) > (int64_t)(seconds * f.QuadPart)) break;
        // frame cap (the "game" renders at base-hz)
        next += period;
        int64_t wait = next - now.QuadPart;
        if (wait > 0) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(wait * 10000000 / f.QuadPart);
            SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(timer, INFINITE);
        } else {
            next = now.QuadPart;
        }
        int a = (int)(k % 3);
        if (fence->GetCompletedValue() < allocFv[a]) { fence->SetEventOnCompletion(allocFv[a], ev); WaitForSingleObject(ev, INFINITE); }
        alloc[a]->Reset();
        list->Reset(alloc[a].Get(), nullptr);
        UINT bi = sc->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        sc->GetBuffer(bi, IID_PPV_ARGS(&bb));
        dev->CreateRenderTargetView(bb.Get(), nullptr, rtv(bi % 3));
        LONG x = (LONG)((k * SPEED) % (W - 60));
        const float bg[4] = { 0.08f, 0.08f, 0.1f, 1 }, fgc[4] = { 1, 1, 1, 1 }, dz[4] = { 0.5f, 0, 0, 0 };
        const float mvv[4] = { x < SPEED ? 0.0f : -(float)SPEED, 0, 0, 0 };
        const float clear0[4] = { 0, 0, 0, 0 }, uic[4] = { 0.9f, 0.3f, 0.1f, 0.8f };
        D3D12_RECT bar = { x, 0, x + 48, (LONG)H }, uiRect = { 20, 20, 300, 80 };
        Barrier(list.Get(), bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(list.Get(), depth.Get(), SRV, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(list.Get(), mv.Get(), SRV, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(list.Get(), ui.Get(), SRV, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(rtv(bi % 3), bg, 0, nullptr);
        list->ClearRenderTargetView(rtv(bi % 3), fgc, 1, &bar);
        list->ClearRenderTargetView(rtv(3), dz, 0, nullptr);
        list->ClearRenderTargetView(rtv(4), mvv, 0, nullptr);
        list->ClearRenderTargetView(rtv(5), clear0, 0, nullptr);
        list->ClearRenderTargetView(rtv(5), uic, 1, &uiRect);
        Barrier(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV);
        Barrier(list.Get(), mv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV);
        Barrier(list.Get(), ui.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, SRV);
        Barrier(list.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

        ffx::DispatchFrameGenerationPrepareV2 prep = {};
        prep.header.type = ffx::DISPATCH_FRAMEGENERATION_PREPARE_V2;
        prep.frameID = k;
        prep.commandList = list.Get();
        prep.renderSize = { W, H };
        prep.motionVectorScale = { 1, 1 };
        prep.frameTimeDelta = (float)(1000.0 / baseHz);
        prep.reset = k == 0;
        prep.cameraNear = 0.1f; prep.cameraFar = 1000; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1;
        prep.depth = ffx::MakeResource(depth.Get(), ffx::STATE_PIXEL_COMPUTE_READ);
        prep.motionVectors = ffx::MakeResource(mv.Get(), ffx::STATE_PIXEL_COMPUTE_READ);
        prep.cameraUp[1] = 1; prep.cameraRight[0] = 1; prep.cameraForward[2] = 1;
        rc = dispatch(&fgCtx, &prep.header);
        if (rc && k < 3) printf("prepare rc=%u\n", rc);
        ffx::ConfigureFrameGeneration c = {};
        c.header.type = ffx::CONFIGURE_FRAMEGENERATION;
        c.swapChain = sc;
        c.frameGenerationCallback = FgCallback;
        c.frameGenerationCallbackUserContext = &fgCtx;
        c.frameGenerationEnabled = fgOn;
        c.generationRect = { 0, 0, (int)W, (int)H };
        c.frameID = k;
        rc = configure(&fgCtx, &c.header);
        if (rc && k < 3) printf("configure rc=%u\n", rc);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fv);
        allocFv[a] = fv;
        HRESULT hr = sc->Present(vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        if (FAILED(hr)) { printf("Present failed 0x%08lx\n", hr); break; }
        ++k;
    }
    g_stopLoad = true;
    for (auto& t : loaders) t.join();
    printf("rendered %llu real frames in %.1f s\n", (unsigned long long)k, seconds);
    // shut down like a game: disable FG, wait for presents, destroy
    ffx::ConfigureFrameGeneration off = {};
    off.header.type = ffx::CONFIGURE_FRAMEGENERATION;
    off.swapChain = sc;
    off.frameID = k;
    configure(&fgCtx, &off.header);
    ffx::Header wfp = { ffx::DISPATCH_FGSWAPCHAIN_WAIT_FOR_PRESENTS_DX12, nullptr };
    dispatch(&scCtx, &wfp);
    queue->Signal(fence.Get(), ++fv);
    fence->SetEventOnCompletion(fv, ev);
    WaitForSingleObject(ev, INFINITE);
    destroy(&fgCtx, nullptr);
    sc->Release();
    destroy(&scCtx, nullptr);
    printf("clean shutdown\n");
    return 0;
}
