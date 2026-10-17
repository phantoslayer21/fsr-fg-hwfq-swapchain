// pace_test: present a frame-generation-like pattern (M-1 generated frames + 1 real frame per base frame) and log
// when each frame was presented and displayed. Own code; Windows SDK only.
//
//   pace_test --api dxgi --mode sw   --mult 2 [--sync 0|1]   D3D12 DXGI flip swapchain, CPU pacer thread (FSR-style)
//   pace_test --api dxgi11 ...                                as dxgi, but a D3D11 swapchain (needed for IDXGISwapChainMedia
//                                                            present duration; D3D12 swapchains do not expose it)
//   pace_test --api comp --mode sw   --mult 2                Composition Swapchain API, CPU pacer, present at once
//   pace_test --api comp --mode meter --mult 2               Composition Swapchain API, all M frames of a base frame
//                                                            queued at once with SetTargetTime (no CPU wake per frame)
// Other options: --base-hz F (default refresh/mult), --seconds S (6), --load N (busy CPU threads), --jitter MS (game
// frame-time noise, uniform +-MS), --gpu-ms MS (interpolation time before the batch is ready, 0.5), --prio 0|1|2,
// --out file.csv, --spin 1 (pure QPC busy-spin like the FSR swapchain's default, instead of timer + spin),
// --comp-dev 12|11 (comp buffers: D3D12 shared-heap textures, or D3D11 displayable textures opened in D3D12).
// ESC quits. The window is a borderless full-screen popup on the primary monitor.
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <Presentation.h>
#include <wrl/client.h>
#include <realtimeapiset.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                  \
    do {                                                                                          \
        HRESULT hr_ = (x);                                                                        \
        if (FAILED(hr_)) { fprintf(stderr, "FAILED 0x%08lx: %s (line %d)\n", hr_, #x, __LINE__); ExitProcess(2); } \
    } while (0)

struct Options {
    std::string api = "dxgi", mode = "sw", out = "pace.csv";
    int mult = 2, sync = 0, load = 0, prio = 0, buffers = 0, spin = 0, compDev = 12, qpcTarget = 0, noTarget = 0;
    double seconds = 6, base_hz = 0, jitter_ms = 0, gpu_ms = 0.5, duration_ms = 0;
    int mode_hz = 0;  // temporary display refresh for the run (CDS_FULLSCREEN), restored on exit
    int max_latency = 0;  // >0: frame-latency waitable swapchain with SetMaximumFrameLatency(N), wait before each present
    double early_ms = 0;  // sw mode: present this long before each frame's time and let the display hold it
};

static Options g_opt;
static LARGE_INTEGER g_freq;
static HWND g_hwnd;
static UINT g_w, g_h;
static std::atomic<bool> g_quit{false};
static HANDLE g_timer;
struct DurationInfo { unsigned want, smaller, larger, used, hr; };
static DurationInfo g_durationInfo = {};

static int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double ToMs(int64_t d) { return d * 1000.0 / g_freq.QuadPart; }
static int64_t FromMs(double m) { return (int64_t)llround(m * g_freq.QuadPart / 1000.0); }

// Hybrid wait like a frame-generation presenter: high-resolution waitable timer until ~0.75 ms before, then spin.
static void WaitUntil(int64_t t)
{
    for (;;) {
        int64_t rem = t - Qpc();
        if (rem <= 0) return;
        double remMs = ToMs(rem);
        if (remMs > 1.0 && !g_opt.spin) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((remMs - 0.75) * 10000.0);
            SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(g_timer, INFINITE);
        } else {
            YieldProcessor();
        }
    }
}

struct PresentRow {
    int j, k, i, real;
    int64_t sched, call, ret;  // QPC: scheduled display time, before Present, after Present
    UINT64 id;                 // comp: present id; dxgi: GetLastPresentCount after the call
    double waitErrMs;
};
struct StatRow {
    UINT64 id; int kind; int status; UINT64 compFrame; UINT64 displayed; UINT64 duration; int64_t seen;
};
struct DxgiStatRow {
    int j; UINT presentCount, presentRefresh, syncRefresh; int64_t syncQpc; int compMode; UINT approvedDuration;
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { g_quit = true; return 0; }
    if (m == WM_DESTROY) { g_quit = true; PostQuitMessage(0); return 0; }
    if (m == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
    return DefWindowProcW(h, m, w, l);
}

// ---- D3D12 plumbing -------------------------------------------------------------------------------------------
struct Gpu {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> alloc[16];
    UINT64 allocFence[16] = {};
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvSize = 0;
    int allocIdx = 0;

    void WaitFence(UINT64 v)
    {
        if (fence->GetCompletedValue() >= v) return;
        fence->SetEventOnCompletion(v, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    // Clear target to a background and draw a moving bar. Returns the fence value of the work.
    UINT64 Render(ID3D12Resource* target, UINT rtvIndex, int j, bool real)
    {
        int a = allocIdx;
        allocIdx = (allocIdx + 1) % 16;
        WaitFence(allocFence[a]);
        alloc[a]->Reset();
        list->Reset(alloc[a].Get(), nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)rtvIndex * rtvSize;
        const float bg[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        const float fgReal[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
        const float fgGen[4] = { 0.55f, 0.75f, 0.95f, 1.0f };
        list->ClearRenderTargetView(rtv, bg, 0, nullptr);
        LONG x = (LONG)((j * 24) % (int)g_w);
        D3D12_RECT r = { x, (LONG)g_h / 3, x + 48, (LONG)(2 * g_h / 3) };
        list->ClearRenderTargetView(rtv, real ? fgReal : fgGen, 1, &r);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        allocFence[a] = fenceValue;
        return fenceValue;
    }
};

static void InitGpu(Gpu& g, IDXGIAdapter1* adapter, UINT rtvCount)
{
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)));
    for (auto& a : g.alloc) CHECK(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    CHECK(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0].Get(), nullptr, IID_PPV_ARGS(&g.list)));
    g.list->Close();
    CHECK(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvCount };
    CHECK(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)));
    g.rtvSize = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

static void MakeRtv(Gpu& g, ID3D12Resource* r, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * g.rtvSize;
    g.dev->CreateRenderTargetView(r, nullptr, h);
}

// ---- schedule ------------------------------------------------------------------------------------------------
struct Schedule {
    int64_t tOut;      // output frame period (QPC)
    int64_t tBase;     // base (real) frame period
    int64_t t0;        // ideal display time of output frame 0
    int64_t gpu;       // interpolation time before a batch is ready
    std::mt19937 rng{1234};
    int64_t lastSched = 0;
    // Real frame k reaches the frame-generation presenter at Arrival(k); its batch is ready at Arrival(k)+gpu.
    int64_t Arrival(int k)
    {
        double jit = g_opt.jitter_ms > 0 ? std::uniform_real_distribution<double>(-g_opt.jitter_ms, g_opt.jitter_ms)(rng) : 0.0;
        return t0 - gpu + (int64_t)k * tBase + FromMs(jit);
    }
    // FSR-style: the batch starts when it is ready but never sooner than one output period after the last frame.
    int64_t BatchStart(int64_t ready) { return lastSched ? std::max(ready, lastSched + tOut) : ready; }
};

static bool Available(IPresentationBuffer* b)
{
    boolean a = false;
    return SUCCEEDED(b->IsAvailable(&a)) && a;
}

static void LoadThread()
{
    volatile double x = 1.0;
    while (!g_quit) for (int i = 0; i < 100000; ++i) x = x * 1.0000001 + 1e-9;
}

int wmain(int argc, wchar_t** argv)
{
    for (int a = 1; a < argc; ++a) {
        std::wstring k = argv[a];
        auto next = [&]() { return a + 1 < argc ? std::wstring(argv[++a]) : std::wstring(); };
        auto nstr = [&]() { std::wstring v = next(); return std::string(v.begin(), v.end()); };
        if (k == L"--api") g_opt.api = nstr();
        else if (k == L"--mode") g_opt.mode = nstr();
        else if (k == L"--out") g_opt.out = nstr();
        else if (k == L"--mult") g_opt.mult = _wtoi(next().c_str());
        else if (k == L"--sync") g_opt.sync = _wtoi(next().c_str());
        else if (k == L"--load") g_opt.load = _wtoi(next().c_str());
        else if (k == L"--prio") g_opt.prio = _wtoi(next().c_str());
        else if (k == L"--buffers") g_opt.buffers = _wtoi(next().c_str());
        else if (k == L"--spin") g_opt.spin = _wtoi(next().c_str());
        else if (k == L"--comp-dev") g_opt.compDev = _wtoi(next().c_str());
        else if (k == L"--target-qpc") g_opt.qpcTarget = _wtoi(next().c_str());
        else if (k == L"--no-target") g_opt.noTarget = _wtoi(next().c_str());
        else if (k == L"--duration-ms") g_opt.duration_ms = _wtof(next().c_str());
        else if (k == L"--mode-hz") g_opt.mode_hz = _wtoi(next().c_str());
        else if (k == L"--max-latency") g_opt.max_latency = _wtoi(next().c_str());
        else if (k == L"--early-ms") g_opt.early_ms = _wtof(next().c_str());
        else if (k == L"--seconds") g_opt.seconds = _wtof(next().c_str());
        else if (k == L"--base-hz") g_opt.base_hz = _wtof(next().c_str());
        else if (k == L"--jitter") g_opt.jitter_ms = _wtof(next().c_str());
        else if (k == L"--gpu-ms") g_opt.gpu_ms = _wtof(next().c_str());
        else { fwprintf(stderr, L"unknown option %s\n", k.c_str()); return 1; }
    }
    if (g_opt.api == "none") {
        // Display-free run: only the software pacer's wake-up accuracy (hybrid timer + spin), at the output rate.
        QueryPerformanceFrequency(&g_freq);
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::vector<std::thread> loads;
        for (int l = 0; l < g_opt.load; ++l) loads.emplace_back(LoadThread);
        if (g_opt.prio == 1) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        if (g_opt.prio == 2) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const double outHz = (g_opt.base_hz > 0 ? g_opt.base_hz : 60.0) * g_opt.mult;
        const int64_t tOut = FromMs(1000.0 / outHz);
        const int n = (int)(g_opt.seconds * outHz);
        std::vector<double> err(n);
        int64_t t = Qpc() + FromMs(50);
        for (int j = 0; j < n; ++j, t += tOut) { WaitUntil(t); err[j] = ToMs(Qpc() - t); }
        g_quit = true;
        for (auto& th : loads) th.join();
        FILE* f = fopen(g_opt.out.c_str(), "w");
        fprintf(f, "# api=none mult=%d out_hz=%.3f load=%d prio=%d\nj,wait_err_ms\n", g_opt.mult, outHz, g_opt.load, g_opt.prio);
        for (int j = 0; j < n; ++j) fprintf(f, "%d,%.4f\n", j, err[j]);
        fclose(f);
        printf("wrote %s: %d waits at %.1f Hz\n", g_opt.out.c_str(), n, outHz);
        return 0;
    }
    const bool comp = g_opt.api == "comp";
    const bool dx11 = g_opt.api == "dxgi11";
    const bool meter = g_opt.mode == "meter";
    if (meter && !comp) { fprintf(stderr, "--mode meter needs --api comp\n"); return 1; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    if (g_opt.mode_hz) {
        // Temporary mode change for this process only; Windows also restores the registry mode when we exit.
        DEVMODEW dm = { sizeof(dm) };
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
        dm.dmDisplayFrequency = (DWORD)g_opt.mode_hz;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        LONG r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        fprintf(stderr, "ChangeDisplaySettingsEx(%d Hz, CDS_FULLSCREEN) = %ld\n", g_opt.mode_hz, r);
        if (r != DISP_CHANGE_SUCCESSFUL) return 4;
        Sleep(1500);  // let the mode settle
    }
    // Primary monitor and its refresh timing from DWM.
    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    g_w = mi.rcMonitor.right - mi.rcMonitor.left;
    g_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DWM_TIMING_INFO ti = { sizeof(ti) };
    CHECK(DwmGetCompositionTimingInfo(nullptr, &ti));
    const int64_t tRef = (int64_t)ti.qpcRefreshPeriod;
    const double refreshHz = (double)g_freq.QuadPart / tRef;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"pace_test";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(comp ? WS_EX_NOREDIRECTIONBITMAP : 0, wc.lpszClassName, L"pace_test", WS_POPUP | WS_VISIBLE,
                             mi.rcMonitor.left, mi.rcMonitor.top, g_w, g_h, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(g_hwnd);

    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));

    const int nBuf = g_opt.buffers ? g_opt.buffers : (comp ? 2 * g_opt.mult + 4 : 4);
    Gpu gpu;
    InitGpu(gpu, adapter.Get(), nBuf);

    // Presentation objects.
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<IDXGISwapChainMedia> media;
    HANDLE latencyWait = nullptr;
    auto SetupLatency = [&]() {
        if (!g_opt.max_latency) return;
        ComPtr<IDXGISwapChain2> s2;
        CHECK(swap.As(&s2));
        CHECK(s2->SetMaximumFrameLatency((UINT)g_opt.max_latency));
        latencyWait = s2->GetFrameLatencyWaitableObject();
    };
    // Returns the time spent waiting (ms) for the swapchain's frame-latency budget.
    auto WaitLatency = [&]() -> double {
        if (!latencyWait) return 0.0;
        int64_t w0 = Qpc();
        WaitForSingleObjectEx(latencyWait, 1000, TRUE);
        return ToMs(Qpc() - w0);
    };
    std::vector<ComPtr<ID3D12Resource>> bufs(nBuf);
    ComPtr<IPresentationFactory> pfac;
    ComPtr<IPresentationManager> mgr;
    ComPtr<IPresentationSurface> surf;
    std::vector<ComPtr<IPresentationBuffer>> pbufs(nBuf);
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<ID3D11Device> d11;
    std::vector<ComPtr<ID3D11Texture2D>> tex11(nBuf);
    HANDLE statsEvent = nullptr;
    ComPtr<ID3D11DeviceContext1> ctx11;
    auto render11 = [&](int j, bool real) {
        ComPtr<ID3D11Texture2D> bb;
        swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        ComPtr<ID3D11RenderTargetView> rtv;
        d11->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
        const float bg[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        const float fgReal[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
        const float fgGen[4] = { 0.55f, 0.75f, 0.95f, 1.0f };
        ctx11->ClearView(rtv.Get(), bg, nullptr, 0);
        LONG x = (LONG)((j * 24) % (int)g_w);
        D3D11_RECT r = { x, (LONG)g_h / 3, x + 48, (LONG)(2 * g_h / 3) };
        ctx11->ClearView(rtv.Get(), real ? fgReal : fgGen, &r, 1);
    };
    if (dx11) {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
        ComPtr<ID3D11DeviceContext> c;
        CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                D3D11_SDK_VERSION, &d11, nullptr, &c));
        CHECK(c.As(&ctx11));
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = g_w; sd.Height = g_h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = nBuf; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                   (g_opt.max_latency ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
        ComPtr<IDXGISwapChain1> sc1;
        CHECK(factory->CreateSwapChainForHwnd(d11.Get(), g_hwnd, &sd, nullptr, nullptr, &sc1));
        CHECK(sc1.As(&swap));
        CHECK(swap.As(&media));
        SetupLatency();
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    } else if (!comp) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = g_w; sd.Height = g_h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = nBuf; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                   (g_opt.max_latency ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
        ComPtr<IDXGISwapChain1> sc1;
        CHECK(factory->CreateSwapChainForHwnd(gpu.queue.Get(), g_hwnd, &sd, nullptr, nullptr, &sc1));
        CHECK(sc1.As(&swap));
        swap.As(&media);
        SetupLatency();
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        for (int b = 0; b < nBuf; ++b) { CHECK(swap->GetBuffer(b, IID_PPV_ARGS(&bufs[b]))); MakeRtv(gpu, bufs[b].Get(), b); }
    } else {
        if (g_opt.compDev == 11) {
            // Microsoft's documented route: a D3D11 device without internal threading optimizations owns the
            // presentation factory, and its buffers are displayable D3D11 textures. D3D12 renders into them via NT handles.
            D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
            CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
                                    &fl, 1, D3D11_SDK_VERSION, &d11, nullptr, nullptr));
            CHECK(CreatePresentationFactory(d11.Get(), IID_PPV_ARGS(&pfac)));
        } else {
            CHECK(CreatePresentationFactory(gpu.dev.Get(), IID_PPV_ARGS(&pfac)));
        }
        if (!pfac->IsPresentationSupportedWithIndependentFlip()) fprintf(stderr, "warning: no independent flip support\n");
        CHECK(pfac->CreatePresentationManager(&mgr));
        HANDLE surfHandle;
        CHECK(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surfHandle));
        CHECK(mgr->CreatePresentationSurface(surfHandle, &surf));
        CHECK(DCompositionCreateDevice3(nullptr, IID_PPV_ARGS(&dcomp)));
        CHECK(dcomp->CreateTargetForHwnd(g_hwnd, TRUE, &dtarget));
        CHECK(dcomp->CreateVisual(&visual));
        ComPtr<IUnknown> content;
        CHECK(dcomp->CreateSurfaceFromHandle(surfHandle, &content));
        CHECK(visual->SetContent(content.Get()));
        CHECK(dtarget->SetRoot(visual.Get()));
        CHECK(dcomp->Commit());
        for (int b = 0; b < nBuf && g_opt.compDev == 11; ++b) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
            CHECK(d11->CreateTexture2D(&td, nullptr, &tex11[b]));
            CHECK(mgr->AddBufferFromResource(tex11[b].Get(), &pbufs[b]));
            ComPtr<IDXGIResource1> dr;
            CHECK(tex11[b].As(&dr));
            HANDLE h = nullptr;
            CHECK(dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h));
            CHECK(gpu.dev->OpenSharedHandle(h, IID_PPV_ARGS(&bufs[b])));
            CloseHandle(h);
            MakeRtv(gpu, bufs[b].Get(), b);
        }
        for (int b = 0; b < nBuf && g_opt.compDev != 11; ++b) {
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = g_w; rd.Height = g_h; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            CHECK(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&bufs[b])));
            CHECK(mgr->AddBufferFromResource(bufs[b].Get(), &pbufs[b]));
            MakeRtv(gpu, bufs[b].Get(), b);
        }
        CHECK(surf->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE));
        RECT src = { 0, 0, (LONG)g_w, (LONG)g_h };
        CHECK(surf->SetSourceRect(&src));
        CHECK(mgr->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus, true));
        CHECK(mgr->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame, true));
        CHECK(mgr->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame, true));
        CHECK(mgr->GetPresentStatisticsAvailableEvent(&statsEvent));
    }

    std::vector<PresentRow> rows;
    std::vector<StatRow> stats;
    std::vector<DxgiStatRow> dstats;
    rows.reserve(100000);
    // Interrupt time (100 ns) <-> QPC offset, for SetTargetTime and the displayed times of the statistics.
    auto interruptNow = []() { ULONGLONG t; QueryInterruptTimePrecise(&t); return t; };
    const double qpcPer100ns = g_freq.QuadPart / 1e7;
    int64_t q0 = Qpc();
    ULONGLONG i0 = interruptNow();
    int64_t q1 = Qpc();
    q0 = (q0 + q1) / 2;
    auto toInterrupt = [&](int64_t q) { return (UINT64)((double)i0 + (q - q0) / qpcPer100ns); };

    auto drainStats = [&]() {
        if (!mgr) return;
        for (;;) {
            ComPtr<IPresentStatistics> s;
            if (FAILED(mgr->GetNextPresentStatistics(&s)) || !s) break;
            StatRow r = { s->GetPresentId(), (int)s->GetKind(), -1, 0, 0, 0, Qpc() };
            ComPtr<IPresentStatusPresentStatistics> ps;
            ComPtr<ICompositionFramePresentStatistics> cs;
            ComPtr<IIndependentFlipFramePresentStatistics> fs;
            if (SUCCEEDED(s.As(&ps))) { r.status = (int)ps->GetPresentStatus(); r.compFrame = ps->GetCompositionFrameId(); }
            if (SUCCEEDED(s.As(&cs))) { r.compFrame = cs->GetCompositionFrameId(); }
            if (SUCCEEDED(s.As(&fs))) { r.displayed = fs->GetDisplayedTime().value; r.duration = fs->GetPresentDuration().value; }
            stats.push_back(r);
        }
    };

    std::atomic<bool> done{false};
    std::thread presenter([&]() {
        if (g_opt.prio == 1) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        if (g_opt.prio == 2) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const int M = g_opt.mult;
        const double baseHz = g_opt.base_hz > 0 ? g_opt.base_hz : refreshHz / M;
        Schedule sch;
        sch.tBase = FromMs(1000.0 / baseHz);
        sch.tOut = sch.tBase / M;
        sch.gpu = FromMs(g_opt.gpu_ms);
        // Warm up 30 frames with plain presents so independent flip engages, then anchor frame 0 half a refresh
        // before a vblank, about 0.5 s from now.
        int buf = 0;
        for (int w = 0; w < 30 && !g_quit; ++w) {
            if (!comp) {
                WaitLatency();
                if (dx11) render11(0, true);
                else { buf = swap->GetCurrentBackBufferIndex(); gpu.Render(bufs[buf].Get(), buf, 0, true); }
                swap->Present(1, 0);
            } else {
                while (!Available(pbufs[buf].Get())) Sleep(1);
                gpu.WaitFence(gpu.Render(bufs[buf].Get(), buf, 0, true));
                surf->SetBuffer(pbufs[buf].Get());
                mgr->Present();
                buf = (buf + 1) % nBuf;
                Sleep(15);
                drainStats();
            }
        }
        stats.clear();
        if (g_opt.duration_ms > 0) {
            UINT want = (UINT)llround(g_opt.duration_ms * 10000.0);
            if (!comp && media) {
                UINT smaller = 0, larger = 0;
                HRESULT hr = media->CheckPresentDurationSupport(want, &smaller, &larger);
                UINT use = (smaller == want || larger == want) ? want : (larger ? larger : smaller);
                HRESULT hr2 = use ? media->SetPresentDuration(use) : E_FAIL;
                fprintf(stderr, "CheckPresentDurationSupport(%u) hr=0x%08lx smaller=%u larger=%u -> SetPresentDuration(%u) hr=0x%08lx\n",
                        want, hr, smaller, larger, use, hr2);
                g_durationInfo = { want, smaller, larger, use, (unsigned)hr2 };
            } else if (comp) {
                HRESULT hr = mgr->SetPreferredPresentDuration({ want }, { want / 10 });
                fprintf(stderr, "SetPreferredPresentDuration(%u, tol %u) hr=0x%08lx\n", want, want / 10, hr);
                g_durationInfo = { want, 0, 0, want, (unsigned)hr };
            }
        }
        DWM_TIMING_INFO t2 = { sizeof(t2) };
        DwmGetCompositionTimingInfo(nullptr, &t2);
        int64_t vb = (int64_t)t2.qpcVBlank;
        int64_t now = Qpc();
        int64_t n = (now + FromMs(500) - vb) / tRef + 1;
        sch.t0 = vb + n * tRef - tRef / 2;
        const int batches = (int)(g_opt.seconds * baseHz);
        int j = 0;
        for (int k = 0; k < batches && !g_quit; ++k) {
            int64_t ready = sch.Arrival(k) + sch.gpu;
            WaitUntil(ready);  // the game's real frame k has arrived and the generated frames are interpolated
            int64_t start = sch.BatchStart(ready);
            if (comp && meter) {
                // Render all M frames, then queue them with target times. No CPU wake-up per output frame.
                std::vector<int> used;
                UINT64 fv = 0;
                for (int i = 0; i < M; ++i) {
                    while (!Available(pbufs[buf].Get())) { drainStats(); Sleep(0); }
                    fv = gpu.Render(bufs[buf].Get(), buf, j + i, i == M - 1);
                    used.push_back(buf);
                    buf = (buf + 1) % nBuf;
                }
                gpu.WaitFence(fv);
                for (int i = 0; i < M; ++i) {
                    int64_t s = start + (int64_t)i * sch.tOut;
                    PresentRow r = { j, k, i, i == M - 1, s, Qpc(), 0, mgr->GetNextPresentId(), 0.0 };
                    surf->SetBuffer(pbufs[used[i]].Get());
                    mgr->SetTargetTime({ g_opt.noTarget ? 0ull : (g_opt.qpcTarget ? (UINT64)s : toInterrupt(s)) });
                    HRESULT hr = mgr->Present();
                    r.ret = Qpc();
                    if (FAILED(hr)) fprintf(stderr, "Present failed 0x%08lx\n", hr);
                    rows.push_back(r);
                    ++j;
                }
                sch.lastSched = start + (int64_t)(M - 1) * sch.tOut;
                drainStats();
            } else {
                for (int i = 0; i < M && !g_quit; ++i) {
                    int64_t s = start + (int64_t)i * sch.tOut;
                    // Present early enough to make the vblank at or after s (vsync) or at s (tearing / immediate).
                    // --mode queue: no per-frame wait; the batch goes out back to back and the display spaces it.
                    if (g_opt.mode != "queue") WaitUntil(s - FromMs(g_opt.early_ms));
                    double werr = ToMs(Qpc() - s);
                    PresentRow r = { j, k, i, i == M - 1, s, 0, 0, 0, werr };
                    if (!comp) {
                        if (latencyWait) r.waitErrMs = WaitLatency();  // with --max-latency the column holds the wait
                        if (dx11) render11(j, i == M - 1);
                        else { buf = swap->GetCurrentBackBufferIndex(); gpu.Render(bufs[buf].Get(), buf, j, i == M - 1); }
                        r.call = Qpc();
                        HRESULT hr = swap->Present(g_opt.sync, g_opt.sync == 0 ? DXGI_PRESENT_ALLOW_TEARING : 0);
                        r.ret = Qpc();
                        if (FAILED(hr)) fprintf(stderr, "Present failed 0x%08lx\n", hr);
                        swap->GetLastPresentCount((UINT*)&r.id);
                        DXGI_FRAME_STATISTICS_MEDIA fm = {};
                        int cm = -1;
                        if (media && SUCCEEDED(media->GetFrameStatisticsMedia(&fm))) cm = (int)fm.CompositionMode;
                        dstats.push_back({ j, fm.PresentCount, fm.PresentRefreshCount, fm.SyncRefreshCount, fm.SyncQPCTime.QuadPart, cm,
                                           fm.ApprovedPresentDuration });
                    } else {
                        while (!Available(pbufs[buf].Get())) { drainStats(); Sleep(0); }
                        gpu.WaitFence(gpu.Render(bufs[buf].Get(), buf, j, i == M - 1));
                        surf->SetBuffer(pbufs[buf].Get());
                        r.id = mgr->GetNextPresentId();
                        r.call = Qpc();
                        HRESULT hr = mgr->Present();
                        r.ret = Qpc();
                        if (FAILED(hr)) fprintf(stderr, "Present failed 0x%08lx\n", hr);
                        buf = (buf + 1) % nBuf;
                        drainStats();
                    }
                    rows.push_back(r);
                    ++j;
                }
                sch.lastSched = start + (int64_t)(M - 1) * sch.tOut;
            }
        }
        // Let queued presents retire, then collect the remaining statistics.
        int64_t endWait = Qpc() + FromMs(300);
        while (Qpc() < endWait) { drainStats(); Sleep(5); }
        drainStats();
        done = true;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    });

    std::vector<std::thread> loads;
    for (int l = 0; l < g_opt.load; ++l) loads.emplace_back(LoadThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_CLOSE) DestroyWindow(g_hwnd);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (done) DestroyWindow(g_hwnd);
    }
    g_quit = true;
    presenter.join();
    if (g_opt.mode_hz) ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);  // back to the registry mode
    for (auto& t : loads) t.join();
    gpu.WaitFence(gpu.fenceValue);

    FILE* f = fopen(g_opt.out.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", g_opt.out.c_str()); return 1; }
    fprintf(f, "# api=%s mode=%s mult=%d sync=%d load=%d prio=%d spin=%d comp_dev=%d target_qpc=%d max_latency=%d mode_hz=%d early_ms=%.2f jitter_ms=%.3f gpu_ms=%.3f buffers=%d size=%ux%u refresh_hz=%.4f qpc_freq=%lld qpc0=%lld interrupt0=%llu\n",
            g_opt.api.c_str(), g_opt.mode.c_str(), g_opt.mult, g_opt.sync, g_opt.load, g_opt.prio, g_opt.spin, g_opt.compDev, g_opt.qpcTarget, g_opt.max_latency, g_opt.mode_hz, g_opt.early_ms,
            g_opt.jitter_ms, g_opt.gpu_ms, nBuf,
            g_w, g_h, refreshHz, g_freq.QuadPart, q0, i0);
    fprintf(f, "type,j,k,i,real,sched_qpc,call_qpc,ret_qpc,id,wait_err_ms\n");
    for (auto& r : rows)
        fprintf(f, "P,%d,%d,%d,%d,%lld,%lld,%lld,%llu,%.4f\n", r.j, r.k, r.i, r.real, r.sched, r.call, r.ret, r.id, r.waitErrMs);
    fprintf(f, "type,id,kind,status,comp_frame,displayed_100ns,duration_100ns,seen_qpc\n");
    for (auto& s : stats)
        fprintf(f, "S,%llu,%d,%d,%llu,%llu,%llu,%lld\n", s.id, s.kind, s.status, s.compFrame, s.displayed, s.duration, s.seen);
    fprintf(f, "# duration want=%u smaller=%u larger=%u used=%u hr=0x%08x mode=%s no_target=%d\n", g_durationInfo.want,
            g_durationInfo.smaller, g_durationInfo.larger, g_durationInfo.used, g_durationInfo.hr, g_opt.mode.c_str(), g_opt.noTarget);
    fprintf(f, "type,j,present_count,present_refresh,sync_refresh,sync_qpc,comp_mode,approved_duration\n");
    for (auto& d : dstats)
        fprintf(f, "D,%d,%u,%u,%u,%lld,%d,%u\n", d.j, d.presentCount, d.presentRefresh, d.syncRefresh, d.syncQpc, d.compMode,
                d.approvedDuration);
    fclose(f);
    printf("wrote %s: %zu presents, %zu stats, refresh %.3f Hz\n", g_opt.out.c_str(), rows.size(), stats.size(), refreshHz);
    timeEndPeriod(1);
    return 0;
}
