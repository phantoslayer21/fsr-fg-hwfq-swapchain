// swapchain.cpp - FgSwapChain implementation (see swapchain.h).
#include "swapchain.h"
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

namespace fgswap {

static const D3D12_RESOURCE_STATES SRV_STATE = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

static void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (!r || a == b) return;
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

static D3D12_RESOURCE_STATES ToD3D12(uint32_t s)
{
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (s & ffx::STATE_UNORDERED_ACCESS) r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & ffx::STATE_COMPUTE_READ) r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & ffx::STATE_PIXEL_READ) r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & ffx::STATE_COPY_SRC) r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & ffx::STATE_COPY_DEST) r |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & ffx::STATE_RENDER_TARGET) r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return r;  // COMMON and PRESENT are both 0
}

static DXGI_FORMAT NonSrgb(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

static DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* d, const D3D12_RESOURCE_DESC& like, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = like;
    rd.Format = fmt;
    rd.Flags = flags;
    rd.MipLevels = 1;
    rd.DepthOrArraySize = 1;
    rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Alignment = 0;
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
HRESULT FgSwapChain::Create(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, FgSwapChain** out)
{
    ComPtr<IDXGIFactory2> f2;
    HRESULT hr = factory->QueryInterface(IID_PPV_ARGS(&f2));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D12Device> dev;
    gameQueue->GetDevice(IID_PPV_ARGS(&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
    ComPtr<ID3D12CommandQueue> pq;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&pq)))) {
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&pq));
        if (FAILED(hr)) return hr;
    }
    pq->SetName(L"fgswap present queue");
    BOOL tearing = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5))))
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    DXGI_SWAP_CHAIN_DESC1 rd = desc;
    rd.BufferCount = std::max(desc.BufferCount, 3u);
    rd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    rd.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
    rd.Flags &= ~(UINT)DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;  // the game's waitable object is ours
    if (tearing) rd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    hr = f2->CreateSwapChainForHwnd(pq.Get(), hwnd, &rd, fs, nullptr, &sc1);
    if (FAILED(hr)) {
        Log("CreateSwapChainForHwnd failed 0x%08lx", hr);
        return hr;
    }
    ComPtr<IDXGISwapChain4> sc4;
    hr = sc1.As(&sc4);
    if (FAILED(hr)) return hr;
    FgSwapChain* s = new FgSwapChain();
    s->gameDesc_ = desc;
    s->realFlags_ = rd.Flags;
    s->tearingSupported_ = tearing != FALSE;
    hr = s->Init(gameQueue, sc4, pq);
    if (FAILED(hr)) { s->Release(); return hr; }
    *out = s;
    return S_OK;
}

HRESULT FgSwapChain::Wrap(IDXGISwapChain4* existing, ID3D12CommandQueue* gameQueue, FgSwapChain** out)
{
    // Re-create the swapchain on our own present queue: take what we need, release the game's handle.
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
    ComPtr<IDXGIFactory> factory;
    existing->GetHwnd(&hwnd);
    existing->GetDesc1(&d1);
    BOOL haveFs = SUCCEEDED(existing->GetFullscreenDesc(&fs));
    existing->GetParent(IID_PPV_ARGS(&factory));
    ULONG left = existing->Release();
    if (left == 0 && hwnd && factory)
        return Create(factory.Get(), gameQueue, hwnd, d1, haveFs ? &fs : nullptr, out);
    // Someone else still holds the original: keep presenting through it on the game's queue.
    existing->AddRef();
    ComPtr<IDXGISwapChain4> keep;
    keep.Attach(existing);
    FgSwapChain* s = new FgSwapChain();
    s->gameDesc_ = d1;
    s->realFlags_ = d1.Flags;
    s->tearingSupported_ = (d1.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    HRESULT hr = s->Init(gameQueue, keep, gameQueue);
    if (FAILED(hr)) { s->Release(); return hr; }
    Log("wrap: original swapchain still referenced (%lu), presenting on the game queue", left);
    *out = s;
    return S_OK;
}

HRESULT FgSwapChain::Init(ID3D12CommandQueue* gameQueue, ComPtr<IDXGISwapChain4> real, ComPtr<ID3D12CommandQueue> presentQueue)
{
    real_ = real;
    gameQueue_ = gameQueue;
    presentQueue_ = presentQueue;
    HRESULT hr = gameQueue->GetDevice(IID_PPV_ARGS(&dev_));
    if (FAILED(hr)) return hr;
    DXGI_SWAP_CHAIN_DESC1 rd;
    real_->GetDesc1(&rd);
    gameDesc_.Width = rd.Width;
    gameDesc_.Height = rd.Height;
    if (gameDesc_.BufferCount == 0) gameDesc_.BufferCount = 2;
    if ((hr = dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gameFence_))) < 0) return hr;
    if ((hr = dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentFence_))) < 0) return hr;
    gameFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    presentFenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (auto& a : presentAllocs_)
        if ((hr = dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) < 0) return hr;
    if ((hr = dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presentAllocs_[0].Get(), nullptr, IID_PPV_ARGS(&presentList_))) < 0) return hr;
    presentList_->Close();
    for (auto& s : slots_)
        if ((hr = dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc))) < 0) return hr;
    if ((hr = dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&gameList_))) < 0) return hr;
    gameList_->Close();
    if ((hr = CreateGameBuffers()) < 0) return hr;
    if ((hr = CreateSlots()) < 0) return hr;
    maxLatency_ = (UINT)Cfg().maxQueuedFrames;
    latencySemaphore_ = CreateSemaphoreW(nullptr, (LONG)maxLatency_, 16, nullptr);
    if (Cfg().presentLog) {
        presentLog_ = _wfopen((Cfg().dir + L"fgswap_presents.csv").c_str(), L"w");
        if (presentLog_) fprintf(presentLog_, "# qpc_freq=%lld\ntype,frame_id,target_qpc,call_qpc,ret_qpc\n", (long long)QpcFreq());
    }
    presenter_ = std::thread([this] { PresenterMain(); });
    Log("swapchain %ux%u fmt %d, game buffers %u, real buffers %u, flags 0x%x, tearing %d, separate present queue %d",
        gameDesc_.Width, gameDesc_.Height, (int)gameDesc_.Format, gameDesc_.BufferCount, rd.BufferCount, realFlags_,
        (int)tearingSupported_, (int)(presentQueue_.Get() != gameQueue_.Get()));
    return S_OK;
}

HRESULT FgSwapChain::CreateGameBuffers()
{
    ComPtr<ID3D12Resource> rb;
    HRESULT hr = real_->GetBuffer(0, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) return hr;
    D3D12_RESOURCE_DESC like = rb->GetDesc();
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (gameDesc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    gameBuffers_.clear();
    for (UINT i = 0; i < gameDesc_.BufferCount; ++i) {
        ComPtr<ID3D12Resource> r = MakeTexture(dev_.Get(), like, like.Format, flags);
        if (!r) return E_OUTOFMEMORY;
        wchar_t n[64];
        swprintf(n, 64, L"fgswap game back buffer %u", i);
        r->SetName(n);
        gameBuffers_.push_back(r);
    }
    gameIndex_ = 0;
    return S_OK;
}

HRESULT FgSwapChain::CreateSlots()
{
    ComPtr<ID3D12Resource> rb;
    HRESULT hr = real_->GetBuffer(0, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) return hr;
    D3D12_RESOURCE_DESC like = rb->GetDesc();
    for (auto& s : slots_) {
        s.realCopy = MakeTexture(dev_.Get(), like, like.Format, D3D12_RESOURCE_FLAG_NONE);
        // UAV-capable generated-frame target; sRGB formats cannot be UAVs, so use the UNORM sibling (copy-compatible).
        s.interp = MakeTexture(dev_.Get(), like, NonSrgb(like.Format), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        s.hudlessCopy.Reset();
        s.freeAt = 0;
        if (!s.realCopy || !s.interp) return E_OUTOFMEMORY;
        s.realCopy->SetName(L"fgswap real frame copy");
        s.interp->SetName(L"fgswap generated frame");
    }
    nextSlot_ = 0;
    return S_OK;
}

void FgSwapChain::ReleaseBuffers()
{
    gameBuffers_.clear();
    for (auto& s : slots_) {
        s.realCopy.Reset();
        s.interp.Reset();
        s.hudlessCopy.Reset();
    }
}

FgSwapChain::~FgSwapChain()
{
    {
        std::unique_lock<std::mutex> l(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (presenter_.joinable()) presenter_.join();
    if (presentFence_ && presentFence_->GetCompletedValue() < presentFenceValue_) {
        presentFence_->SetEventOnCompletion(presentFenceValue_, presentFenceEvent_);
        WaitForSingleObject(presentFenceEvent_, 2000);
    }
    if (gameFence_ && gameFence_->GetCompletedValue() < gameFenceValue_) {
        gameFence_->SetEventOnCompletion(gameFenceValue_, gameFenceEvent_);
        WaitForSingleObject(gameFenceEvent_, 2000);
    }
    if (presentLog_) fclose(presentLog_);
    if (gameFenceEvent_) CloseHandle(gameFenceEvent_);
    if (presentFenceEvent_) CloseHandle(presentFenceEvent_);
    if (latencySemaphore_) CloseHandle(latencySemaphore_);
    Log("swapchain destroyed after %u presents", gamePresentCount_);
}

// ---------------------------------------------------------------------------------------------------------------
HRESULT FgSwapChain::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_FgSwapChain || riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
        riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
        *ppv = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
    }
    return real_->QueryInterface(riid, ppv);  // e.g. IDXGISwapChainMedia
}

ULONG FgSwapChain::AddRef() { return ++refs_; }

ULONG FgSwapChain::Release()
{
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

HRESULT FgSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* p)
{
    HRESULT hr = real_->GetDesc(p);
    if (SUCCEEDED(hr)) { p->BufferCount = gameDesc_.BufferCount; p->Flags = gameDesc_.Flags; }
    return hr;
}

HRESULT FgSwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* p)
{
    HRESULT hr = real_->GetDesc1(p);
    if (SUCCEEDED(hr)) { p->BufferCount = gameDesc_.BufferCount; p->Flags = gameDesc_.Flags; }
    return hr;
}

HRESULT FgSwapChain::GetBuffer(UINT i, REFIID riid, void** pp)
{
    if (i >= gameBuffers_.size()) return DXGI_ERROR_INVALID_CALL;
    return gameBuffers_[i]->QueryInterface(riid, pp);
}

HRESULT FgSwapChain::SetFullscreenState(BOOL f, IDXGIOutput* t)
{
    Drain();
    return real_->SetFullscreenState(f, t);
}

HRESULT FgSwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE c)
{
    HRESULT hr = real_->SetColorSpace1(c);
    if (SUCCEEDED(hr)) colorSpace_ = c;
    return hr;
}

HRESULT FgSwapChain::ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    Drain();
    ReleaseBuffers();
    UINT realCount = count ? std::max(count, 3u) : 0;  // 0 keeps the current count
    UINT rf = (flags & ~(UINT)DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) |
              (tearingSupported_ ? (UINT)DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    HRESULT hr = real_->ResizeBuffers(realCount, w, h, fmt, rf);
    Log("ResizeBuffers(%u, %u x %u, fmt %d, flags 0x%x) = 0x%08lx", count, w, h, (int)fmt, flags, hr);
    if (FAILED(hr)) return hr;
    realFlags_ = rf;
    DXGI_SWAP_CHAIN_DESC1 rd;
    real_->GetDesc1(&rd);
    if (count) gameDesc_.BufferCount = count;
    gameDesc_.Width = rd.Width;
    gameDesc_.Height = rd.Height;
    gameDesc_.Format = rd.Format;
    gameDesc_.Flags = flags;
    if ((hr = CreateGameBuffers()) < 0) return hr;
    if ((hr = CreateSlots()) < 0) return hr;
    resetNext_ = true;
    return S_OK;
}

HRESULT FgSwapChain::ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT*, IUnknown* const*)
{
    return ResizeBuffers(count, w, h, fmt, flags);
}

HRESULT FgSwapChain::SetMaximumFrameLatency(UINT m)
{
    if (m < 1 || m > 16) return DXGI_ERROR_INVALID_CALL;
    maxLatency_ = m;
    return S_OK;
}

HANDLE FgSwapChain::GetFrameLatencyWaitableObject()
{
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), latencySemaphore_, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h;
}

// ---------------------------------------------------------------------------------------------------------------
void FgSwapChain::SetFgConfig(const FgConfig& c)
{
    std::lock_guard<std::mutex> l(cfgMtx_);
    if (!fg_.enabled && c.enabled) resetNext_ = true;
    fg_ = c;
    fgCtxValue_ = c.fgCtx;
}

void FgSwapChain::SetUiResource(const ffx::Resource& r, uint32_t flags)
{
    std::lock_guard<std::mutex> l(cfgMtx_);
    ui_ = r;
    uiFlags_ = flags;
}

ID3D12GraphicsCommandList* FgSwapChain::InterpolationCommandList() { return gameList_.Get(); }

ffx::Resource FgSwapChain::InterpolationTexture()
{
    return ffx::MakeResource(slots_[nextSlot_ % kSlots].interp.Get(), ffx::STATE_UNORDERED_ACCESS);
}

uint64_t FgSwapChain::GpuMemoryBytes() const
{
    uint64_t total = 0;
    auto add = [&](ID3D12Resource* r) {
        if (!r) return;
        D3D12_RESOURCE_DESC d = r->GetDesc();
        total += dev_->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
    };
    for (auto& b : gameBuffers_) add(b.Get());
    for (auto& s : slots_) { add(s.realCopy.Get()); add(s.interp.Get()); add(s.hudlessCopy.Get()); }
    return total;
}

void FgSwapChain::Drain()
{
    std::unique_lock<std::mutex> l(mtx_);
    cvSpace_.wait(l, [&] { return inFlight_ == 0; });
    l.unlock();
    UINT64 v = presentFenceValue_;
    if (presentFence_->GetCompletedValue() < v) {
        presentFence_->SetEventOnCompletion(v, presentFenceEvent_);
        WaitForSingleObject(presentFenceEvent_, INFINITE);
    }
    if (gameFence_->GetCompletedValue() < gameFenceValue_) {
        gameFence_->SetEventOnCompletion(gameFenceValue_, gameFenceEvent_);
        WaitForSingleObject(gameFenceEvent_, INFINITE);
    }
}

void FgSwapChain::WaitForPresents() { Drain(); }

// ---------------------------------------------------------------------------------------------------------------
// Game thread: copy the frame, record the generated frame, hand the batch to the presenter.
HRESULT FgSwapChain::Present(UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return real_->Present(0, DXGI_PRESENT_TEST);
    const int64_t arrival = Qpc();
    ++gamePresentCount_;
    {   // never run further ahead of the presenter than configured
        std::unique_lock<std::mutex> l(mtx_);
        cvSpace_.wait(l, [&] { return inFlight_ < Cfg().maxQueuedFrames || stop_; });
    }
    int si = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % kSlots;
    Slot& s = slots_[si];
    if (presentFence_->GetCompletedValue() < s.freeAt) {
        presentFence_->SetEventOnCompletion(s.freeAt, presentFenceEvent_);
        WaitForSingleObject(presentFenceEvent_, INFINITE);
    }
    FgConfig fg;
    {
        std::lock_guard<std::mutex> l(cfgMtx_);
        fg = fg_;
    }
    const bool fgActive = fg.configured && fg.enabled && (fg.dispatch || fg.fgCallback);
    ID3D12Resource* game = gameBuffers_[gameIndex_].Get();
    s.alloc->Reset();
    gameList_->Reset(s.alloc.Get(), nullptr);
    ID3D12GraphicsCommandList* l = gameList_.Get();

    // real frame copy (the game may reuse its buffer before the presenter shows it)
    Transition(l, game, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(l, s.realCopy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(s.realCopy.Get(), game);
    Transition(l, s.realCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(l, game, D3D12_RESOURCE_STATE_COPY_SOURCE, SRV_STATE);

    bool haveHudless = false;
    if (fgActive) {
        ffx::DispatchFrameGeneration d = {};
        d.header.type = ffx::DISPATCH_FRAMEGENERATION;
        d.commandList = l;
        d.presentColor = fg.hudless.resource ? fg.hudless : ffx::MakeResource(game, ffx::STATE_PIXEL_COMPUTE_READ);
        Transition(l, s.interp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        d.outputs[0] = ffx::MakeResource(s.interp.Get(), ffx::STATE_UNORDERED_ACCESS);
        d.numGeneratedFrames = 1;
        d.reset = resetNext_;
        d.backbufferTransferFunction = colorSpace_ == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? ffx::TF_PQ
                                     : colorSpace_ == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709  ? ffx::TF_SCRGB : ffx::TF_SRGB;
        d.minMaxLuminance[0] = 0.0001f;
        d.minMaxLuminance[1] = 1000.0f;
        d.generationRect = fg.rect;
        if (d.generationRect.width <= 0 || d.generationRect.height <= 0)
            d.generationRect = { 0, 0, (int32_t)gameDesc_.Width, (int32_t)gameDesc_.Height };
        d.frameID = fg.frameID;
        ffx::ReturnCode rc;
        if (fg.fgCallback) rc = fg.fgCallback(&d, fg.fgCallbackCtx);
        else { ffx::Context ctxv = fg.fgCtx; rc = fg.dispatch(&ctxv, &d.header); }
        Transition(l, s.interp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (rc != ffx::RC_OK) Log("frame-generation dispatch rc=%u (frame %llu)", rc, (unsigned long long)fg.frameID);
        resetNext_ = false;
        if (fg.hudless.resource) {  // keep the hudless image for UI extraction on the generated frame
            ID3D12Resource* h = (ID3D12Resource*)fg.hudless.resource;
            if (!s.hudlessCopy) {
                s.hudlessCopy = MakeTexture(dev_.Get(), h->GetDesc(), h->GetDesc().Format, D3D12_RESOURCE_FLAG_NONE);
                if (s.hudlessCopy) s.hudlessCopy->SetName(L"fgswap hudless copy");
            }
            if (s.hudlessCopy) {
                D3D12_RESOURCE_STATES hs = ToD3D12(fg.hudless.state);
                Transition(l, h, hs, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(l, s.hudlessCopy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(s.hudlessCopy.Get(), h);
                Transition(l, s.hudlessCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Transition(l, h, D3D12_RESOURCE_STATE_COPY_SOURCE, hs);
                haveHudless = true;
            }
        }
    }
    Transition(l, game, SRV_STATE, D3D12_RESOURCE_STATE_PRESENT);
    l->Close();
    ID3D12CommandList* lists[] = { l };
    gameQueue_->ExecuteCommandLists(1, lists);
    gameQueue_->Signal(gameFence_.Get(), ++gameFenceValue_);

    Batch b = { si, gameFenceValue_, fgActive, haveHudless, sync, flags, arrival, fg.frameID };
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(b);
        ++inFlight_;
    }
    cv_.notify_one();
    gameIndex_ = (gameIndex_ + 1) % (UINT)gameBuffers_.size();
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Presenter thread.
void FgSwapChain::PresenterMain()
{
    SetThreadPriority(GetCurrentThread(), Cfg().presenterPriority);
    SetThreadDescription(GetCurrentThread(), L"fgswap presenter");
    for (;;) {
        Batch b;
        size_t waiting;
        {
            std::unique_lock<std::mutex> l(mtx_);
            cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            b = queue_.front();
            queue_.pop_front();
            waiting = queue_.size();
        }
        if (gameFence_->GetCompletedValue() < b.gameFenceValue) {
            gameFence_->SetEventOnCompletion(b.gameFenceValue, gameFenceEvent_);
            WaitForSingleObject(gameFenceEvent_, 2000);
        }
        const int64_t ready = Qpc();
        // Smoothed real-frame interval from the game's own Present() times (not from when the presenter got to the
        // batch: that would feed the presenter's own delay back into its schedule and lock the game to it).
        if (lastArrival_) {
            double dt = QpcToMs(b.arrival - lastArrival_);
            if (dt > 0 && dt < 100.0) frameMs_ = frameMs_ > 0 ? frameMs_ * 0.85 + dt * 0.15 : dt;
            else frameMs_ = 0;
        }
        lastArrival_ = b.arrival;
        FgConfig fg;
        {
            std::lock_guard<std::mutex> l(cfgMtx_);
            fg = fg_;
        }
        if (b.fg && frameMs_ > 0) {
            // Aim a hair early so a steady game never builds a backlog; compress harder when batches are already waiting.
            const double squeeze = waiting > 0 ? 0.90 : 0.995;
            const int64_t half = MsToQpc(frameMs_ * 0.5 * squeeze);
            int64_t g = lastRealSlot_ ? std::max(ready, lastRealSlot_ + half) : ready;
            if (g > ready + 2 * half) g = ready;  // far behind: resynchronise instead of carrying latency
            PresentOne(b, true, g);
            if (!fg.onlyPresentGenerated) PresentOne(b, false, g + half);
            lastRealSlot_ = g + half;
        } else {
            PresentOne(b, b.fg && fg.onlyPresentGenerated, ready);
            lastRealSlot_ = ready;
        }
        slots_[b.slot].freeAt = presentFenceValue_;
        {
            std::lock_guard<std::mutex> l(mtx_);
            --inFlight_;
        }
        cvSpace_.notify_all();
        ReleaseSemaphore(latencySemaphore_, 1, nullptr);
    }
}

UINT FgSwapChain::RealPresentFlags(const Batch& b, UINT* sync) const
{
    const Pacing p = Cfg().pacing;
    if (p == Pacing::Early) { *sync = 1; return 0; }
    if (p == Pacing::Queue) { *sync = queueInterval_; return 0; }
    BOOL fullscreen = FALSE;
    real_->GetFullscreenState(&fullscreen, nullptr);
    if (b.sync == 0 && tearingSupported_ && Cfg().allowTearing && !fullscreen) { *sync = 0; return DXGI_PRESENT_ALLOW_TEARING; }
    *sync = b.sync > 1 ? 1 : b.sync;
    return 0;
}

void FgSwapChain::PresentOne(const Batch& b, bool generated, int64_t target)
{
    Slot& s = slots_[b.slot];
    int a = presentAllocIdx_;
    presentAllocIdx_ = (presentAllocIdx_ + 1) % kPresentAllocs;
    if (presentFence_->GetCompletedValue() < presentAllocFence_[a]) {
        presentFence_->SetEventOnCompletion(presentAllocFence_[a], presentFenceEvent_);
        WaitForSingleObject(presentFenceEvent_, INFINITE);
    }
    presentAllocs_[a]->Reset();
    presentList_->Reset(presentAllocs_[a].Get(), nullptr);
    ComPtr<ID3D12Resource> bb;
    real_->GetBuffer(real_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    Compose(presentList_.Get(), generated ? s.interp.Get() : s.realCopy.Get(), bb.Get(), generated, b);
    presentList_->Close();
    ID3D12CommandList* lists[] = { presentList_.Get() };
    presentQueue_->Wait(gameFence_.Get(), b.gameFenceValue);
    presentQueue_->ExecuteCommandLists(1, lists);
    UINT64 v = ++presentFenceValue_;
    presentQueue_->Signal(presentFence_.Get(), v);
    presentAllocFence_[a] = v;

    const Pacing p = Cfg().pacing;
    if (p == Pacing::Spin) WaitUntil(target, true);
    else if (p == Pacing::Early) WaitUntil(target - MsToQpc(Cfg().earlyMs), false);
    if (p == Pacing::Queue && queueInterval_ == 0) {
        // derive the interval from the refresh rate and the output rate once both are known
        DWM_TIMING_INFO ti = { sizeof(ti) };
        if (frameMs_ > 0 && SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) {
            double refreshMs = QpcToMs((int64_t)ti.qpcRefreshPeriod);
            queueInterval_ = std::max(1, (int)std::lround(frameMs_ * 0.5 / refreshMs));
            Log("queue pacing: refresh %.3f ms, output %.3f ms -> sync interval %u", refreshMs, frameMs_ * 0.5, queueInterval_);
        }
    }
    if (p == Pacing::Queue && Cfg().queueInterval > 0) queueInterval_ = (UINT)Cfg().queueInterval;
    UINT sync;
    UINT flags = RealPresentFlags(b, &sync);
    if (p == Pacing::Queue && queueInterval_ == 0) sync = 1;
    int64_t call = Qpc();
    HRESULT hr = real_->Present(sync, flags);
    int64_t ret = Qpc();
    if (FAILED(hr)) Log("Present failed 0x%08lx", hr);
    if (presentLog_) WritePresentLog(generated, target, call, ret, b.frameID);
}

void FgSwapChain::WritePresentLog(bool generated, int64_t target, int64_t call, int64_t ret, uint64_t frameID)
{
    fprintf(presentLog_, "%s,%llu,%lld,%lld,%lld\n", generated ? "G" : "R", (unsigned long long)frameID, (long long)target,
            (long long)call, (long long)ret);
}

// ---------------------------------------------------------------------------------------------------------------
// Composition: plain copy, the app's present callback, or our pixel shader (UI blend / hudless UI extraction).
static const char* kShader = R"(
Texture2D<float4> src : register(t0);
Texture2D<float4> ui : register(t1);
Texture2D<float4> hudless : register(t2);
Texture2D<float4> realc : register(t3);
cbuffer C : register(b0) { uint mode; uint premul; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target
{
    int3 p = int3(pos.xy, 0);
    float4 c = src.Load(p);
    if (mode & 2) { c.rgb += realc.Load(p).rgb - hudless.Load(p).rgb; }  // put the real frame's UI on the generated frame
    if (mode & 1) {
        float4 u = ui.Load(p);
        c.rgb = premul ? u.rgb + c.rgb * (1 - u.a) : lerp(c.rgb, u.rgb, u.a);
    }
    return c;
}
)";

HRESULT FgSwapChain::CreateComposer()
{
    if (rootSig_) return S_OK;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { Log("d3dcompiler_47.dll not available: UI composition disabled"); return E_FAIL; }
    ComPtr<ID3DBlob> err;
    HRESULT hr = compile(kShader, strlen(kShader), "fgswap", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs_, &err);
    if (SUCCEEDED(hr)) hr = compile(kShader, strlen(kShader), "fgswap", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps_, &err);
    if (FAILED(hr)) { Log("shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return hr; }
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, 2 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &range };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (SUCCEEDED(hr)) hr = dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
    if (FAILED(hr)) return hr;
    D3D12_DESCRIPTOR_HEAP_DESC sh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4 * 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16 };
    if (FAILED(hr = dev_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvHeap_)))) return hr;
    if (FAILED(hr = dev_->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap_)))) return hr;
    srvInc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvInc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return S_OK;
}

void FgSwapChain::Compose(ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* bb, bool generated, const Batch& b)
{
    FgConfig fg;
    ffx::Resource ui;
    uint32_t uiFlags;
    {
        std::lock_guard<std::mutex> lk(cfgMtx_);
        fg = fg_;
        ui = ui_;
        uiFlags = uiFlags_;
    }
    Slot& s = slots_[b.slot];
    if (fg.presentCallback) {
        Transition(l, src, D3D12_RESOURCE_STATE_COMMON, SRV_STATE);
        struct { ffx::CallbackFrameGenerationPresent p; } c = {};
        ffx::Header premul = { ffx::EffectSub(ffx::EFFECT_FRAMEGENERATION, 0x0d), nullptr };
        struct PremulExt { ffx::Header h; bool use; } pre = { premul, (uiFlags & ffx::UI_USE_PREMUL_ALPHA) != 0 };
        c.p.header.type = ffx::CALLBACK_FRAMEGENERATION_PRESENT;
        c.p.header.pNext = &pre.h;
        c.p.device = dev_.Get();
        c.p.commandList = l;
        c.p.currentBackBuffer = ffx::MakeResource(src, ffx::STATE_PIXEL_COMPUTE_READ);
        c.p.currentUI = ui;
        c.p.outputSwapChainBuffer = ffx::MakeResource(bb, ffx::STATE_PRESENT);
        c.p.isGeneratedFrame = generated;
        c.p.frameID = b.frameID;
        ffx::ReturnCode rc = fg.presentCallback(&c.p, fg.presentCallbackCtx);
        if (rc) Log("present callback rc=%u", rc);
        Transition(l, src, SRV_STATE, D3D12_RESOURCE_STATE_COMMON);
        return;
    }
    const bool useUi = ui.resource != nullptr;
    const bool extract = generated && b.hudless && s.hudlessCopy;
    if ((useUi || extract) && SUCCEEDED(CreateComposer())) {
        DXGI_FORMAT bbFmt = bb->GetDesc().Format;
        if (!pso_ || psoFormat_ != bbFmt) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rootSig_.Get();
            pd.VS = { vs_->GetBufferPointer(), vs_->GetBufferSize() };
            pd.PS = { ps_->GetBufferPointer(), ps_->GetBufferSize() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = ViewFormat(bbFmt);
            pd.SampleDesc.Count = 1;
            pso_.Reset();
            if (FAILED(dev_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)))) { Log("composer PSO failed"); goto copy; }
            psoFormat_ = bbFmt;
        }
        {
            ID3D12Resource* srvs[4] = { src, useUi ? (ID3D12Resource*)ui.resource : nullptr, extract ? s.hudlessCopy.Get() : nullptr,
                                        extract ? s.realCopy.Get() : nullptr };
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            UINT base = (srvRing_++ % 16) * 4;
            cpu.ptr += (SIZE_T)base * srvInc_;
            gpu.ptr += (UINT64)base * srvInc_;
            for (int i = 0; i < 4; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC vd = {};
                vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                vd.Texture2D.MipLevels = 1;
                vd.Format = srvs[i] ? ViewFormat(srvs[i]->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM;
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
                h.ptr += (SIZE_T)i * srvInc_;
                dev_->CreateShaderResourceView(srvs[i], &vd, h);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)(rtvRing_++ % 16) * rtvInc_;
            D3D12_RENDER_TARGET_VIEW_DESC rd = {};
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rd.Format = ViewFormat(bbFmt);
            dev_->CreateRenderTargetView(bb, &rd, rtv);
            D3D12_RESOURCE_STATES uiState = useUi ? ToD3D12(ui.state) : D3D12_RESOURCE_STATE_COMMON;
            Transition(l, src, D3D12_RESOURCE_STATE_COMMON, SRV_STATE);
            if (useUi) Transition(l, (ID3D12Resource*)ui.resource, uiState, SRV_STATE);
            if (extract) {
                Transition(l, s.hudlessCopy.Get(), D3D12_RESOURCE_STATE_COMMON, SRV_STATE);
                Transition(l, s.realCopy.Get(), D3D12_RESOURCE_STATE_COMMON, SRV_STATE);
            }
            Transition(l, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_RESOURCE_DESC bd = bb->GetDesc();
            D3D12_VIEWPORT vp = { 0, 0, (float)bd.Width, (float)bd.Height, 0, 1 };
            D3D12_RECT sc = { 0, 0, (LONG)bd.Width, (LONG)bd.Height };
            ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
            l->SetDescriptorHeaps(1, heaps);
            l->SetGraphicsRootSignature(rootSig_.Get());
            UINT consts[2] = { (useUi ? 1u : 0u) | (extract ? 2u : 0u), (uiFlags & ffx::UI_USE_PREMUL_ALPHA) ? 1u : 0u };
            l->SetGraphicsRoot32BitConstants(0, 2, consts, 0);
            l->SetGraphicsRootDescriptorTable(1, gpu);
            l->SetPipelineState(pso_.Get());
            l->RSSetViewports(1, &vp);
            l->RSSetScissorRects(1, &sc);
            l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            l->DrawInstanced(3, 1, 0, 0);
            Transition(l, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            if (extract) {
                Transition(l, s.hudlessCopy.Get(), SRV_STATE, D3D12_RESOURCE_STATE_COMMON);
                Transition(l, s.realCopy.Get(), SRV_STATE, D3D12_RESOURCE_STATE_COMMON);
            }
            if (useUi) Transition(l, (ID3D12Resource*)ui.resource, SRV_STATE, uiState);
            Transition(l, src, SRV_STATE, D3D12_RESOURCE_STATE_COMMON);
            return;
        }
    }
copy:
    Transition(l, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(l, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(bb, src);
    Transition(l, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Transition(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

}  // namespace fgswap
