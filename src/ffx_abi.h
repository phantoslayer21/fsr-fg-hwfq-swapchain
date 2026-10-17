// ffx_abi.h - our own declarations of the parts of AMD's public FidelityFX API (FFX API, SDK 2.x) that the
// frame-generation swapchain replacement needs. Written for binary compatibility with AMD's published interface;
// type IDs, field order and sizes follow AMD's public headers (FidelityFX SDK v2.3.0, MIT licence). No AMD code.
#pragma once
#include <stdint.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace ffx {

// ---- core --------------------------------------------------------------------------------------------------
typedef void* Context;
typedef uint32_t ReturnCode;
enum : ReturnCode {
    RC_OK = 0, RC_ERROR = 1, RC_UNKNOWN_DESCTYPE = 2, RC_RUNTIME_ERROR = 3, RC_NO_PROVIDER = 4, RC_MEMORY = 5,
    RC_PARAMETER = 6, RC_NO_SUPPORT_NEW_DESCTYPE = 7,
};

struct Header {
    uint64_t type;
    Header* pNext;
};

struct AllocationCallbacks {
    void* pUserData;
    void* (*alloc)(void* pUserData, uint64_t size);
    void (*dealloc)(void* pUserData, void* pMem);
};

typedef ReturnCode (*PfnCreateContext)(Context* context, Header* desc, const AllocationCallbacks* memCb);
typedef ReturnCode (*PfnDestroyContext)(Context* context, const AllocationCallbacks* memCb);
typedef ReturnCode (*PfnConfigure)(Context* context, const Header* desc);
typedef ReturnCode (*PfnQuery)(Context* context, Header* desc);
typedef ReturnCode (*PfnDispatch)(Context* context, const Header* desc);

constexpr uint32_t EFFECT_MASK = 0x00ff0000u;
constexpr uint32_t BACKEND_MASK = 0xff000000u;
constexpr uint32_t EFFECT_FRAMEGENERATION = 0x00020000u;
constexpr uint32_t EFFECT_FRAMEGENERATIONSWAPCHAIN = 0x00030000u;
constexpr uint32_t BACKEND_DX12 = 0x00000000u;
constexpr uint64_t EffectSub(uint32_t effect, uint32_t sub) { return (effect & EFFECT_MASK) | (sub & ~EFFECT_MASK); }
constexpr uint64_t BackendSub(uint32_t backend, uint32_t sub) { return (backend & BACKEND_MASK) | (sub & ~BACKEND_MASK); }
constexpr uint64_t BackendEffectSub(uint32_t backend, uint32_t effect, uint32_t sub)
{
    return (sub & ~EFFECT_MASK) | (effect & EFFECT_MASK) | (backend & BACKEND_MASK) | (sub & ~(BACKEND_MASK | EFFECT_MASK));
}
inline uint32_t EffectOf(uint64_t type) { return (uint32_t)type & EFFECT_MASK; }

// general descs
constexpr uint64_t CONFIGURE_GLOBALDEBUG1 = 1u;
typedef void (*MessageFunc)(uint32_t type, const wchar_t* message);
struct ConfigureGlobalDebug1 {
    Header header;
    MessageFunc fpMessage;
    uint32_t debugLevel;  // 0 silence, 1 errors, 2 warnings, 0xfffffff verbose
};
constexpr uint64_t QUERY_GET_PROVIDER_VERSION = 6u;
struct QueryGetProviderVersion {
    Header header;
    uint64_t versionId;
    const char* versionName;
};

// ---- types -------------------------------------------------------------------------------------------------
enum SurfaceFormat : uint32_t {
    FMT_UNKNOWN = 0, FMT_R32G32B32A32_TYPELESS, FMT_R32G32B32A32_UINT, FMT_R32G32B32A32_FLOAT, FMT_R16G16B16A16_FLOAT,
    FMT_R32G32B32_FLOAT, FMT_R32G32_FLOAT, FMT_R8_UINT, FMT_R32_UINT, FMT_R8G8B8A8_TYPELESS, FMT_R8G8B8A8_UNORM,
    FMT_R8G8B8A8_SNORM, FMT_R8G8B8A8_SRGB, FMT_B8G8R8A8_TYPELESS, FMT_B8G8R8A8_UNORM, FMT_B8G8R8A8_SRGB,
    FMT_R11G11B10_FLOAT, FMT_R10G10B10A2_UNORM, FMT_R16G16_FLOAT, FMT_R16G16_UINT, FMT_R16G16_SINT, FMT_R16_FLOAT,
    FMT_R16_UINT, FMT_R16_UNORM, FMT_R16_SNORM, FMT_R8_UNORM, FMT_R8G8_UNORM, FMT_R8G8_UINT, FMT_R32_FLOAT,
    FMT_R9G9B9E5_SHAREDEXP, FMT_R16G16B16A16_TYPELESS, FMT_R32G32_TYPELESS, FMT_R10G10B10A2_TYPELESS,
    FMT_R16G16_TYPELESS, FMT_R16_TYPELESS, FMT_R8_TYPELESS, FMT_R8G8_TYPELESS, FMT_R32_TYPELESS, FMT_R32G32_UINT,
    FMT_R8_SNORM,
};
enum ResourceUsage : uint32_t { USAGE_READ_ONLY = 0, USAGE_RENDERTARGET = 1u << 0, USAGE_UAV = 1u << 1, USAGE_DEPTHTARGET = 1u << 2 };
enum ResourceState : uint32_t {
    STATE_COMMON = 1u << 0, STATE_UNORDERED_ACCESS = 1u << 1, STATE_COMPUTE_READ = 1u << 2, STATE_PIXEL_READ = 1u << 3,
    STATE_PIXEL_COMPUTE_READ = STATE_PIXEL_READ | STATE_COMPUTE_READ, STATE_COPY_SRC = 1u << 4, STATE_COPY_DEST = 1u << 5,
    STATE_PRESENT = 1u << 7, STATE_RENDER_TARGET = 1u << 8,
};
enum ResourceType : uint32_t { TYPE_BUFFER = 0, TYPE_TEXTURE1D, TYPE_TEXTURE2D, TYPE_TEXTURE_CUBE, TYPE_TEXTURE3D };
enum TransferFunction : uint32_t { TF_SRGB = 0, TF_PQ, TF_SCRGB };

struct Dimensions2D { uint32_t width, height; };
struct FloatCoords2D { float x, y; };
struct Rect2D { int32_t left, top, width, height; };

struct ResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;   // or size
    uint32_t height;  // or stride
    uint32_t depth;   // or alignment
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};
struct Resource {
    void* resource;  // ID3D12Resource*
    ResourceDescription description;
    uint32_t state;
};
static_assert(sizeof(Resource) == 48, "FfxApiResource layout");

uint32_t SurfaceFormatFromDxgi(DXGI_FORMAT f);
DXGI_FORMAT DxgiFromSurfaceFormat(uint32_t f);
Resource MakeResource(ID3D12Resource* r, uint32_t state);

// ---- DX12 backend ------------------------------------------------------------------------------------------
constexpr uint64_t CREATE_BACKEND_DX12 = BackendSub(BACKEND_DX12, 0x02);
struct CreateBackendDX12 {
    Header header;
    ID3D12Device* device;
};

// ---- frame generation (the ML / analytic interpolation context; implemented by AMD's DLL) ------------------
enum FgCreateFlags : uint32_t {
    FG_ENABLE_ASYNC_WORKLOAD_SUPPORT = 1u << 0, FG_ENABLE_DISPLAY_RES_MV = 1u << 1, FG_ENABLE_MV_JITTER_CANCELLATION = 1u << 2,
    FG_ENABLE_DEPTH_INVERTED = 1u << 3, FG_ENABLE_DEPTH_INFINITE = 1u << 4, FG_ENABLE_HDR = 1u << 5,
};
enum FgDispatchFlags : uint32_t {
    FG_FLAG_DRAW_DEBUG_TEAR_LINES = 1u << 0, FG_FLAG_DRAW_DEBUG_RESET_INDICATORS = 1u << 1, FG_FLAG_DRAW_DEBUG_VIEW = 1u << 2,
    FG_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY = 1u << 3, FG_FLAG_DRAW_DEBUG_PACING_LINES = 1u << 4,
};
enum UiCompositionFlags : uint32_t { UI_USE_PREMUL_ALPHA = 1u << 0, UI_ENABLE_INTERNAL_DOUBLE_BUFFERING = 1u << 1 };

constexpr uint64_t CREATE_FRAMEGENERATION = EffectSub(EFFECT_FRAMEGENERATION, 0x01);
struct CreateFrameGeneration {
    Header header;
    uint32_t flags;
    Dimensions2D displaySize;
    Dimensions2D maxRenderSize;
    uint32_t backBufferFormat;
};
constexpr uint64_t CREATE_FRAMEGENERATION_VERSION = EffectSub(EFFECT_FRAMEGENERATION, 0x0e);
constexpr uint32_t FRAMEGENERATION_VERSION = (4u << 22) | (0u << 12) | 1u;  // 4.0.1
struct CreateFrameGenerationVersion {
    Header header;
    uint32_t version;
};

constexpr uint64_t CALLBACK_FRAMEGENERATION_PRESENT = EffectSub(EFFECT_FRAMEGENERATION, 0x05);
struct CallbackFrameGenerationPresent {
    Header header;
    void* device;
    void* commandList;
    Resource currentBackBuffer;
    Resource currentUI;
    Resource outputSwapChainBuffer;
    bool isGeneratedFrame;
    uint64_t frameID;
};

constexpr uint64_t DISPATCH_FRAMEGENERATION = EffectSub(EFFECT_FRAMEGENERATION, 0x03);
struct DispatchFrameGeneration {
    Header header;
    void* commandList;
    Resource presentColor;
    Resource outputs[4];
    uint32_t numGeneratedFrames;
    bool reset;
    uint32_t backbufferTransferFunction;
    float minMaxLuminance[2];
    Rect2D generationRect;
    uint64_t frameID;
};

typedef ReturnCode (*PresentCallbackFunc)(CallbackFrameGenerationPresent* params, void* pUserCtx);
typedef ReturnCode (*FrameGenerationDispatchFunc)(DispatchFrameGeneration* params, void* pUserCtx);

constexpr uint64_t CONFIGURE_FRAMEGENERATION = EffectSub(EFFECT_FRAMEGENERATION, 0x02);
struct ConfigureFrameGeneration {
    Header header;
    void* swapChain;
    PresentCallbackFunc presentCallback;
    void* presentCallbackUserContext;
    FrameGenerationDispatchFunc frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    Resource HUDLessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    Rect2D generationRect;
    uint64_t frameID;
};

constexpr uint64_t DISPATCH_FRAMEGENERATION_PREPARE = EffectSub(EFFECT_FRAMEGENERATION, 0x04);
constexpr uint64_t DISPATCH_FRAMEGENERATION_PREPARE_V2 = EffectSub(EFFECT_FRAMEGENERATION, 0x0c);
struct DispatchFrameGenerationPrepareV2 {
    Header header;
    uint64_t frameID;
    uint32_t flags;
    void* commandList;
    Dimensions2D renderSize;
    FloatCoords2D jitterOffset;
    FloatCoords2D motionVectorScale;
    float frameTimeDelta;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    Resource depth;
    Resource motionVectors;
    float cameraPosition[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraForward[3];
};

// ---- frame generation swapchain (what our DLL implements) --------------------------------------------------
constexpr uint64_t CREATE_FGSWAPCHAIN_WRAP_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x01);
struct CreateFgSwapChainWrapDX12 {
    Header header;
    IDXGISwapChain4** swapchain;
    ID3D12CommandQueue* gameQueue;
};
constexpr uint64_t CREATE_FGSWAPCHAIN_NEW_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x05);
struct CreateFgSwapChainNewDX12 {
    Header header;
    IDXGISwapChain4** swapchain;
    DXGI_SWAP_CHAIN_DESC* desc;
    IDXGIFactory* dxgiFactory;
    ID3D12CommandQueue* gameQueue;
};
constexpr uint64_t CREATE_FGSWAPCHAIN_FOR_HWND_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x06);
struct CreateFgSwapChainForHwndDX12 {
    Header header;
    IDXGISwapChain4** swapchain;
    HWND hwnd;
    DXGI_SWAP_CHAIN_DESC1* desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc;
    IDXGIFactory* dxgiFactory;
    ID3D12CommandQueue* gameQueue;
};
constexpr uint64_t CREATE_FGSWAPCHAIN_VERSION_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x0b);
constexpr uint32_t FGSWAPCHAIN_VERSION = (3u << 22) | (1u << 12) | 7u;  // 3.1.7
struct CreateFgSwapChainVersionDX12 {
    Header header;
    uint32_t version;
};

constexpr uint64_t CONFIGURE_FGSWAPCHAIN_REGISTERUIRESOURCE_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x02);
struct ConfigureFgSwapChainRegisterUiResourceDX12 {
    Header header;
    Resource uiResource;
    uint32_t flags;
};
constexpr uint64_t QUERY_FGSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x03);
struct QueryFgSwapChainInterpolationCommandListDX12 {
    Header header;
    void** pOutCommandList;
};
constexpr uint64_t QUERY_FGSWAPCHAIN_INTERPOLATIONTEXTURE_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x04);
struct QueryFgSwapChainInterpolationTextureDX12 {
    Header header;
    Resource* pOutTexture;
};
constexpr uint64_t DISPATCH_FGSWAPCHAIN_WAIT_FOR_PRESENTS_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x07);
constexpr uint64_t CONFIGURE_FGSWAPCHAIN_KEYVALUE_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x08);
struct ConfigureFgSwapChainKeyValueDX12 {
    Header header;
    uint64_t key;
    uint64_t u64;
    void* ptr;
};
enum FgSwapChainKey : uint64_t { FGSC_KEY_WAITCALLBACK = 0, FGSC_KEY_FRAMEPACINGTUNING = 2 };
constexpr uint64_t QUERY_FGSWAPCHAIN_GPU_MEMORY_USAGE_DX12 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x09);
constexpr uint64_t QUERY_FGSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2 = BackendEffectSub(BACKEND_DX12, EFFECT_FRAMEGENERATIONSWAPCHAIN, 0x0a);
struct EffectMemoryUsage { uint64_t totalUsageInBytes, aliasableUsageInBytes; };
struct QueryFgSwapChainGpuMemoryUsageDX12 {
    Header header;
    EffectMemoryUsage* usage;
};

// Walk a pNext chain for a given type.
inline const Header* Find(const Header* h, uint64_t type)
{
    for (; h; h = h->pNext)
        if (h->type == type) return h;
    return nullptr;
}

}  // namespace ffx
