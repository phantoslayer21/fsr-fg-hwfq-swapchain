// fg_probe: off-screen check that AMD's frame-generation context (ML FG in amd_fidelityfx_framegeneration_dx12.dll)
// produces interpolated frames when driven through the public FFX API without AMD's swapchain:
// configure with FG_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY, then ffxDispatch(DISPATCH_FRAMEGENERATION) ourselves.
// Scene: a white bar moving +16 px per frame on dark grey; motion vectors constant; depth constant.
// Usage: fg_probe <path to FFX dll exporting ffxCreateContext...> [frames]   (no window, nothing presented)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <map>
#include "ffx_abi.h"

using Microsoft::WRL::ComPtr;
#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("FAILED 0x%08lx %s line %d\n", h_, #x, __LINE__); return 2; } } while (0)

static void Msg(uint32_t type, const wchar_t* m) { wprintf(L"  [ffx %s] %s\n", type == 0 ? L"error" : L"warn", m); }

static const UINT W = 1280, H = 720;
static const int SPEED = 16, BAR = 40;

struct Gpu {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = { list.Get() };
        q->ExecuteCommandLists(1, l);
        q->Signal(fence.Get(), ++fv);
        fence->SetEventOnCompletion(fv, ev);
        WaitForSingleObject(ev, INFINITE);
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
    }
};

static ComPtr<ID3D12Resource> Tex(ID3D12Device* d, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = f; rd.SampleDesc.Count = 1; rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
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

// Bar position: centre column of bright pixels on the middle row of an RGBA8 image.
static double BarCentre(const uint8_t* px, UINT pitch)
{
    const uint8_t* row = px + (size_t)pitch * (H / 2);
    double sum = 0, n = 0;
    for (UINT x = 0; x < W; ++x) {
        int v = row[x * 4 + 0];
        if (v > 128) { sum += x * (v / 255.0); n += v / 255.0; }
    }
    return n > 0 ? sum / n : -1;
}

int main(int argc, char** argv)
{
    const char* dll = argc > 1 ? argv[1] : "amd_fidelityfx_framegeneration_dx12.dll";
    const int frames = argc > 2 ? atoi(argv[2]) : 12;
    HMODULE m = LoadLibraryA(dll);
    if (!m) { printf("LoadLibrary(%s) failed %lu\n", dll, GetLastError()); return 1; }
    auto create = (ffx::PfnCreateContext)GetProcAddress(m, "ffxCreateContext");
    auto destroy = (ffx::PfnDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    auto configure = (ffx::PfnConfigure)GetProcAddress(m, "ffxConfigure");
    auto query = (ffx::PfnQuery)GetProcAddress(m, "ffxQuery");
    auto dispatch = (ffx::PfnDispatch)GetProcAddress(m, "ffxDispatch");
    if (!create || !configure || !dispatch) { printf("missing exports\n"); return 1; }

    ffx::ConfigureGlobalDebug1 dbg = { { ffx::CONFIGURE_GLOBALDEBUG1, nullptr }, Msg, 2 };
    printf("global debug configure rc=%u\n", configure(nullptr, &dbg.header));

    Gpu g;
    ComPtr<IDXGIFactory6> f;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)));
    ComPtr<IDXGIAdapter1> a;
    CHECK(f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)));
    CHECK(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)));
    CHECK(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)));
    CHECK(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)));
    CHECK(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));

    // Resources: two colour frames (ping-pong), depth, motion vectors, FG output, readback.
    const auto RT = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> color[2] = { Tex(g.dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, RT, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                        Tex(g.dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, RT, D3D12_RESOURCE_STATE_RENDER_TARGET) };
    ComPtr<ID3D12Resource> depth = Tex(g.dev.Get(), DXGI_FORMAT_R32_FLOAT, RT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12Resource> mv = Tex(g.dev.Get(), DXGI_FORMAT_R16G16_FLOAT, RT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12Resource> out = Tex(g.dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4 };
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    CHECK(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc; return h; };
    g.dev->CreateRenderTargetView(color[0].Get(), nullptr, rtv(0));
    g.dev->CreateRenderTargetView(color[1].Get(), nullptr, rtv(1));
    g.dev->CreateRenderTargetView(depth.Get(), nullptr, rtv(2));
    g.dev->CreateRenderTargetView(mv.Get(), nullptr, rtv(3));
    D3D12_RESOURCE_DESC od = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 rbSize = 0;
    g.dev->GetCopyableFootprints(&od, 0, 1, 0, &fp, nullptr, nullptr, &rbSize);
    D3D12_HEAP_PROPERTIES rh = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = rbSize * 2; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    CHECK(g.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)));

    std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> st = { { color[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
        { color[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET }, { depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
        { mv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET }, { out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
    auto To = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES s2) { Barrier(g.list.Get(), r, st[r], s2); st[r] = s2; };

    // Frame-generation context (whatever provider the DLL picks by default; ML on RDNA 4 is expected).
    ffx::CreateFrameGenerationVersion ver = { { ffx::CREATE_FRAMEGENERATION_VERSION, nullptr }, ffx::FRAMEGENERATION_VERSION };
    ffx::CreateBackendDX12 be = { { ffx::CREATE_BACKEND_DX12, &ver.header }, g.dev.Get() };
    ffx::CreateFrameGeneration cfg = { { ffx::CREATE_FRAMEGENERATION, &be.header }, 0, { W, H }, { W, H }, ffx::FMT_R8G8B8A8_UNORM };
    ffx::Context ctx = nullptr;
    ffx::ReturnCode rc = create(&ctx, &cfg.header, nullptr);
    printf("ffxCreateContext(FRAMEGENERATION) rc=%u ctx=%p\n", rc, ctx);
    if (rc) return 3;
    ffx::QueryGetProviderVersion pv = { { ffx::QUERY_GET_PROVIDER_VERSION, nullptr }, 0, nullptr };
    rc = query(&ctx, &pv.header);
    printf("provider rc=%u id=0x%llx name=%s\n", rc, (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "?");

    for (int k = 0; k < frames; ++k) {
        ID3D12Resource* cur = color[k & 1].Get();
        int x = 100 + k * SPEED;
        // render frame k: colour, depth, motion vectors (prev = cur + mv, in pixels)
        const float bg[4] = { 0.1f, 0.1f, 0.1f, 1 }, fg[4] = { 1, 1, 1, 1 }, dz[4] = { 0.5f, 0, 0, 0 }, mvv[4] = { -(float)SPEED, 0, 0, 0 };
        D3D12_RECT bar = { x, 0, x + BAR, (LONG)H };
        To(cur, D3D12_RESOURCE_STATE_RENDER_TARGET);
        To(depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        To(mv.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        g.list->ClearRenderTargetView(rtv(k & 1), bg, 0, nullptr);
        g.list->ClearRenderTargetView(rtv(k & 1), fg, 1, &bar);
        g.list->ClearRenderTargetView(rtv(2), dz, 0, nullptr);
        g.list->ClearRenderTargetView(rtv(3), mvv, 0, nullptr);
        const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        To(cur, SRV);
        To(depth.Get(), SRV);
        To(mv.Get(), SRV);
        To(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ffx::DispatchFrameGenerationPrepareV2 prep = {};
        prep.header.type = ffx::DISPATCH_FRAMEGENERATION_PREPARE_V2;
        prep.frameID = (uint64_t)k;
        prep.commandList = g.list.Get();
        prep.renderSize = { W, H };
        prep.motionVectorScale = { 1, 1 };
        prep.frameTimeDelta = 16.667f;
        prep.reset = k == 0;
        prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = ffx::MakeResource(depth.Get(), ffx::STATE_PIXEL_COMPUTE_READ);
        prep.motionVectors = ffx::MakeResource(mv.Get(), ffx::STATE_PIXEL_COMPUTE_READ);
        prep.cameraUp[1] = 1; prep.cameraRight[0] = 1; prep.cameraForward[2] = 1;
        rc = dispatch(&ctx, &prep.header);
        if (rc) printf("frame %d prepare rc=%u\n", k, rc);

        ffx::ConfigureFrameGeneration c = {};
        c.header.type = ffx::CONFIGURE_FRAMEGENERATION;
        c.swapChain = nullptr;
        c.frameGenerationEnabled = true;
        c.flags = ffx::FG_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        c.generationRect = { 0, 0, (int)W, (int)H };
        c.frameID = (uint64_t)k;
        rc = configure(&ctx, &c.header);
        if (rc) printf("frame %d configure rc=%u\n", k, rc);

        ffx::DispatchFrameGeneration d = {};
        d.header.type = ffx::DISPATCH_FRAMEGENERATION;
        d.commandList = g.list.Get();
        d.presentColor = ffx::MakeResource(cur, ffx::STATE_PIXEL_COMPUTE_READ);
        d.outputs[0] = ffx::MakeResource(out.Get(), ffx::STATE_UNORDERED_ACCESS);
        d.numGeneratedFrames = 1;
        d.reset = k == 0;
        d.backbufferTransferFunction = ffx::TF_SRGB;
        d.minMaxLuminance[0] = 0; d.minMaxLuminance[1] = 1000;
        d.generationRect = { 0, 0, (int)W, (int)H };
        d.frameID = (uint64_t)k;
        rc = dispatch(&ctx, &d.header);
        if (rc) printf("frame %d FG dispatch rc=%u\n", k, rc);

        // read back FG output and the current real frame
        To(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        To(cur, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = out.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.pResource = rb.Get(); dst.PlacedFootprint = fp;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        src.pResource = cur;
        dst.PlacedFootprint.Offset = rbSize;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        g.Submit();
        uint8_t* p = nullptr;
        D3D12_RANGE rr = { 0, (SIZE_T)rbSize * 2 };
        rb->Map(0, &rr, (void**)&p);
        double gen = BarCentre(p, fp.Footprint.RowPitch), real = BarCentre(p + rbSize, fp.Footprint.RowPitch);
        rb->Unmap(0, nullptr);
        printf("frame %2d: real bar centre %.1f, previous %.1f, generated %.1f (expect %.1f)\n", k, real, real - SPEED, gen,
               real - SPEED / 2.0);
    }
    if (destroy) destroy(&ctx, nullptr);
    return 0;
}
