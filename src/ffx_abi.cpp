// ffx_abi.cpp - format and resource helpers for our FFX API declarations.
#include "ffx_abi.h"

namespace ffx {

uint32_t SurfaceFormatFromDxgi(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return FMT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_UINT: return FMT_R32G32B32A32_UINT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return FMT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return FMT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FMT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32_FLOAT: return FMT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return FMT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32_FLOAT: return FMT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32_UINT: return FMT_R32G32_UINT;
    case DXGI_FORMAT_R8_UINT: return FMT_R8_UINT;
    case DXGI_FORMAT_R32_UINT: return FMT_R32_UINT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return FMT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return FMT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return FMT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return FMT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return FMT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return FMT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return FMT_B8G8R8A8_SRGB;
    case DXGI_FORMAT_R11G11B10_FLOAT: return FMT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return FMT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return FMT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return FMT_R16G16_TYPELESS;
    case DXGI_FORMAT_R16G16_FLOAT: return FMT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16_UINT: return FMT_R16G16_UINT;
    case DXGI_FORMAT_R16G16_SINT: return FMT_R16G16_SINT;
    case DXGI_FORMAT_R16_TYPELESS: return FMT_R16_TYPELESS;
    case DXGI_FORMAT_R16_FLOAT: return FMT_R16_FLOAT;
    case DXGI_FORMAT_R16_UINT: return FMT_R16_UINT;
    case DXGI_FORMAT_R16_UNORM: return FMT_R16_UNORM;
    case DXGI_FORMAT_R16_SNORM: return FMT_R16_SNORM;
    case DXGI_FORMAT_R8_TYPELESS: return FMT_R8_TYPELESS;
    case DXGI_FORMAT_R8_UNORM: return FMT_R8_UNORM;
    case DXGI_FORMAT_R8_SNORM: return FMT_R8_SNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return FMT_R8G8_TYPELESS;
    case DXGI_FORMAT_R8G8_UNORM: return FMT_R8G8_UNORM;
    case DXGI_FORMAT_R8G8_UINT: return FMT_R8G8_UINT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: return FMT_R32_FLOAT;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return FMT_R9G9B9E5_SHAREDEXP;
    default: return FMT_UNKNOWN;
    }
}

DXGI_FORMAT DxgiFromSurfaceFormat(uint32_t f)
{
    switch (f) {
    case FMT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case FMT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case FMT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case FMT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case FMT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case FMT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case FMT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case FMT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

Resource MakeResource(ID3D12Resource* r, uint32_t state)
{
    Resource res = {};
    res.resource = r;
    res.state = state;
    if (!r) return res;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        res.description.type = TYPE_BUFFER;
        res.description.width = (uint32_t)d.Width;
        res.description.usage = USAGE_UAV;
        return res;
    }
    res.description.type = TYPE_TEXTURE2D;
    res.description.format = SurfaceFormatFromDxgi(d.Format);
    res.description.width = (uint32_t)d.Width;
    res.description.height = d.Height;
    res.description.depth = d.DepthOrArraySize;
    res.description.mipCount = d.MipLevels;
    res.description.usage = (d.Format == DXGI_FORMAT_D32_FLOAT || d.Format == DXGI_FORMAT_D16_UNORM) ? USAGE_DEPTHTARGET : USAGE_READ_ONLY;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) res.description.usage |= USAGE_UAV;
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) res.description.usage |= USAGE_RENDERTARGET;
    return res;
}

}  // namespace ffx
