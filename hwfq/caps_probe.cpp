// caps_probe: record what the installed display driver reports about flip queues, independent flip, MPO,
// hardware scheduling, tearing/VRR and the Composition Swapchain API. Read-only queries, nothing is presented.
// Own code; Windows SDK headers only.
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>
#include <Presentation.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

static D3DKMT_HANDLE g_adapter;

template <class T>
static NTSTATUS Query(KMTQUERYADAPTERINFOTYPE type, T& out)
{
    D3DKMT_QUERYADAPTERINFO q = {};
    q.hAdapter = g_adapter;
    q.Type = type;
    q.pPrivateDriverData = &out;
    q.PrivateDriverDataSize = sizeof(T);
    return D3DKMTQueryAdapterInfo(&q);
}

static void PrintVersion(const char* name, LARGE_INTEGER v)
{
    printf("  %-28s %u.%u.%u.%u\n", name, (unsigned)(v.QuadPart >> 48), (unsigned)((v.QuadPart >> 32) & 0xffff),
           (unsigned)((v.QuadPart >> 16) & 0xffff), (unsigned)(v.QuadPart & 0xffff));
}

static const char* SupportState(UINT s)
{
    switch (s) {
    case 0: return "ALWAYS_OFF";
    case 1: return "EXPERIMENTAL";
    case 2: return "STABLE";
    case 3: return "ALWAYS_ON";
    }
    return "?";
}

static void ProbeKmt(LUID luid)
{
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = luid;
    NTSTATUS st = D3DKMTOpenAdapterFromLuid(&open);
    if (st != 0) { printf("  D3DKMTOpenAdapterFromLuid failed 0x%08lx\n", st); return; }
    g_adapter = open.hAdapter;

    D3DKMT_DRIVERVERSION wddm = {};
    if (Query(KMTQAITYPE_DRIVERVERSION, wddm) == 0) printf("  %-28s %u (WDDM %u.%u)\n", "WDDM (DRIVERVERSION)", (unsigned)wddm, (unsigned)wddm / 1000, ((unsigned)wddm % 1000) / 100);
    D3DKMT_UMD_DRIVER_VERSION umd = {};
    if (Query(KMTQAITYPE_UMD_DRIVER_VERSION, umd) == 0) PrintVersion("UMD driver version", umd.DriverVersion);
    D3DKMT_KMD_DRIVER_VERSION kmd = {};
    if (Query(KMTQAITYPE_KMD_DRIVER_VERSION, kmd) == 0) PrintVersion("KMD driver version", kmd.DriverVersion);

    D3DKMT_WDDM_2_7_CAPS c27 = {};
    st = Query(KMTQAITYPE_WDDM_2_7_CAPS, c27);
    printf("  WDDM_2_7_CAPS st=0x%08lx value=0x%08x HwSchSupported=%u HwSchEnabled=%u HwSchEnabledByDefault=%u IndependentVidPnVSyncControl=%u\n",
           st, c27.Value, c27.HwSchSupported, c27.HwSchEnabled, c27.HwSchEnabledByDefault, c27.IndependentVidPnVSyncControl);
    D3DKMT_WDDM_2_9_CAPS c29 = {};
    st = Query(KMTQAITYPE_WDDM_2_9_CAPS, c29);
    printf("  WDDM_2_9_CAPS st=0x%08lx value=0x%08x HwSchSupportState=%s HwSchEnabled=%u SelfRefreshMemorySupported=%u\n",
           st, c29.Value, SupportState(c29.HwSchSupportState), c29.HwSchEnabled, c29.SelfRefreshMemorySupported);
    D3DKMT_WDDM_3_0_CAPS c30 = {};
    st = Query(KMTQAITYPE_WDDM_3_0_CAPS, c30);
    printf("  WDDM_3_0_CAPS st=0x%08lx value=0x%08x HwFlipQueueSupportState=%s HwFlipQueueEnabled=%u DisplayableSupported=%u\n",
           st, c30.Value, SupportState(c30.HwFlipQueueSupportState), c30.HwFlipQueueEnabled, c30.DisplayableSupported);
    D3DKMT_WDDM_3_1_CAPS c31 = {};
    st = Query(KMTQAITYPE_WDDM_3_1_CAPS, c31);
    printf("  WDDM_3_1_CAPS st=0x%08lx value=0x%08x NativeGpuFenceSupported=%u\n", st, c31.Value, c31.NativeGpuFenceSupported);

    D3DKMT_FLIPQUEUEINFO fq = {};
    st = Query(KMTQAITYPE_FLIPQUEUEINFO, fq);
    printf("  FLIPQUEUEINFO st=0x%08lx MaxHardwareFlipQueueLength=%u MaxSoftwareFlipQueueLength=%u FlipInterval=%u\n",
           st, fq.MaxHardwareFlipQueueLength, fq.MaxSoftwareFlipQueueLength, fq.FlipFlags.FlipInterval);

    D3DKMT_DIRECTFLIP_SUPPORT df = {};
    st = Query(KMTQAITYPE_DIRECTFLIP_SUPPORT, df);
    printf("  DIRECTFLIP_SUPPORT st=0x%08lx Supported=%d\n", st, df.Supported);
    D3DKMT_INDEPENDENTFLIP_SUPPORT inf = {};
    st = Query(KMTQAITYPE_INDEPENDENTFLIP_SUPPORT, inf);
    printf("  INDEPENDENTFLIP_SUPPORT st=0x%08lx Supported=%d\n", st, inf.Supported);
    D3DKMT_MULTIPLANEOVERLAY_SUPPORT mpo = {};
    st = Query(KMTQAITYPE_MULTIPLANEOVERLAY_SUPPORT, mpo);
    printf("  MULTIPLANEOVERLAY_SUPPORT st=0x%08lx Supported=%d\n", st, mpo.Supported);
    D3DKMT_MPO3DDI_SUPPORT mpo3 = {};
    st = Query(KMTQAITYPE_MPO3DDI_SUPPORT, mpo3);
    printf("  MPO3DDI_SUPPORT st=0x%08lx Supported=%d\n", st, mpo3.Supported);
    D3DKMT_MPOKERNELCAPS_SUPPORT mpok = {};
    st = Query(KMTQAITYPE_MPOKERNELCAPS_SUPPORT, mpok);
    printf("  MPOKERNELCAPS_SUPPORT st=0x%08lx Supported=%d\n", st, mpok.Supported);
    D3DKMT_DISPLAY_CAPS dc = {};
    st = Query(KMTQAITYPE_DISPLAY_CAPS, dc);
    printf("  DISPLAY_CAPS st=0x%08lx value=0x%016llx\n", st, (unsigned long long)dc.Value);
    D3DKMT_ADAPTERTYPE at = {};
    st = Query(KMTQAITYPE_ADAPTERTYPE, at);
    printf("  ADAPTERTYPE st=0x%08lx value=0x%08x RenderSupported=%u DisplaySupported=%u HybridDiscrete=%u\n", st, at.Value,
           at.RenderSupported, at.DisplaySupported, at.HybridDiscrete);

    // Per-source MPO caps for VidPnSource 0..3 (the ones that exist answer).
    for (UINT src = 0; src < 4; ++src) {
        D3DKMT_GET_MULTIPLANE_OVERLAY_CAPS mc = {};
        mc.hAdapter = g_adapter;
        mc.VidPnSourceId = src;
        if (D3DKMTGetMultiPlaneOverlayCaps(&mc) == 0)
            printf("  MPO caps source %u: MaxPlanes=%u MaxRGBPlanes=%u MaxYUVPlanes=%u OverlayCaps=0x%08x MaxStretch=%.2f MaxShrink=%.2f\n",
                   src, mc.MaxPlanes, mc.MaxRGBPlanes, mc.MaxYUVPlanes, mc.OverlayCaps.Value, mc.MaxStretchFactor, mc.MaxShrinkFactor);
    }

    D3DKMT_CLOSEADAPTER close = { g_adapter };
    D3DKMTCloseAdapter(&close);
}

static void ProbeComposition(IDXGIAdapter1* adapter)
{
    ComPtr<ID3D11Device> d11;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &d11, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        ComPtr<IPresentationFactory> f;
        hr = CreatePresentationFactory(d11.Get(), IID_PPV_ARGS(&f));
        printf("  CreatePresentationFactory(D3D11 device) hr=0x%08lx", hr);
        if (SUCCEEDED(hr))
            printf(" IsPresentationSupported=%d IsPresentationSupportedWithIndependentFlip=%d", (int)f->IsPresentationSupported(),
                   (int)f->IsPresentationSupportedWithIndependentFlip());
        printf("\n");
    }
    ComPtr<ID3D12Device> d12;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12));
    if (SUCCEEDED(hr)) {
        ComPtr<IPresentationFactory> f;
        hr = CreatePresentationFactory(d12.Get(), IID_PPV_ARGS(&f));
        printf("  CreatePresentationFactory(D3D12 device) hr=0x%08lx", hr);
        if (SUCCEEDED(hr))
            printf(" IsPresentationSupported=%d IsPresentationSupportedWithIndependentFlip=%d", (int)f->IsPresentationSupported(),
                   (int)f->IsPresentationSupportedWithIndependentFlip());
        printf("\n");
        if (SUCCEEDED(hr)) {
            // Can a D3D12 texture be a presentation buffer? Try plain and shared committed textures.
            ComPtr<IPresentationManager> mgr;
            hr = f->CreatePresentationManager(&mgr);
            printf("  CreatePresentationManager(D3D12) hr=0x%08lx\n", hr);
            const D3D12_HEAP_FLAGS heapFlags[2] = { D3D12_HEAP_FLAG_NONE, D3D12_HEAP_FLAG_SHARED };
            for (int k = 0; mgr && k < 2; ++k) {
                D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC rd = {};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                rd.Width = 256; rd.Height = 256; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count = 1;
                rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                ComPtr<ID3D12Resource> tex;
                hr = d12->CreateCommittedResource(&hp, heapFlags[k], &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex));
                ComPtr<IPresentationBuffer> buf;
                HRESULT hr2 = tex ? mgr->AddBufferFromResource(tex.Get(), &buf) : E_FAIL;
                printf("  AddBufferFromResource(D3D12 texture, heap %s) create=0x%08lx add=0x%08lx\n", k ? "SHARED" : "NONE", hr, hr2);
            }
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        ComPtr<ID3D12CommandQueue> q;
        d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        f.Reset();
        hr = CreatePresentationFactory(q.Get(), IID_PPV_ARGS(&f));
        printf("  CreatePresentationFactory(D3D12 queue) hr=0x%08lx", hr);
        if (SUCCEEDED(hr))
            printf(" IsPresentationSupported=%d IsPresentationSupportedWithIndependentFlip=%d", (int)f->IsPresentationSupported(),
                   (int)f->IsPresentationSupportedWithIndependentFlip());
        printf("\n");
    }
}

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { printf("no DXGI factory\n"); return 1; }
    BOOL tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    printf("DXGI_FEATURE_PRESENT_ALLOW_TEARING=%d\n", tearing);

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        LARGE_INTEGER umd = {};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
        printf("\nAdapter %u: %ls  VendorId=0x%04x DeviceId=0x%04x  LUID=%08lx:%08lx\n", i, d.Description, d.VendorId, d.DeviceId,
               d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        PrintVersion("DXGI UMD version", umd);
        ProbeKmt(d.AdapterLuid);
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> o6;
            out.As(&o6);
            DXGI_OUTPUT_DESC1 od;
            o6->GetDesc1(&od);
            UINT hwc = 0;
            o6->CheckHardwareCompositionSupport(&hwc);
            DEVMODEW dm = { sizeof(dm) };
            EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
            DWORD maxHz = 0;
            DEVMODEW em = { sizeof(em) };
            for (DWORD m = 0; EnumDisplaySettingsW(od.DeviceName, m, &em); ++m)
                if (em.dmPelsWidth == dm.dmPelsWidth && em.dmPelsHeight == dm.dmPelsHeight && em.dmDisplayFrequency > maxHz)
                    maxHz = em.dmDisplayFrequency;
            printf("  Output %u %ls: mode %lux%lu, highest %lu Hz at this size\n", o, od.DeviceName, dm.dmPelsWidth, dm.dmPelsHeight, maxHz);
            printf("    desktop %ldx%ld @ %lu Hz, bpc=%u, colorspace=%d, maxLum=%.0f, HwCompositionSupport=0x%x (FULLSCREEN=%d WINDOWED=%d CURSOR_STRETCHED=%d)\n",
                   od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                   od.DesktopCoordinates.bottom - od.DesktopCoordinates.top, dm.dmDisplayFrequency, od.BitsPerColor,
                   (int)od.ColorSpace, od.MaxLuminance, hwc, !!(hwc & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN),
                   !!(hwc & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED), !!(hwc & DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED));
        }
        ProbeComposition(adapter.Get());
    }
    return 0;
}
