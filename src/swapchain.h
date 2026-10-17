// swapchain.h - FgSwapChain: our frame-generation swapchain. The game sees an IDXGISwapChain4 with replacement back
// buffers; each game Present records the generated frame (AMD's frame-generation context via the public FFX dispatch)
// and hands a batch to a presenter thread, which composes and presents generated + real frames on its own schedule.
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "common.h"
#include "ffx_abi.h"

namespace fgswap {

using Microsoft::WRL::ComPtr;

// {6E0F2A5B-6C7E-4B6B-9C21-3F3E2B1A7D11}: lets our DLL recognise its own swapchain objects.
static const GUID IID_FgSwapChain = { 0x6e0f2a5b, 0x6c7e, 0x4b6b, { 0x9c, 0x21, 0x3f, 0x3e, 0x2b, 0x1a, 0x7d, 0x11 } };

struct FgConfig {  // what the game configured on its frame-generation context for this swapchain
    ffx::Context fgCtx = nullptr;          // AMD's context handle (value, not the game's pointer)
    ffx::PfnDispatch dispatch = nullptr;  // AMD's ffxDispatch
    ffx::PresentCallbackFunc presentCallback = nullptr;
    void* presentCallbackCtx = nullptr;
    ffx::FrameGenerationDispatchFunc fgCallback = nullptr;
    void* fgCallbackCtx = nullptr;
    bool enabled = false;
    bool onlyPresentGenerated = false;
    ffx::Resource hudless = {};
    uint32_t flags = 0;
    ffx::Rect2D rect = {};
    uint64_t frameID = 0;
    bool configured = false;
};

class FgSwapChain final : public IDXGISwapChain4 {
public:
    static HRESULT Create(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, FgSwapChain** out);
    static HRESULT Wrap(IDXGISwapChain4* existing, ID3D12CommandQueue* gameQueue, FgSwapChain** out);

    // FFX-side entry points
    void SetFgConfig(const FgConfig& c);
    void SetUiResource(const ffx::Resource& r, uint32_t flags);
    void WaitForPresents();
    ID3D12GraphicsCommandList* InterpolationCommandList();
    ffx::Resource InterpolationTexture();
    void SetWaitCallback(void* fn) { waitCallback_ = fn; }
    uint64_t GpuMemoryBytes() const;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return real_->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return real_->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return real_->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** p) override { return real_->GetParent(riid, p); }
    // IDXGIDeviceSubObject
    // D3D12 swapchains report their present queue here; games expect the queue they created the swapchain with.
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** p) override
    {
        if (riid == __uuidof(ID3D12CommandQueue) || riid == __uuidof(IUnknown)) return gameQueue_->QueryInterface(riid, p);
        if (riid == __uuidof(ID3D12Device) || riid == __uuidof(ID3D12Device1)) return dev_->QueryInterface(riid, p);
        return real_->GetDevice(riid, p);
    }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** t) override { return real_->GetFullscreenState(f, t); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* p) override { return real_->ResizeTarget(p); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return real_->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return real_->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { *c = gamePresentCount_; return S_OK; }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p) override { return real_->GetFullscreenDesc(p); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return real_->GetHwnd(h); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID r, void** u) override { return real_->GetCoreWindow(r, u); }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS*) override { return Present(SyncInterval, Flags); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return real_->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return real_->GetRestrictToOutput(o); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return real_->SetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return real_->GetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return real_->SetRotation(r); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return real_->GetRotation(r); }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return real_->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return real_->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) override { *p = maxLatency_; return S_OK; }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return real_->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return real_->GetMatrixTransform(m); }
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return gameIndex_; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return real_->CheckColorSpaceSupport(c, s); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags,
                                             const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) override;
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* d) override { return real_->SetHDRMetaData(t, s, d); }

private:
    static const int kSlots = 4;  // ring of per-real-frame copies (real copy, generated frame, hudless copy)
    struct Slot {
        ComPtr<ID3D12Resource> realCopy, interp, hudlessCopy;
        ComPtr<ID3D12CommandAllocator> alloc;   // game-queue work for this slot
        UINT64 freeAt = 0;                      // presentFence value after which the slot may be rewritten
    };
    struct Batch {
        int slot;
        UINT64 gameFenceValue;
        bool fg;           // a generated frame is in slot.interp
        bool hudless;      // slot.hudlessCopy is valid (UI extraction for the generated frame)
        UINT sync, flags;
        int64_t arrival;   // QPC when the game called Present
        uint64_t frameID;
    };

    FgSwapChain() = default;
    ~FgSwapChain();
    HRESULT Init(ID3D12CommandQueue* gameQueue, ComPtr<IDXGISwapChain4> real, ComPtr<ID3D12CommandQueue> presentQueue);
    HRESULT CreateGameBuffers();
    HRESULT CreateSlots();
    HRESULT CreateComposer();
    void ReleaseBuffers();
    void Drain();
    void PresenterMain();
    void PresentOne(const Batch& b, bool generated, int64_t target);
    void Compose(ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* backBuffer, bool generated, const Batch& b);
    UINT RealPresentFlags(const Batch& b, UINT* sync) const;
    void WritePresentLog(bool generated, int64_t target, int64_t call, int64_t ret, uint64_t frameID);

    std::atomic<ULONG> refs_{1};
    ComPtr<IDXGISwapChain4> real_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> gameQueue_, presentQueue_;
    DXGI_SWAP_CHAIN_DESC1 gameDesc_ = {};
    UINT realFlags_ = 0;
    bool tearingSupported_ = false;
    std::vector<ComPtr<ID3D12Resource>> gameBuffers_;
    UINT gameIndex_ = 0;
    UINT gamePresentCount_ = 0;
    Slot slots_[kSlots];
    int nextSlot_ = 0;
    ComPtr<ID3D12GraphicsCommandList> gameList_;
    ComPtr<ID3D12GraphicsCommandList> interpList_;  // handed to apps that query the interpolation command list
    ComPtr<ID3D12Fence> gameFence_, presentFence_;
    UINT64 gameFenceValue_ = 0;
    std::atomic<UINT64> presentFenceValue_{0};
    HANDLE gameFenceEvent_ = nullptr, presentFenceEvent_ = nullptr;

    // presenter
    std::thread presenter_;
    std::mutex mtx_;
    std::condition_variable cv_, cvSpace_;
    std::deque<Batch> queue_;
    bool stop_ = false;
    int inFlight_ = 0;  // batches handed over and not yet fully presented
    UINT maxLatency_ = 2;
    HANDLE latencySemaphore_ = nullptr;
    static const int kPresentAllocs = 8;
    ComPtr<ID3D12CommandAllocator> presentAllocs_[kPresentAllocs];
    UINT64 presentAllocFence_[kPresentAllocs] = {};
    int presentAllocIdx_ = 0;
    ComPtr<ID3D12GraphicsCommandList> presentList_;

    // pacing state (presenter thread)
    int64_t lastRealSlot_ = 0;
    double frameMs_ = 0;        // smoothed real-frame interval
    int64_t lastArrival_ = 0;
    FILE* presentLog_ = nullptr;

    // FG + UI configuration (game thread writes under cfgMtx_)
    std::mutex cfgMtx_;
    FgConfig fg_;
    ffx::Context fgCtxValue_ = nullptr;  // AMD's frame-generation context handle (dispatch takes a pointer to it)
    bool resetNext_ = true;
    DXGI_COLOR_SPACE_TYPE colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT queueInterval_ = 0;
    ffx::Resource ui_ = {};
    uint32_t uiFlags_ = 0;
    void* waitCallback_ = nullptr;

    // composer (fullscreen triangle; only used when UI / hudless / present callback need more than a copy)
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    DXGI_FORMAT psoFormat_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> srvHeap_, rtvHeap_;
    UINT srvInc_ = 0, rtvInc_ = 0, srvRing_ = 0, rtvRing_ = 0;
    ComPtr<ID3DBlob> vs_, ps_;
};

}  // namespace fgswap
