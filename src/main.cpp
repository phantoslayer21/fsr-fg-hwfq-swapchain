// main.cpp - fgswap: a one-file swap for AMD FSR frame generation. It exports the same five FFX API entry points as
// AMD's DLLs and works in one of two roles, picked from its own file name:
//   amd_fidelityfx_loader_dx12.dll  (loader role, the default install): replaces AMD's 26 KB loader. Like AMD's loader
//       it loads AMD's effect DLLs next to it by their real names (frame generation incl. ML, upscaler, denoiser,
//       radiance cache) and routes each call to the right one, untouched. Only the frame-generation *swapchain* is ours.
//   any other name (proxy role): stands in for one AMD DLL and forwards to AMD's original at fgswap.ini `original`
//       (default fgswap\<same name>), for games without the loader.
// In both roles the game's frame-generation context is configured "interpolate only" and our swapchain dispatches it
// through the public FFX API. Own code; uses only AMD's public FFX API and Microsoft's DXGI/D3D12.
#include <windows.h>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common.h"
#include "ffx_abi.h"
#include "swapchain.h"

using namespace fgswap;

namespace {

struct Provider {
    std::wstring name;
    HMODULE module = nullptr;
    std::vector<uint32_t> effects;  // effect IDs routed here (empty = everything)
    ffx::PfnCreateContext create = nullptr;
    ffx::PfnDestroyContext destroy = nullptr;
    ffx::PfnConfigure configure = nullptr;
    ffx::PfnQuery query = nullptr;
    ffx::PfnDispatch dispatch = nullptr;
    bool Handles(uint32_t effect) const
    {
        if (effects.empty()) return true;
        for (uint32_t e : effects) if (e == effect) return true;
        return false;
    }
};

HMODULE g_self;
std::once_flag g_init;
bool g_ok, g_loaderRole;
std::vector<Provider> g_providers;

const uint32_t kMagic = 0x46475357;  // 'FGSW'
struct SwapCtx {
    uint32_t magic;
    FgSwapChain* sc;
};
std::mutex g_mtx;
std::unordered_set<void*> g_ours;                         // our swapchain contexts
std::unordered_map<void*, Provider*> g_owner;             // AMD contexts -> the provider that created them
std::unordered_set<void*> g_fgContexts;                   // AMD frame-generation contexts

bool LoadProvider(const std::wstring& path, std::vector<uint32_t> effects, bool required)
{
    Provider p;
    p.name = path;
    p.effects = std::move(effects);
    p.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!p.module) {
        if (required) Log("cannot load '%ls' (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    p.create = (ffx::PfnCreateContext)GetProcAddress(p.module, "ffxCreateContext");
    p.destroy = (ffx::PfnDestroyContext)GetProcAddress(p.module, "ffxDestroyContext");
    p.configure = (ffx::PfnConfigure)GetProcAddress(p.module, "ffxConfigure");
    p.query = (ffx::PfnQuery)GetProcAddress(p.module, "ffxQuery");
    p.dispatch = (ffx::PfnDispatch)GetProcAddress(p.module, "ffxDispatch");
    if (!(p.create && p.destroy && p.configure && p.query && p.dispatch)) {
        Log("'%ls' lacks the FFX exports", path.c_str());
        return false;
    }
    Log("provider '%ls' loaded", path.c_str());
    g_providers.push_back(p);
    return true;
}

void Init()
{
    LoadConfig(g_self);
    const Config& c = Cfg();
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(g_self, self, MAX_PATH);
    std::wstring name = self;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    for (auto& ch : name) ch = (wchar_t)towlower(ch);
    g_loaderRole = name == L"amd_fidelityfx_loader_dx12.dll";
    if (g_loaderRole) {
        // The same effect DLLs AMD's loader looks for, by their real names, next to us.
        LoadProvider(c.dir + L"amd_fidelityfx_framegeneration_dx12.dll",
                     { ffx::EFFECT_FRAMEGENERATION, ffx::EFFECT_FRAMEGENERATIONSWAPCHAIN }, true);
        LoadProvider(c.dir + L"amd_fidelityfx_upscaler_dx12.dll", { 0x00010000u }, false);
        LoadProvider(c.dir + L"amd_fidelityfx_denoiser_dx12.dll", { 0x00050000u }, false);
        LoadProvider(c.dir + L"amd_fidelityfx_radiancecache_dx12.dll", { 0x00060000u }, false);
    } else {
        LoadProvider(c.original, {}, true);
    }
    // the providers vector is final now; pointers into it stay valid
    g_ok = !g_providers.empty();
    static const char* names[] = { "spin", "early", "queue", "amd" };
    Log("fgswap 0.2 loaded as %ls (%s role), %zu AMD provider DLL(s); pacing=%s early_ms=%.2f spin_us=%d queue_interval=%d "
        "max_queued_frames=%d",
        name.c_str(), g_loaderRole ? "loader" : "proxy", g_providers.size(), names[(int)c.pacing], c.earlyMs, c.spinUs,
        c.queueInterval, c.maxQueuedFrames);
}

bool Ready()
{
    std::call_once(g_init, Init);
    return g_ok;
}

Provider* ForEffect(uint32_t effect)
{
    for (auto& p : g_providers) if (p.Handles(effect)) return &p;
    return nullptr;
}

Provider* Owner(ffx::Context* ctx)
{
    if (ctx && *ctx) {
        std::lock_guard<std::mutex> l(g_mtx);
        auto it = g_owner.find(*ctx);
        if (it != g_owner.end()) return it->second;
    }
    return g_providers.size() == 1 ? &g_providers[0] : nullptr;
}

Provider* FgProvider() { return ForEffect(ffx::EFFECT_FRAMEGENERATION); }

SwapCtx* Ours(ffx::Context* ctx)
{
    if (!ctx || !*ctx) return nullptr;
    std::lock_guard<std::mutex> l(g_mtx);
    return g_ours.count(*ctx) ? (SwapCtx*)*ctx : nullptr;
}

bool IsFgContext(ffx::Context* ctx)
{
    if (!ctx || !*ctx) return false;
    std::lock_guard<std::mutex> l(g_mtx);
    return g_fgContexts.count(*ctx) != 0;
}

bool IsSwapchainCreate(uint64_t t)
{
    return t == ffx::CREATE_FGSWAPCHAIN_FOR_HWND_DX12 || t == ffx::CREATE_FGSWAPCHAIN_NEW_DX12 || t == ffx::CREATE_FGSWAPCHAIN_WRAP_DX12;
}

ffx::ReturnCode CreateSwapchain(ffx::Context* ctx, ffx::Header* desc)
{
    FgSwapChain* sc = nullptr;
    HRESULT hr = E_INVALIDARG;
    IDXGISwapChain4** out = nullptr;
    if (desc->type == ffx::CREATE_FGSWAPCHAIN_FOR_HWND_DX12) {
        auto* d = (ffx::CreateFgSwapChainForHwndDX12*)desc;
        out = d->swapchain;
        hr = FgSwapChain::Create(d->dxgiFactory, d->gameQueue, d->hwnd, *d->desc, d->fullscreenDesc, &sc);
    } else if (desc->type == ffx::CREATE_FGSWAPCHAIN_NEW_DX12) {
        auto* d = (ffx::CreateFgSwapChainNewDX12*)desc;
        out = d->swapchain;
        DXGI_SWAP_CHAIN_DESC1 d1 = {};
        d1.Width = d->desc->BufferDesc.Width;
        d1.Height = d->desc->BufferDesc.Height;
        d1.Format = d->desc->BufferDesc.Format;
        d1.SampleDesc = { 1, 0 };
        d1.BufferUsage = d->desc->BufferUsage;
        d1.BufferCount = d->desc->BufferCount;
        d1.Scaling = DXGI_SCALING_STRETCH;
        d1.SwapEffect = d->desc->SwapEffect;
        d1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        d1.Flags = d->desc->Flags;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
        fs.RefreshRate = d->desc->BufferDesc.RefreshRate;
        fs.ScanlineOrdering = d->desc->BufferDesc.ScanlineOrdering;
        fs.Scaling = d->desc->BufferDesc.Scaling;
        fs.Windowed = d->desc->Windowed;
        hr = FgSwapChain::Create(d->dxgiFactory, d->gameQueue, d->desc->OutputWindow, d1, &fs, &sc);
    } else {
        auto* d = (ffx::CreateFgSwapChainWrapDX12*)desc;
        out = d->swapchain;
        hr = FgSwapChain::Wrap(*d->swapchain, d->gameQueue, &sc);
    }
    if (FAILED(hr) || !sc) {
        Log("swapchain creation (type 0x%llx) failed 0x%08lx", (unsigned long long)desc->type, hr);
        return ffx::RC_RUNTIME_ERROR;
    }
    *out = sc;     // the game's reference
    sc->AddRef();  // the context's reference
    auto* c = new SwapCtx{ kMagic, sc };
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_ours.insert(c);
    }
    *ctx = c;
    Log("created fgswap swapchain context %p (type 0x%llx)", (void*)c, (unsigned long long)desc->type);
    return ffx::RC_OK;
}

// The game configures its frame-generation context with our swapchain: keep the settings for our presenter and give
// AMD's context the "interpolate only" configuration (our swapchain dispatches it through the public API).
ffx::ReturnCode ConfigureFg(Provider* p, ffx::Context* ctx, const ffx::Header* desc)
{
    auto* c = (const ffx::ConfigureFrameGeneration*)desc;
    FgSwapChain* sc = nullptr;
    if (c->swapChain) ((IUnknown*)c->swapChain)->QueryInterface(IID_FgSwapChain, (void**)&sc);
    if (!sc) return p->configure(ctx, desc);
    FgConfig f;
    f.fgCtx = *ctx;
    f.dispatch = p->dispatch;
    f.presentCallback = c->presentCallback;
    f.presentCallbackCtx = c->presentCallbackUserContext;
    f.fgCallback = c->frameGenerationCallback;
    f.fgCallbackCtx = c->frameGenerationCallbackUserContext;
    f.enabled = c->frameGenerationEnabled;
    f.onlyPresentGenerated = c->onlyPresentGenerated;
    f.hudless = c->HUDLessColor;
    f.flags = c->flags;
    f.rect = c->generationRect;
    f.frameID = c->frameID;
    f.configured = true;
    sc->SetFgConfig(f);
    sc->Release();
    ffx::ConfigureFrameGeneration amd = *c;
    amd.swapChain = nullptr;
    amd.presentCallback = nullptr;
    amd.presentCallbackUserContext = nullptr;
    amd.frameGenerationCallback = nullptr;
    amd.frameGenerationCallbackUserContext = nullptr;
    amd.flags |= ffx::FG_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    return p->configure(ctx, &amd.header);
}

// Context-free calls: route by the effect the descriptor refers to; otherwise ask each provider in turn.
template <class F>
ffx::ReturnCode TryProviders(uint32_t effect, F call)
{
    if (Provider* p = ForEffect(effect)) {
        ffx::ReturnCode rc = call(*p);
        if (rc != ffx::RC_UNKNOWN_DESCTYPE && rc != ffx::RC_NO_PROVIDER) return rc;
    }
    ffx::ReturnCode last = ffx::RC_NO_PROVIDER;
    for (auto& p : g_providers) {
        ffx::ReturnCode rc = call(p);
        if (rc != ffx::RC_UNKNOWN_DESCTYPE && rc != ffx::RC_NO_PROVIDER) return rc;
        last = rc;
    }
    return last;
}

}  // namespace

extern "C" {

__declspec(dllexport) ffx::ReturnCode ffxCreateContext(ffx::Context* ctx, ffx::Header* desc, const ffx::AllocationCallbacks* memCb)
{
    if (!Ready()) return ffx::RC_NO_PROVIDER;
    if (!ctx || !desc) return ffx::RC_PARAMETER;
    if (IsSwapchainCreate(desc->type) && Cfg().pacing != Pacing::Amd) return CreateSwapchain(ctx, desc);
    Provider* used = nullptr;
    ffx::ReturnCode rc = TryProviders(ffx::EffectOf(desc->type), [&](Provider& p) {
        ffx::ReturnCode r = p.create(ctx, desc, memCb);
        if (r == ffx::RC_OK) used = &p;
        return r;
    });
    if (rc == ffx::RC_OK && used) {
        std::lock_guard<std::mutex> l(g_mtx);
        g_owner[*ctx] = used;
        if (desc->type == ffx::CREATE_FRAMEGENERATION) g_fgContexts.insert(*ctx);
    }
    if (desc->type == ffx::CREATE_FRAMEGENERATION)
        Log("frame-generation context %p created by AMD (rc %u)", rc == ffx::RC_OK ? *ctx : nullptr, rc);
    return rc;
}

__declspec(dllexport) ffx::ReturnCode ffxDestroyContext(ffx::Context* ctx, const ffx::AllocationCallbacks* memCb)
{
    if (!Ready()) return ffx::RC_NO_PROVIDER;
    if (SwapCtx* c = Ours(ctx)) {
        {
            std::lock_guard<std::mutex> l(g_mtx);
            g_ours.erase(c);
        }
        c->sc->WaitForPresents();
        c->sc->Release();
        delete c;
        *ctx = nullptr;
        return ffx::RC_OK;
    }
    Provider* p = Owner(ctx);
    if (!p) return ffx::RC_PARAMETER;
    void* key = ctx ? *ctx : nullptr;
    ffx::ReturnCode rc = p->destroy(ctx, memCb);
    std::lock_guard<std::mutex> l(g_mtx);
    g_owner.erase(key);
    g_fgContexts.erase(key);
    return rc;
}

__declspec(dllexport) ffx::ReturnCode ffxConfigure(ffx::Context* ctx, const ffx::Header* desc)
{
    if (!Ready()) return ffx::RC_NO_PROVIDER;
    if (!desc) return ffx::RC_PARAMETER;
    if (SwapCtx* c = Ours(ctx)) {
        if (desc->type == ffx::CONFIGURE_FGSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
            auto* d = (const ffx::ConfigureFgSwapChainRegisterUiResourceDX12*)desc;
            c->sc->SetUiResource(d->uiResource, d->flags);
            return ffx::RC_OK;
        }
        if (desc->type == ffx::CONFIGURE_FGSWAPCHAIN_KEYVALUE_DX12) {
            auto* d = (const ffx::ConfigureFgSwapChainKeyValueDX12*)desc;
            if (d->key == ffx::FGSC_KEY_WAITCALLBACK) c->sc->SetWaitCallback(d->ptr);
            else if (d->key == ffx::FGSC_KEY_FRAMEPACINGTUNING) Log("game set AMD frame pacing tuning (ignored: fgswap paces)");
            return ffx::RC_OK;
        }
        return ffx::RC_UNKNOWN_DESCTYPE;
    }
    if (!ctx || !*ctx) {  // global configuration (e.g. debug callback): give it to every provider
        ffx::ReturnCode best = ffx::RC_NO_PROVIDER;
        for (auto& p : g_providers) {
            ffx::ReturnCode rc = p.configure(ctx, desc);
            if (rc == ffx::RC_OK) best = rc;
            else if (best != ffx::RC_OK) best = rc;
        }
        return best;
    }
    Provider* p = Owner(ctx);
    if (!p) return ffx::RC_PARAMETER;
    if (desc->type == ffx::CONFIGURE_FRAMEGENERATION && IsFgContext(ctx)) return ConfigureFg(p, ctx, desc);
    return p->configure(ctx, desc);
}

__declspec(dllexport) ffx::ReturnCode ffxQuery(ffx::Context* ctx, ffx::Header* desc)
{
    if (!Ready()) return ffx::RC_NO_PROVIDER;
    if (!desc) return ffx::RC_PARAMETER;
    if (SwapCtx* c = Ours(ctx)) {
        switch (desc->type) {
        case ffx::QUERY_FGSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12:
            *((ffx::QueryFgSwapChainInterpolationCommandListDX12*)desc)->pOutCommandList = c->sc->InterpolationCommandList();
            return ffx::RC_OK;
        case ffx::QUERY_FGSWAPCHAIN_INTERPOLATIONTEXTURE_DX12:
            *((ffx::QueryFgSwapChainInterpolationTextureDX12*)desc)->pOutTexture = c->sc->InterpolationTexture();
            return ffx::RC_OK;
        case ffx::QUERY_FGSWAPCHAIN_GPU_MEMORY_USAGE_DX12: {
            auto* u = ((ffx::QueryFgSwapChainGpuMemoryUsageDX12*)desc)->usage;
            if (u) { u->totalUsageInBytes = c->sc->GpuMemoryBytes(); u->aliasableUsageInBytes = 0; }
            return ffx::RC_OK;
        }
        case ffx::QUERY_GET_PROVIDER_VERSION: {
            auto* v = (ffx::QueryGetProviderVersion*)desc;
            v->versionId = 0x46475357000002ull;
            v->versionName = "fgswap 0.2";
            return ffx::RC_OK;
        }
        default:
            return ffx::RC_UNKNOWN_DESCTYPE;
        }
    }
    if (ctx && *ctx) {
        Provider* p = Owner(ctx);
        return p ? p->query(ctx, desc) : ffx::RC_PARAMETER;
    }
    // context-free queries; GET_VERSIONS names the effect in createDescType
    uint32_t effect = ffx::EffectOf(desc->type);
    if (desc->type == 4u) effect = ffx::EffectOf(*(const uint64_t*)((const char*)desc + sizeof(ffx::Header)));
    return TryProviders(effect, [&](Provider& p) { return p.query(ctx, desc); });
}

__declspec(dllexport) ffx::ReturnCode ffxDispatch(ffx::Context* ctx, const ffx::Header* desc)
{
    if (!Ready()) return ffx::RC_NO_PROVIDER;
    if (!desc) return ffx::RC_PARAMETER;
    if (SwapCtx* c = Ours(ctx)) {
        if (desc->type == ffx::DISPATCH_FGSWAPCHAIN_WAIT_FOR_PRESENTS_DX12) {
            c->sc->WaitForPresents();
            return ffx::RC_OK;
        }
        return ffx::RC_UNKNOWN_DESCTYPE;
    }
    Provider* p = Owner(ctx);
    return p ? p->dispatch(ctx, desc) : ffx::RC_PARAMETER;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
