#include "LiveStateCapture.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxResources = 128;
constexpr std::size_t kMaxVertexBuffers = 16;
constexpr std::size_t kMaxInputElements = 32;
constexpr std::size_t kMaxStateValues = 256;

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string Utf8String(const std::filesystem::path& value) {
    const auto utf8 = value.u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string PointerString(const void* pointer) {
    if (!pointer)
        return {};
    std::ostringstream stream;
    stream << "0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(pointer);
    return stream.str();
}

void AddMissing(LiveProbeSnapshot& snapshot, std::string value) {
    if (std::find(
            snapshot.missing_fields.begin(),
            snapshot.missing_fields.end(),
            value) == snapshot.missing_fields.end()) {
        snapshot.missing_fields.push_back(std::move(value));
    }
}

void AddLimit(LiveProbeSnapshot& snapshot, const char* value) {
    if (std::find(
            snapshot.limits_hit.begin(),
            snapshot.limits_hit.end(),
            value) == snapshot.limits_hit.end()) {
        snapshot.limits_hit.emplace_back(value);
    }
}

void AddState(
    LiveProbeSnapshot& snapshot,
    std::string name,
    std::string value) {
    if (snapshot.states.size() >= kMaxStateValues) {
        AddLimit(snapshot, "states");
        return;
    }
    snapshot.states.push_back({std::move(name), std::move(value)});
}

std::string TopologyName(D3D11_PRIMITIVE_TOPOLOGY topology) {
    switch (topology) {
    case D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED:
        return "undefined";
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
        return "pointlist";
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
        return "linelist";
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
        return "linestrip";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
        return "trianglelist";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        return "trianglestrip";
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
        return "linelist_adj";
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        return "linestrip_adj";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
        return "trianglelist_adj";
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
        return "trianglestrip_adj";
    case D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
        return "1_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
        return "2_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
        return "3_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
        return "4_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
        return "5_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
        return "6_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
        return "7_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
        return "8_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
        return "9_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
        return "10_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
        return "11_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
        return "12_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
        return "13_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
        return "14_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
        return "15_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
        return "16_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
        return "17_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
        return "18_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
        return "19_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
        return "20_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
        return "21_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
        return "22_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
        return "23_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
        return "24_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
        return "25_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
        return "26_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
        return "27_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
        return "28_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
        return "29_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
        return "30_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
        return "31_control_point_patchlist";
    case D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
        return "32_control_point_patchlist";
    default:
        break;
    }
    return "unknown";
}

std::string InputClassificationName(
    D3D11_INPUT_CLASSIFICATION classification) {
    switch (classification) {
    case D3D11_INPUT_PER_VERTEX_DATA:
        return "per_vertex";
    case D3D11_INPUT_PER_INSTANCE_DATA:
        return "per_instance";
    default:
        break;
    }
    return "unknown";
}

std::string SrvDimensionName(D3D11_SRV_DIMENSION dimension) {
    switch (dimension) {
    case D3D11_SRV_DIMENSION_UNKNOWN:
        return "unknown";
    case D3D11_SRV_DIMENSION_BUFFER:
        return "buffer";
    case D3D11_SRV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        return "texture2dms";
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        return "texture2dmsarray";
    case D3D11_SRV_DIMENSION_TEXTURE3D:
        return "texture3d";
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        return "texturecube";
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
        return "texturecubearray";
    case D3D11_SRV_DIMENSION_BUFFEREX:
        return "bufferex";
    default:
        break;
    }
    return "unknown";
}

std::string RtvDimensionName(D3D11_RTV_DIMENSION dimension) {
    switch (dimension) {
    case D3D11_RTV_DIMENSION_UNKNOWN:
        return "unknown";
    case D3D11_RTV_DIMENSION_BUFFER:
        return "buffer";
    case D3D11_RTV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case D3D11_RTV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case D3D11_RTV_DIMENSION_TEXTURE2DMS:
        return "texture2dms";
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        return "texture2dmsarray";
    case D3D11_RTV_DIMENSION_TEXTURE3D:
        return "texture3d";
    default:
        break;
    }
    return "unknown";
}

std::string DsvDimensionName(D3D11_DSV_DIMENSION dimension) {
    switch (dimension) {
    case D3D11_DSV_DIMENSION_UNKNOWN:
        return "unknown";
    case D3D11_DSV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case D3D11_DSV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        return "texture2dms";
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        return "texture2dmsarray";
    default:
        break;
    }
    return "unknown";
}

std::string UavDimensionName(D3D11_UAV_DIMENSION dimension) {
    switch (dimension) {
    case D3D11_UAV_DIMENSION_UNKNOWN:
        return "unknown";
    case D3D11_UAV_DIMENSION_BUFFER:
        return "buffer";
    case D3D11_UAV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case D3D11_UAV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case D3D11_UAV_DIMENSION_TEXTURE3D:
        return "texture3d";
    default:
        break;
    }
    return "unknown";
}

void DescribeResourceDescriptor(
    ID3D11Resource* resource,
    ProbeResourceInfo& info,
    LiveProbeSnapshot& snapshot,
    const std::string& field_prefix) {
    if (!resource) {
        AddMissing(snapshot, field_prefix + ".resource");
        return;
    }

    info.resource_pointer =
        reinterpret_cast<std::uintptr_t>(resource);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    info.resource_dimension = ProbeResourceDimensionName(dimension);
    if (info.resource_dimension == "unknown") {
        AddMissing(snapshot, field_prefix + ".resource_dimension");
    }

    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
        ComPtr<ID3D11Buffer> buffer;
        D3D11_BUFFER_DESC description = {};
        if (FAILED(resource->QueryInterface(
                IID_PPV_ARGS(buffer.GetAddressOf())))) {
            AddMissing(snapshot, field_prefix + ".buffer_desc");
            break;
        }
        buffer->GetDesc(&description);
        info.bind_flags = description.BindFlags;
        info.usage = description.Usage;
        info.byte_width = description.ByteWidth;
        info.structure_stride = description.StructureByteStride;
        info.width = description.ByteWidth;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        ComPtr<ID3D11Texture1D> texture;
        D3D11_TEXTURE1D_DESC description = {};
        if (FAILED(resource->QueryInterface(
                IID_PPV_ARGS(texture.GetAddressOf())))) {
            AddMissing(snapshot, field_prefix + ".texture1d_desc");
            break;
        }
        texture->GetDesc(&description);
        info.format = ProbeFormatName(description.Format);
        info.width = description.Width;
        info.height = 1;
        info.depth = 1;
        info.mip_levels = description.MipLevels;
        info.array_size = description.ArraySize;
        info.bind_flags = description.BindFlags;
        info.usage = description.Usage;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC description = {};
        if (FAILED(resource->QueryInterface(
                IID_PPV_ARGS(texture.GetAddressOf())))) {
            AddMissing(snapshot, field_prefix + ".texture2d_desc");
            break;
        }
        texture->GetDesc(&description);
        info.format = ProbeFormatName(description.Format);
        info.width = description.Width;
        info.height = description.Height;
        info.depth = 1;
        info.mip_levels = description.MipLevels;
        info.array_size = description.ArraySize;
        info.sample_count = description.SampleDesc.Count;
        info.bind_flags = description.BindFlags;
        info.usage = description.Usage;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        ComPtr<ID3D11Texture3D> texture;
        D3D11_TEXTURE3D_DESC description = {};
        if (FAILED(resource->QueryInterface(
                IID_PPV_ARGS(texture.GetAddressOf())))) {
            AddMissing(snapshot, field_prefix + ".texture3d_desc");
            break;
        }
        texture->GetDesc(&description);
        info.format = ProbeFormatName(description.Format);
        info.width = description.Width;
        info.height = description.Height;
        info.depth = description.Depth;
        info.mip_levels = description.MipLevels;
        info.bind_flags = description.BindFlags;
        info.usage = description.Usage;
        break;
    }
    default:
        AddMissing(snapshot, field_prefix + ".resource_desc");
        break;
    }
}

bool ReserveResourceSlot(
    LiveProbeSnapshot& snapshot,
    ProbeResourceInfo& info) {
    if (snapshot.resources.size() >= kMaxResources) {
        AddLimit(snapshot, "resources");
        return false;
    }
    snapshot.resources.push_back(std::move(info));
    return true;
}

void CaptureShaderResource(
    LiveProbeSnapshot& snapshot,
    const char* stage,
    std::uint32_t slot,
    ID3D11ShaderResourceView* view) {
    if (!view)
        return;
    ProbeResourceInfo info;
    info.stage = stage;
    info.slot = slot;
    info.view_pointer =
        reinterpret_cast<std::uintptr_t>(view);
    const auto position = snapshot.resources.size();
    if (!ReserveResourceSlot(snapshot, info))
        return;

    auto& added = snapshot.resources[position];
    D3D11_SHADER_RESOURCE_VIEW_DESC description = {};
    view->GetDesc(&description);
    added.view_dimension =
        SrvDimensionName(description.ViewDimension);
    added.format = ProbeFormatName(description.Format);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    if (!resource) {
        AddMissing(
            snapshot,
            "resources." + std::to_string(position) + ".resource");
    } else {
        DescribeResourceDescriptor(
            resource.Get(),
            added,
            snapshot,
            "resources." + std::to_string(position));
    }
}

void CaptureRenderTarget(
    LiveProbeSnapshot& snapshot,
    std::uint32_t slot,
    ID3D11RenderTargetView* view) {
    if (!view)
        return;
    ProbeResourceInfo info;
    info.stage = "rt";
    info.slot = slot;
    info.view_pointer =
        reinterpret_cast<std::uintptr_t>(view);
    const auto position = snapshot.resources.size();
    if (!ReserveResourceSlot(snapshot, info))
        return;

    auto& added = snapshot.resources[position];
    D3D11_RENDER_TARGET_VIEW_DESC description = {};
    view->GetDesc(&description);
    added.view_dimension =
        RtvDimensionName(description.ViewDimension);
    added.format = ProbeFormatName(description.Format);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    if (!resource) {
        AddMissing(
            snapshot,
            "resources." + std::to_string(position) + ".resource");
    } else {
        DescribeResourceDescriptor(
            resource.Get(),
            added,
            snapshot,
            "resources." + std::to_string(position));
    }
}

void CaptureDepthTarget(
    LiveProbeSnapshot& snapshot,
    ID3D11DepthStencilView* view) {
    if (!view)
        return;
    ProbeResourceInfo info;
    info.stage = "depth";
    info.view_pointer =
        reinterpret_cast<std::uintptr_t>(view);
    const auto position = snapshot.resources.size();
    if (!ReserveResourceSlot(snapshot, info))
        return;

    auto& added = snapshot.resources[position];
    D3D11_DEPTH_STENCIL_VIEW_DESC description = {};
    view->GetDesc(&description);
    added.view_dimension =
        DsvDimensionName(description.ViewDimension);
    added.format = ProbeFormatName(description.Format);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    if (!resource) {
        AddMissing(
            snapshot,
            "resources." + std::to_string(position) + ".resource");
    } else {
        DescribeResourceDescriptor(
            resource.Get(),
            added,
            snapshot,
            "resources." + std::to_string(position));
    }
}

void CaptureUnorderedAccess(
    LiveProbeSnapshot& snapshot,
    const char* stage,
    std::uint32_t slot,
    ID3D11UnorderedAccessView* view) {
    if (!view)
        return;
    ProbeResourceInfo info;
    info.stage = stage;
    info.slot = slot;
    info.view_pointer =
        reinterpret_cast<std::uintptr_t>(view);
    const auto position = snapshot.resources.size();
    if (!ReserveResourceSlot(snapshot, info))
        return;

    auto& added = snapshot.resources[position];
    D3D11_UNORDERED_ACCESS_VIEW_DESC description = {};
    view->GetDesc(&description);
    added.view_dimension =
        UavDimensionName(description.ViewDimension);
    added.format = ProbeFormatName(description.Format);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    if (!resource) {
        AddMissing(
            snapshot,
            "resources." + std::to_string(position) + ".resource");
    } else {
        DescribeResourceDescriptor(
            resource.Get(),
            added,
            snapshot,
            "resources." + std::to_string(position));
    }
}

void CaptureVertexBuffer(
    LiveProbeSnapshot& snapshot,
    std::uint32_t slot,
    ID3D11Buffer* buffer,
    UINT stride,
    UINT offset) {
    if (!buffer)
        return;
    if (snapshot.vertex_buffers.size() >= kMaxVertexBuffers) {
        AddLimit(snapshot, "vertex_buffers");
        return;
    }

    ProbeBufferBinding binding;
    binding.role = "vertex";
    binding.slot = slot;
    binding.pointer =
        reinterpret_cast<std::uintptr_t>(buffer);
    binding.offset = offset;
    binding.stride = stride;
    D3D11_BUFFER_DESC description = {};
    buffer->GetDesc(&description);
    binding.format = ProbeFormatName(DXGI_FORMAT_UNKNOWN);
    binding.byte_width = description.ByteWidth;
    binding.bind_flags = description.BindFlags;
    binding.usage = description.Usage;
    snapshot.vertex_buffers.push_back(std::move(binding));
}

void CaptureIndexBuffer(
    LiveProbeSnapshot& snapshot,
    ID3D11Buffer* buffer,
    DXGI_FORMAT format,
    UINT offset) {
    if (!buffer)
        return;
    ProbeBufferBinding binding;
    binding.role = "index";
    binding.pointer =
        reinterpret_cast<std::uintptr_t>(buffer);
    binding.format = ProbeFormatName(format);
    binding.offset = offset;
    D3D11_BUFFER_DESC description = {};
    buffer->GetDesc(&description);
    binding.byte_width = description.ByteWidth;
    binding.bind_flags = description.BindFlags;
    binding.usage = description.Usage;
    snapshot.index_buffer = std::move(binding);
}

template <std::size_t N>
void ReleaseBindings(std::array<ID3D11ShaderResourceView*, N>& views) {
    for (auto*& view : views) {
        if (view) {
            view->Release();
            view = nullptr;
        }
    }
}

template <std::size_t N>
void ReleaseBindings(std::array<ID3D11UnorderedAccessView*, N>& views) {
    for (auto*& view : views) {
        if (view) {
            view->Release();
            view = nullptr;
        }
    }
}

template <std::size_t N>
void ReleaseBindings(std::array<ID3D11RenderTargetView*, N>& views) {
    for (auto*& view : views) {
        if (view) {
            view->Release();
            view = nullptr;
        }
    }
}

template <std::size_t N>
void ReleaseBindings(std::array<ID3D11SamplerState*, N>& samplers) {
    for (auto*& sampler : samplers) {
        if (sampler) {
            sampler->Release();
            sampler = nullptr;
        }
    }
}

template <std::size_t N>
void ReleaseBindings(std::array<ID3D11Buffer*, N>& buffers) {
    for (auto*& buffer : buffers) {
        if (buffer) {
            buffer->Release();
            buffer = nullptr;
        }
    }
}

template <typename Getter>
void CaptureSrvStage(
    LiveProbeSnapshot& snapshot,
    const char* stage,
    Getter getter) {
    std::array<
        ID3D11ShaderResourceView*,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views = {};
    getter(views.data());
    for (std::uint32_t slot = 0; slot < views.size(); ++slot) {
        CaptureShaderResource(snapshot, stage, slot, views[slot]);
    }
    ReleaseBindings(views);
}

template <typename Getter>
void CaptureSamplerStage(
    LiveProbeSnapshot& snapshot,
    const char* stage,
    Getter getter) {
    std::array<
        ID3D11SamplerState*,
        D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers = {};
    getter(samplers.data());
    UINT count = 0;
    for (std::uint32_t slot = 0; slot < samplers.size(); ++slot) {
        if (!samplers[slot])
            continue;
        ++count;
        AddState(
            snapshot,
            std::string(stage) + ".sampler." + std::to_string(slot),
            PointerString(samplers[slot]));
    }
    AddState(
        snapshot,
        std::string(stage) + ".sampler_count",
        std::to_string(count));
    ReleaseBindings(samplers);
}

std::string BlendStateDescription(
    ID3D11BlendState* state,
    const FLOAT blend_factor[4],
    UINT sample_mask) {
    if (!state)
        return "unbound";
    D3D11_BLEND_DESC description = {};
    state->GetDesc(&description);
    std::ostringstream stream;
    stream << "alpha_to_coverage="
           << (description.AlphaToCoverageEnable != FALSE)
           << ";independent_blend="
           << (description.IndependentBlendEnable != FALSE)
           << ";sample_mask=" << sample_mask
           << ";factor=" << blend_factor[0] << ','
           << blend_factor[1] << ','
           << blend_factor[2] << ','
           << blend_factor[3];
    for (UINT index = 0;
         index < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
         ++index) {
        const auto& target = description.RenderTarget[index];
        stream << ";rt" << index
               << ".blend=" << (target.BlendEnable != FALSE)
               << ",src=" << target.SrcBlend
               << ",dst=" << target.DestBlend
               << ",op=" << target.BlendOp
               << ",src_alpha=" << target.SrcBlendAlpha
               << ",dst_alpha=" << target.DestBlendAlpha
               << ",op_alpha=" << target.BlendOpAlpha
               << ",write_mask="
               << static_cast<unsigned>(
                      target.RenderTargetWriteMask);
    }
    return stream.str();
}

std::string DepthStencilStateDescription(
    ID3D11DepthStencilState* state,
    UINT stencil_ref) {
    if (!state)
        return "unbound";
    D3D11_DEPTH_STENCIL_DESC description = {};
    state->GetDesc(&description);
    std::ostringstream stream;
    stream << "depth_enable=" << (description.DepthEnable != FALSE)
           << ";depth_write=" << description.DepthWriteMask
           << ";depth_func=" << description.DepthFunc
           << ";stencil_enable="
           << (description.StencilEnable != FALSE)
           << ";stencil_read_mask="
           << static_cast<unsigned>(description.StencilReadMask)
           << ";stencil_write_mask="
           << static_cast<unsigned>(description.StencilWriteMask)
           << ";front_fail="
           << description.FrontFace.StencilFailOp
           << ";front_depth_fail="
           << description.FrontFace.StencilDepthFailOp
           << ";front_pass="
           << description.FrontFace.StencilPassOp
           << ";front_func="
           << description.FrontFace.StencilFunc
           << ";back_fail="
           << description.BackFace.StencilFailOp
           << ";back_depth_fail="
           << description.BackFace.StencilDepthFailOp
           << ";back_pass="
           << description.BackFace.StencilPassOp
           << ";back_func="
           << description.BackFace.StencilFunc
           << ";ref=" << stencil_ref;
    return stream.str();
}

std::string RasterizerStateDescription(
    ID3D11RasterizerState* state) {
    if (!state)
        return "unbound";
    D3D11_RASTERIZER_DESC description = {};
    state->GetDesc(&description);
    std::ostringstream stream;
    stream << "fill=" << description.FillMode
           << ";cull=" << description.CullMode
           << ";front_counter_clockwise="
           << (description.FrontCounterClockwise != FALSE)
           << ";depth_bias=" << description.DepthBias
           << ";depth_bias_clamp=" << description.DepthBiasClamp
           << ";slope_scaled_depth_bias="
           << description.SlopeScaledDepthBias
           << ";depth_clip_enable="
           << (description.DepthClipEnable != FALSE)
           << ";scissor_enable="
           << (description.ScissorEnable != FALSE)
           << ";multisample_enable="
           << (description.MultisampleEnable != FALSE)
           << ";antialiased_line_enable="
           << (description.AntialiasedLineEnable != FALSE);
    return stream.str();
}

bool CaptureImmediateContextImpl(
    ID3D11Device1* device,
    LiveProbeSnapshot& snapshot,
    std::string& error) {
    ComPtr<ID3D11DeviceContext1> context;
    device->GetImmediateContext1(context.GetAddressOf());
    if (!context) {
        ComPtr<ID3D11DeviceContext> base_context;
        device->GetImmediateContext(base_context.GetAddressOf());
        if (!base_context ||
            FAILED(base_context.As(&context)) ||
            !context) {
            error = "could not obtain the D3D11 immediate context";
            return false;
        }
    }

    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "captured context is not immediate";
        return false;
    }
    snapshot.context_pointer =
        reinterpret_cast<std::uintptr_t>(context.Get());
    AddState(
        snapshot,
        "context.type",
        "immediate");
    AddState(
        snapshot,
        "context.flags",
        std::to_string(context->GetContextFlags()));

    D3D11_PRIMITIVE_TOPOLOGY topology =
        D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&topology);
    AddState(
        snapshot,
        "ia.topology",
        TopologyName(topology));

    ComPtr<ID3D11InputLayout> input_layout;
    context->IAGetInputLayout(input_layout.GetAddressOf());
    if (input_layout) {
        AddState(
            snapshot,
            "ia.input_layout",
            PointerString(input_layout.Get()));
        std::array<
            D3D11_INPUT_ELEMENT_DESC,
            D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT>
            elements = {};
        UINT count = 0;
        if (!ProbeHackerInputLayoutElements(
                input_layout.Get(),
                elements.data(),
                elements.size(),
                count)) {
            AddMissing(
                snapshot,
                "input_elements.layout");
            snapshot.warnings.emplace_back(
                "input layout elements could not be read; declaration "
                "omitted");
        } else {
            if (count > kMaxInputElements)
                AddLimit(snapshot, "input_elements");
            const auto available =
                std::min<std::size_t>(count, kMaxInputElements);
            for (std::size_t index = 0; index < available; ++index) {
                const auto& source = elements[index];
                ProbeInputElement element;
                if (source.SemanticName)
                    element.semantic_name = source.SemanticName;
                else
                    AddMissing(
                        snapshot,
                        "input_elements." +
                            std::to_string(index) +
                            ".semantic_name");
                element.semantic_index = source.SemanticIndex;
                element.format = ProbeFormatName(source.Format);
                element.input_slot = source.InputSlot;
                element.aligned_byte_offset =
                    source.AlignedByteOffset;
                element.classification =
                    InputClassificationName(source.InputSlotClass);
                element.instance_data_step_rate =
                    source.InstanceDataStepRate;
                snapshot.input_elements.push_back(
                    std::move(element));
            }
        }
    } else {
        AddMissing(snapshot, "input_layout");
    }

    ComPtr<ID3D11VertexShader> vertex_shader;
    context->VSGetShader(
        vertex_shader.GetAddressOf(), nullptr, nullptr);
    if (vertex_shader) {
        snapshot.shaders.push_back({
            "vs",
            reinterpret_cast<std::uintptr_t>(
                vertex_shader.Get())});
    } else {
        AddMissing(snapshot, "shaders.vs");
    }
    ComPtr<ID3D11HullShader> hull_shader;
    context->HSGetShader(
        hull_shader.GetAddressOf(), nullptr, nullptr);
    if (hull_shader) {
        snapshot.shaders.push_back({
            "hs",
            reinterpret_cast<std::uintptr_t>(
                hull_shader.Get())});
    }
    ComPtr<ID3D11DomainShader> domain_shader;
    context->DSGetShader(
        domain_shader.GetAddressOf(), nullptr, nullptr);
    if (domain_shader) {
        snapshot.shaders.push_back({
            "ds",
            reinterpret_cast<std::uintptr_t>(
                domain_shader.Get())});
    }
    ComPtr<ID3D11GeometryShader> geometry_shader;
    context->GSGetShader(
        geometry_shader.GetAddressOf(), nullptr, nullptr);
    if (geometry_shader) {
        snapshot.shaders.push_back({
            "gs",
            reinterpret_cast<std::uintptr_t>(
                geometry_shader.Get())});
    }
    ComPtr<ID3D11PixelShader> pixel_shader;
    context->PSGetShader(
        pixel_shader.GetAddressOf(), nullptr, nullptr);
    if (pixel_shader) {
        snapshot.shaders.push_back({
            "ps",
            reinterpret_cast<std::uintptr_t>(
                pixel_shader.Get())});
    } else {
        AddMissing(snapshot, "shaders.ps");
    }
    ComPtr<ID3D11ComputeShader> compute_shader;
    context->CSGetShader(
        compute_shader.GetAddressOf(), nullptr, nullptr);
    if (compute_shader) {
        snapshot.shaders.push_back({
            "cs",
            reinterpret_cast<std::uintptr_t>(
                compute_shader.Get())});
    }

    CaptureSrvStage(snapshot, "vs", [&](auto* views) {
        context->VSGetShaderResources(
            0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    });
    CaptureSrvStage(snapshot, "hs", [&](auto* views) {
        context->HSGetShaderResources(
            0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    });
    CaptureSrvStage(snapshot, "ds", [&](auto* views) {
        context->DSGetShaderResources(
            0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    });
    CaptureSrvStage(snapshot, "gs", [&](auto* views) {
        context->GSGetShaderResources(
            0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    });
    CaptureSrvStage(snapshot, "ps", [&](auto* views) {
        context->PSGetShaderResources(
            0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    });
    CaptureSrvStage(snapshot, "cs", [&](auto* views) {
        context->CSGetShaderResources(
            0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
    });

    std::array<
        ID3D11Buffer*,
        D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers = {};
    std::array<
        UINT,
        D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides = {};
    std::array<
        UINT,
        D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> offsets = {};
    context->IAGetVertexBuffers(
        0,
        D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
        vertex_buffers.data(),
        strides.data(),
        offsets.data());
    for (std::uint32_t slot = 0; slot < vertex_buffers.size(); ++slot) {
        CaptureVertexBuffer(
            snapshot,
            slot,
            vertex_buffers[slot],
            strides[slot],
            offsets[slot]);
    }
    ReleaseBindings(vertex_buffers);

    ComPtr<ID3D11Buffer> index_buffer;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    context->IAGetIndexBuffer(
        index_buffer.GetAddressOf(), &index_format, &index_offset);
    CaptureIndexBuffer(
        snapshot,
        index_buffer.Get(),
        index_format,
        index_offset);

    std::array<
        ID3D11RenderTargetView*,
        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets = {};
    std::array<
        ID3D11UnorderedAccessView*,
        D3D11_PS_CS_UAV_REGISTER_COUNT> pixel_uavs = {};
    ComPtr<ID3D11DepthStencilView> depth_target;
    context->OMGetRenderTargetsAndUnorderedAccessViews(
        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
        render_targets.data(),
        depth_target.GetAddressOf(),
        0,
        D3D11_PS_CS_UAV_REGISTER_COUNT,
        pixel_uavs.data());
    UINT render_target_count = 0;
    for (std::uint32_t slot = 0; slot < render_targets.size(); ++slot) {
        if (render_targets[slot]) {
            ++render_target_count;
            CaptureRenderTarget(
                snapshot, slot, render_targets[slot]);
        }
    }
    ReleaseBindings(render_targets);
    if (depth_target) {
        CaptureDepthTarget(snapshot, depth_target.Get());
    }
    UINT pixel_uav_count = 0;
    for (std::uint32_t slot = 0; slot < pixel_uavs.size(); ++slot) {
        if (pixel_uavs[slot]) {
            ++pixel_uav_count;
            CaptureUnorderedAccess(
                snapshot, "ps", slot, pixel_uavs[slot]);
        }
    }
    ReleaseBindings(pixel_uavs);

    std::array<
        ID3D11UnorderedAccessView*,
        D3D11_PS_CS_UAV_REGISTER_COUNT> compute_uavs = {};
    context->CSGetUnorderedAccessViews(
        0,
        D3D11_PS_CS_UAV_REGISTER_COUNT,
        compute_uavs.data());
    UINT compute_uav_count = 0;
    for (std::uint32_t slot = 0; slot < compute_uavs.size(); ++slot) {
        if (compute_uavs[slot]) {
            ++compute_uav_count;
            CaptureUnorderedAccess(
                snapshot, "cs", slot, compute_uavs[slot]);
        }
    }
    ReleaseBindings(compute_uavs);
    AddState(
        snapshot,
        "om.render_target_count",
        std::to_string(render_target_count));
    AddState(
        snapshot,
        "om.ps_uav_count",
        std::to_string(pixel_uav_count));
    AddState(
        snapshot,
        "om.cs_uav_count",
        std::to_string(compute_uav_count));

    CaptureSamplerStage(snapshot, "vs", [&](auto* samplers) {
        context->VSGetSamplers(
            0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    });
    CaptureSamplerStage(snapshot, "hs", [&](auto* samplers) {
        context->HSGetSamplers(
            0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    });
    CaptureSamplerStage(snapshot, "ds", [&](auto* samplers) {
        context->DSGetSamplers(
            0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    });
    CaptureSamplerStage(snapshot, "gs", [&](auto* samplers) {
        context->GSGetSamplers(
            0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    });
    CaptureSamplerStage(snapshot, "ps", [&](auto* samplers) {
        context->PSGetSamplers(
            0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    });
    CaptureSamplerStage(snapshot, "cs", [&](auto* samplers) {
        context->CSGetSamplers(
            0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    });

    ComPtr<ID3D11BlendState> blend_state;
    FLOAT blend_factor[4] = {};
    UINT sample_mask = 0;
    context->OMGetBlendState(
        blend_state.GetAddressOf(), blend_factor, &sample_mask);
    AddState(
        snapshot,
        "om.blend_state",
        BlendStateDescription(
            blend_state.Get(), blend_factor, sample_mask));

    ComPtr<ID3D11DepthStencilState> depth_stencil_state;
    UINT stencil_ref = 0;
    context->OMGetDepthStencilState(
        depth_stencil_state.GetAddressOf(), &stencil_ref);
    AddState(
        snapshot,
        "om.depth_stencil_state",
        DepthStencilStateDescription(
            depth_stencil_state.Get(), stencil_ref));

    ComPtr<ID3D11RasterizerState> rasterizer_state;
    context->RSGetState(rasterizer_state.GetAddressOf());
    AddState(
        snapshot,
        "rs.rasterizer_state",
        RasterizerStateDescription(rasterizer_state.Get()));

    std::array<
        D3D11_VIEWPORT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports = {};
    UINT viewport_count =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&viewport_count, viewports.data());
    AddState(
        snapshot,
        "rs.viewport_count",
        std::to_string(viewport_count));
    std::ostringstream viewport_stream;
    viewport_stream << std::setprecision(6);
    for (UINT index = 0;
         index < std::min<UINT>(viewport_count, 16);
         ++index) {
        const auto& viewport = viewports[index];
        viewport_stream << "x=" << viewport.TopLeftX
                        << ";y=" << viewport.TopLeftY
                        << ";width=" << viewport.Width
                        << ";height=" << viewport.Height
                        << ";min_depth=" << viewport.MinDepth
                        << ";max_depth=" << viewport.MaxDepth;
        AddState(
            snapshot,
            "rs.viewport." + std::to_string(index),
            viewport_stream.str());
        viewport_stream.str({});
        viewport_stream.clear();
        viewport_stream << std::setprecision(6);
    }
    if (viewport_count > 16)
        AddLimit(snapshot, "viewports");

    std::array<
        D3D11_RECT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        scissors = {};
    UINT scissor_count =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetScissorRects(&scissor_count, scissors.data());
    AddState(
        snapshot,
        "rs.scissor_count",
        std::to_string(scissor_count));
    for (UINT index = 0;
         index < std::min<UINT>(scissor_count, 16);
         ++index) {
        const auto& scissor = scissors[index];
        std::ostringstream stream;
        stream << "left=" << scissor.left
               << ";top=" << scissor.top
               << ";right=" << scissor.right
               << ";bottom=" << scissor.bottom;
        AddState(
            snapshot,
            "rs.scissor." + std::to_string(index),
            stream.str());
    }
    if (scissor_count > 16)
        AddLimit(snapshot, "scissors");

    return true;
}

struct CaptureImmediateContextCall {
    ID3D11Device1* device = nullptr;
    LiveProbeSnapshot* snapshot = nullptr;
    std::string* error = nullptr;
};

void SetCaptureStructuredExceptionError(
    std::string* error,
    DWORD exception_code) {
    if (!error)
        return;
    std::ostringstream stream;
    stream << "structured exception 0x" << std::hex
           << exception_code;
    *error = stream.str();
}

bool InvokeCaptureImmediateContext(
    CaptureImmediateContextCall& call) {
    __try {
        return CaptureImmediateContextImpl(
            call.device, *call.snapshot, *call.error);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetCaptureStructuredExceptionError(
            call.error, GetExceptionCode());
        return false;
    }
}

// 3DMigoto exposes the declared elements through non-virtual members of
// HackerInputLayout, so they cannot be called through a probe interface. The
// element vector is read from the object image instead and every field is
// validated first, so an unexpected layout fails closed instead of returning
// garbage.
struct InputElementVector {
    const D3D11_INPUT_ELEMENT_DESC* elements = nullptr;
    std::size_t count = 0;
    std::size_t offset = 0;
};

bool IsReadableRange(const void* address, std::size_t size) {
    if (!address)
        return false;
    auto remaining = size;
    auto current = reinterpret_cast<std::uintptr_t>(address);
    while (remaining != 0) {
        MEMORY_BASIC_INFORMATION information = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(current),
                &information,
                sizeof(information)) == 0) {
            return false;
        }
        if (information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const auto region_end =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress) +
            information.RegionSize;
        if (region_end <= current)
            return false;
        const auto available = region_end - current;
        if (available >= remaining)
            return true;
        remaining -= available;
        current = region_end;
    }
    return true;
}

bool ReadPointerValue(const void* address, std::uintptr_t& value) {
    return ReadProcessMemory(
               GetCurrentProcess(),
               address,
               &value,
               sizeof(value),
               nullptr) != FALSE;
}

bool IsPlausibleInputElement(
    const D3D11_INPUT_ELEMENT_DESC& element) {
    if (!element.SemanticName)
        return false;
    if (!IsReadableRange(element.SemanticName, 2))
        return false;
    std::size_t length = 0;
    while (length < 32) {
        if (!IsReadableRange(element.SemanticName + length, 1))
            return false;
        const auto character =
            static_cast<unsigned char>(element.SemanticName[length]);
        if (character == 0)
            break;
        if (character < 0x20 || character > 0x7E)
            return false;
        ++length;
    }
    if (length == 0 || length >= 32)
        return false;
    if (element.InputSlot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        return false;
    if (element.InputSlotClass > D3D11_INPUT_PER_INSTANCE_DATA)
        return false;
    if (element.InstanceDataStepRate > 0xFFFF)
        return false;
    constexpr int kMaxDeclaredFormat = 200;
    if (element.Format <= DXGI_FORMAT_UNKNOWN ||
        static_cast<int>(element.Format) > kMaxDeclaredFormat) {
        return false;
    }
    return true;
}

bool TryReadInputElementVector(
    const void* layout,
    std::size_t offset,
    InputElementVector& result) {
    __try {
        const auto* base =
            static_cast<const std::uint8_t*>(layout) + offset;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::uintptr_t capacity = 0;
        if (!ReadPointerValue(base, begin) ||
            !ReadPointerValue(base + sizeof(void*), end) ||
            !ReadPointerValue(base + 2 * sizeof(void*), capacity)) {
            return false;
        }
        if (begin == 0 || end < begin || capacity < end)
            return false;
        const auto bytes = static_cast<std::size_t>(end - begin);
        if (bytes == 0 ||
            bytes % sizeof(D3D11_INPUT_ELEMENT_DESC) != 0) {
            return false;
        }
        const auto count = bytes / sizeof(D3D11_INPUT_ELEMENT_DESC);
        if (count > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
            return false;
        const auto* elements =
            reinterpret_cast<const D3D11_INPUT_ELEMENT_DESC*>(begin);
        if (!IsReadableRange(elements, bytes))
            return false;
        for (std::size_t index = 0; index < count; ++index) {
            if (!IsPlausibleInputElement(elements[index]))
                return false;
        }
        result.elements = elements;
        result.count = count;
        result.offset = offset;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FindInputElementVector(
    const void* layout,
    InputElementVector& result) {
    // MSVC x64 image of 3DMigoto's HackerInputLayout: vptr, mRefCount,
    // mOrigLayout, mShaderSignature, mLayoutHash, mElements, mSemanticNames.
    constexpr std::size_t kExpectedElementVectorOffset = 56;
    if (TryReadInputElementVector(
            layout, kExpectedElementVectorOffset, result)) {
        return true;
    }
    for (std::size_t offset = 8; offset <= 112; offset += sizeof(void*)) {
        if (offset == kExpectedElementVectorOffset)
            continue;
        if (TryReadInputElementVector(layout, offset, result))
            return true;
    }
    return false;
}

}  // namespace

std::string ProbeFormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_UNKNOWN:
        return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return "R32G32B32A32_TYPELESS";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_UINT:
        return "R32G32B32A32_UINT";
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return "R32G32B32A32_SINT";
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return "R32G32B32_TYPELESS";
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return "R32G32B32_FLOAT";
    case DXGI_FORMAT_R32G32B32_UINT:
        return "R32G32B32_UINT";
    case DXGI_FORMAT_R32G32B32_SINT:
        return "R32G32B32_SINT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R16G16B16A16_UINT:
        return "R16G16B16A16_UINT";
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return "R16G16B16A16_SNORM";
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return "R16G16B16A16_SINT";
    case DXGI_FORMAT_R32G32_TYPELESS:
        return "R32G32_TYPELESS";
    case DXGI_FORMAT_R32G32_FLOAT:
        return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32_UINT:
        return "R32G32_UINT";
    case DXGI_FORMAT_R32G32_SINT:
        return "R32G32_SINT";
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return "X32_TYPELESS_G8X24_UINT";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return "R10G10B10A2_UINT";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UINT:
        return "R8G8B8A8_UINT";
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return "R8G8B8A8_SNORM";
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return "R8G8B8A8_SINT";
    case DXGI_FORMAT_R16G16_TYPELESS:
        return "R16G16_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:
        return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_UNORM:
        return "R16G16_UNORM";
    case DXGI_FORMAT_R16G16_UINT:
        return "R16G16_UINT";
    case DXGI_FORMAT_R16G16_SNORM:
        return "R16G16_SNORM";
    case DXGI_FORMAT_R16G16_SINT:
        return "R16G16_SINT";
    case DXGI_FORMAT_R32_TYPELESS:
        return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:
        return "D32_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:
        return "R32_FLOAT";
    case DXGI_FORMAT_R32_UINT:
        return "R32_UINT";
    case DXGI_FORMAT_R32_SINT:
        return "R32_SINT";
    case DXGI_FORMAT_R24G8_TYPELESS:
        return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return "X24_TYPELESS_G8_UINT";
    case DXGI_FORMAT_R8G8_TYPELESS:
        return "R8G8_TYPELESS";
    case DXGI_FORMAT_R8G8_UNORM:
        return "R8G8_UNORM";
    case DXGI_FORMAT_R8G8_UINT:
        return "R8G8_UINT";
    case DXGI_FORMAT_R8G8_SNORM:
        return "R8G8_SNORM";
    case DXGI_FORMAT_R8G8_SINT:
        return "R8G8_SINT";
    case DXGI_FORMAT_R16_TYPELESS:
        return "R16_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT:
        return "R16_FLOAT";
    case DXGI_FORMAT_D16_UNORM:
        return "D16_UNORM";
    case DXGI_FORMAT_R16_UNORM:
        return "R16_UNORM";
    case DXGI_FORMAT_R16_UINT:
        return "R16_UINT";
    case DXGI_FORMAT_R16_SNORM:
        return "R16_SNORM";
    case DXGI_FORMAT_R16_SINT:
        return "R16_SINT";
    case DXGI_FORMAT_R8_TYPELESS:
        return "R8_TYPELESS";
    case DXGI_FORMAT_R8_UNORM:
        return "R8_UNORM";
    case DXGI_FORMAT_R8_UINT:
        return "R8_UINT";
    case DXGI_FORMAT_R8_SNORM:
        return "R8_SNORM";
    case DXGI_FORMAT_R8_SINT:
        return "R8_SINT";
    case DXGI_FORMAT_A8_UNORM:
        return "A8_UNORM";
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return "R9G9B9E5_SHAREDEXP";
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
        return "R8G8_B8G8_UNORM";
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
        return "G8R8_G8B8_UNORM";
    case DXGI_FORMAT_BC1_TYPELESS:
        return "BC1_TYPELESS";
    case DXGI_FORMAT_BC1_UNORM:
        return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC2_TYPELESS:
        return "BC2_TYPELESS";
    case DXGI_FORMAT_BC2_UNORM:
        return "BC2_UNORM";
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return "BC2_UNORM_SRGB";
    case DXGI_FORMAT_BC3_TYPELESS:
        return "BC3_TYPELESS";
    case DXGI_FORMAT_BC3_UNORM:
        return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_BC4_TYPELESS:
        return "BC4_TYPELESS";
    case DXGI_FORMAT_BC4_UNORM:
        return "BC4_UNORM";
    case DXGI_FORMAT_BC4_SNORM:
        return "BC4_SNORM";
    case DXGI_FORMAT_BC5_TYPELESS:
        return "BC5_TYPELESS";
    case DXGI_FORMAT_BC5_UNORM:
        return "BC5_UNORM";
    case DXGI_FORMAT_BC5_SNORM:
        return "BC5_SNORM";
    case DXGI_FORMAT_B5G6R5_UNORM:
        return "B5G6R5_UNORM";
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return "B5G5R5A1_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        return "R10G10B10_XR_BIAS_A2_UNORM";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return "B8G8R8X8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return "B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_BC6H_TYPELESS:
        return "BC6H_TYPELESS";
    case DXGI_FORMAT_BC6H_UF16:
        return "BC6H_UF16";
    case DXGI_FORMAT_BC6H_SF16:
        return "BC6H_SF16";
    case DXGI_FORMAT_BC7_TYPELESS:
        return "BC7_TYPELESS";
    case DXGI_FORMAT_BC7_UNORM:
        return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return "BC7_UNORM_SRGB";
    default:
        break;
    }
    return "unknown";
}

std::string ProbeResourceDimensionName(D3D11_RESOURCE_DIMENSION dimension) {
    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER:
        return "buffer";
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        return "texture3d";
    default:
        break;
    }
    return "unknown";
}

std::string ProbeSnapshotDirectory(const std::string& analysis_root) {
    if (analysis_root.empty())
        return {};
    return Utf8String(
        Utf8Path(analysis_root) / L"xxmi_mcp_snapshots");
}

bool RetainNewestProbeSnapshots(
    const std::filesystem::path& directory,
    std::size_t keep,
    std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    if (!std::filesystem::exists(directory, filesystem_error)) {
        if (filesystem_error) {
            error = filesystem_error.message();
            return false;
        }
        return true;
    }

    struct SnapshotFile {
        std::filesystem::file_time_type write_time;
        std::filesystem::path path;
        std::string name;
    };
    std::vector<SnapshotFile> snapshots;
    std::filesystem::directory_iterator iterator(
        directory, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    for (const auto& entry : iterator) {
        if (!entry.is_regular_file(filesystem_error)) {
            filesystem_error.clear();
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("live-", 0) != 0 ||
            entry.path().extension() != ".json") {
            continue;
        }
        const auto write_time =
            entry.last_write_time(filesystem_error);
        if (filesystem_error) {
            filesystem_error.clear();
            continue;
        }
        snapshots.push_back({write_time, entry.path(), name});
    }

    std::sort(
        snapshots.begin(),
        snapshots.end(),
        [] (const SnapshotFile& left, const SnapshotFile& right) {
            if (left.write_time != right.write_time)
                return left.write_time > right.write_time;
            return left.name > right.name;
        });

    for (std::size_t index = keep;
         index < snapshots.size();
         ++index) {
        std::filesystem::remove(snapshots[index].path, filesystem_error);
        if (filesystem_error) {
            error =
                "could not remove old snapshot " +
                snapshots[index].name + ": " +
                filesystem_error.message();
            return false;
        }
    }
    return true;
}

bool ProbeHackerInputLayoutElements(
    const ID3D11InputLayout* layout,
    D3D11_INPUT_ELEMENT_DESC* elements,
    std::size_t capacity,
    UINT& count) {
    count = 0;
    if (!layout)
        return false;
    InputElementVector found;
    if (!FindInputElementVector(layout, found))
        return false;
    const auto available = std::min<std::size_t>(found.count, capacity);
    if (available != 0) {
        if (!elements)
            return false;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                found.elements,
                elements,
                available * sizeof(D3D11_INPUT_ELEMENT_DESC),
                nullptr)) {
            return false;
        }
    }
    count = static_cast<UINT>(available);
    return true;
}

bool CaptureImmediateContext(
    ID3D11Device1* device,
    LiveProbeSnapshot& snapshot,
    std::string& error) {
    error.clear();
    if (!device) {
        error = "D3D11 device is null";
        return false;
    }
    CaptureImmediateContextCall call = {
        device,
        &snapshot,
        &error,
    };
    return InvokeCaptureImmediateContext(call);
}
