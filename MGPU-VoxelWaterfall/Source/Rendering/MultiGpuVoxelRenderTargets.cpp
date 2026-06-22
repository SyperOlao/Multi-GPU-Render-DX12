#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"

#include "GDevice.h"
#include "d3dUtil.h"

#include <algorithm>
#include <cfloat>

using namespace PEPEngine::Graphics;

namespace
{
    constexpr float SecondaryColorClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    constexpr float LinearDepthClear[4] = {FLT_MAX, 0.0f, 0.0f, 0.0f};

    std::wstring FrameName(const wchar_t* baseName, const UINT frameIndex)
    {
        return std::wstring(baseName) + L" Frame " + std::to_wstring(frameIndex);
    }
}

bool MultiGpuVoxelRenderTargets::SupportsFormat(
    const std::shared_ptr<GDevice>& device,
    const DXGI_FORMAT format,
    const D3D12_FORMAT_SUPPORT1 requiredSupport)
{
    if (!device)
        return false;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    if (FAILED(device->GetDXDevice()->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
    {
        return false;
    }

    return (support.Support1 & requiredSupport) == requiredSupport;
}

D3D12_RESOURCE_DESC MultiGpuVoxelRenderTargets::Texture2DDesc(
    const UINT width,
    const UINT height,
    const DXGI_FORMAT format,
    const D3D12_RESOURCE_FLAGS flags,
    const D3D12_TEXTURE_LAYOUT layout)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = std::max(1u, width);
    desc.Height = std::max(1u, height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = layout;
    desc.Flags = flags;
    return desc;
}

UINT64 MultiGpuVoxelRenderTargets::CopyableTextureBytes(
    const std::shared_ptr<GDevice>& device,
    const D3D12_RESOURCE_DESC& desc)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    device->GetDXDevice()->GetCopyableFootprints(
        &desc, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &totalBytes);
    return totalBytes;
}

void MultiGpuVoxelRenderTargets::AppendResourceInfo(
    const std::wstring& name,
    const std::shared_ptr<GDevice>& owner,
    const DXGI_FORMAT format,
    const UINT width,
    const UINT height,
    const D3D12_RESOURCE_FLAGS flags,
    const D3D12_RESOURCE_STATES initialState)
{
    resourceTable.push_back({
        name,
        owner ? owner->GetName() : L"unavailable",
        format,
        width,
        height,
        flags,
        initialState
    });
}

bool MultiGpuVoxelRenderTargets::ValidateCapabilities(
    const std::shared_ptr<GDevice>& primaryDevice,
    const std::shared_ptr<GDevice>& secondaryDevice,
    const MultiGpuVoxelRenderTargetDesc& desc)
{
    failureMessage.clear();

    if (!primaryDevice)
    {
        failureMessage = L"MultiGpu unavailable: primary device is not initialized";
        return false;
    }

    if (!secondaryDevice || secondaryDevice == primaryDevice)
    {
        failureMessage = L"MultiGpu unavailable: distinct secondary hardware adapter was not found";
        return false;
    }

    if (!secondaryDevice->GetCommandQueue(GQueueType::Graphics))
    {
        failureMessage = L"MultiGpu unavailable: secondary adapter graphics queue is not available";
        return false;
    }

    if (!primaryDevice->IsCrossAdapterTextureSupported() || !secondaryDevice->IsCrossAdapterTextureSupported())
    {
        failureMessage = L"MultiGpu unavailable: cross-adapter row-major texture path is not supported";
        return false;
    }

    if (desc.Width == 0 || desc.Height == 0 || desc.FrameCount == 0)
    {
        failureMessage = L"MultiGpu unavailable: invalid secondary render target dimensions or frame count";
        return false;
    }

    if (!SupportsFormat(secondaryDevice, desc.ColorFormat,
                        D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
    {
        failureMessage = L"MultiGpu unavailable: secondary adapter does not support color RT/SRV format";
        return false;
    }

    if (!SupportsFormat(secondaryDevice, desc.LinearDepthFormat,
                        D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
    {
        failureMessage = L"MultiGpu unavailable: secondary adapter does not support R32_FLOAT linear-depth RT/SRV format";
        return false;
    }

    if (!SupportsFormat(secondaryDevice, desc.DepthStencilFormat, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL))
    {
        failureMessage = L"MultiGpu unavailable: secondary adapter does not support required local DSV format";
        return false;
    }

    if (!SupportsFormat(primaryDevice, desc.ColorFormat, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
    {
        failureMessage = L"MultiGpu unavailable: primary adapter does not support received color SRV format";
        return false;
    }

    if (!SupportsFormat(primaryDevice, desc.LinearDepthFormat, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
    {
        failureMessage = L"MultiGpu unavailable: primary adapter does not support received R32_FLOAT linear-depth SRV format";
        return false;
    }

    try
    {
        auto colorDesc = Texture2DDesc(desc.Width, desc.Height, desc.ColorFormat, D3D12_RESOURCE_FLAG_NONE);
        GCrossAdapterResource colorBridge(colorDesc, primaryDevice, secondaryDevice, L"Voxel Capability Color Bridge");
        auto depthDesc = Texture2DDesc(desc.Width, desc.Height, desc.LinearDepthFormat, D3D12_RESOURCE_FLAG_NONE);
        GCrossAdapterResource depthBridge(depthDesc, primaryDevice, secondaryDevice, L"Voxel Capability Depth Bridge");
    }
    catch (...)
    {
        failureMessage = L"MultiGpu unavailable: failed to allocate/open cross-adapter color or linear-depth bridge textures";
        return false;
    }

    return true;
}

bool MultiGpuVoxelRenderTargets::Initialize(
    const std::shared_ptr<GDevice>& primaryDevice,
    const std::shared_ptr<GDevice>& secondaryDevice,
    const MultiGpuVoxelRenderTargetDesc& desc)
{
    Reset();

    if (!ValidateCapabilities(primaryDevice, secondaryDevice, desc))
        return false;

    currentDesc = desc;
    frames.resize(desc.FrameCount);

    for (UINT frameIndex = 0; frameIndex < desc.FrameCount; ++frameIndex)
    {
        auto& frame = frames[frameIndex];
        frame.FrameIndex = frameIndex;

        frame.SecondaryRtvDescriptors = secondaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
        frame.SecondaryDsvDescriptor = secondaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
        frame.PrimarySrvDescriptors = primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
        frame.PrimaryCompositeDescriptors =
            primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5);
        frame.PrimaryCompositeRtvDescriptor =
            primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);

        const auto colorName = FrameName(L"SecondaryLocalColor", frameIndex);
        const auto localColorDesc = Texture2DDesc(desc.Width, desc.Height, desc.ColorFormat,
                                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto colorClear = CD3DX12_CLEAR_VALUE(desc.ColorFormat, SecondaryColorClear);
        frame.SecondaryLocalColor = GTexture(secondaryDevice, localColorDesc, colorName,
                                             TextureUsage::RenderTarget, &colorClear);

        const auto linearDepthName = FrameName(L"SecondaryLocalLinearDepth", frameIndex);
        const auto linearDepthDesc = Texture2DDesc(desc.Width, desc.Height, desc.LinearDepthFormat,
                                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto linearDepthClear = CD3DX12_CLEAR_VALUE(desc.LinearDepthFormat, LinearDepthClear);
        frame.SecondaryLocalLinearDepth = GTexture(secondaryDevice, linearDepthDesc, linearDepthName,
                                                  TextureUsage::RenderTarget, &linearDepthClear);

        const auto dsvName = FrameName(L"SecondaryLocalDepthStencil", frameIndex);
        const auto dsvDesc = Texture2DDesc(desc.Width, desc.Height, desc.DepthStencilFormat,
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE dsvClear{};
        dsvClear.Format = desc.DepthStencilFormat;
        dsvClear.DepthStencil.Depth = 1.0f;
        dsvClear.DepthStencil.Stencil = 0;
        frame.SecondaryLocalDepthStencil = GTexture(secondaryDevice, dsvDesc, dsvName, TextureUsage::Depth, &dsvClear);

        auto bridgeColorDesc = Texture2DDesc(desc.Width, desc.Height, desc.ColorFormat, D3D12_RESOURCE_FLAG_NONE);
        frame.ColorTransferBytes = CopyableTextureBytes(primaryDevice, bridgeColorDesc);
        frame.CrossAdapterColor = std::make_shared<GCrossAdapterResource>(
            bridgeColorDesc, primaryDevice, secondaryDevice, FrameName(L"CrossAdapterSecondaryColor", frameIndex));

        auto bridgeDepthDesc = Texture2DDesc(desc.Width, desc.Height, desc.LinearDepthFormat, D3D12_RESOURCE_FLAG_NONE);
        frame.LinearDepthTransferBytes = CopyableTextureBytes(primaryDevice, bridgeDepthDesc);
        frame.CrossAdapterLinearDepth = std::make_shared<GCrossAdapterResource>(
            bridgeDepthDesc, primaryDevice, secondaryDevice, FrameName(L"CrossAdapterSecondaryLinearDepth", frameIndex));

        const auto receivedColorName = FrameName(L"PrimaryReceivedSecondaryColor", frameIndex);
        const auto receivedColorDesc = Texture2DDesc(desc.Width, desc.Height, desc.ColorFormat,
                                                     D3D12_RESOURCE_FLAG_NONE);
        frame.PrimaryReceivedSecondaryColor = GTexture(primaryDevice, receivedColorDesc, receivedColorName,
                                                       TextureUsage::Albedo);

        const auto receivedDepthName = FrameName(L"PrimaryReceivedSecondaryLinearDepth", frameIndex);
        const auto receivedDepthDesc = Texture2DDesc(desc.Width, desc.Height, desc.LinearDepthFormat,
                                                     D3D12_RESOURCE_FLAG_NONE);
        frame.PrimaryReceivedSecondaryLinearDepth = GTexture(primaryDevice, receivedDepthDesc, receivedDepthName,
                                                              TextureUsage::Albedo);

        const auto compositeColorName = FrameName(L"PrimaryCompositeColor", frameIndex);
        const auto compositeColorDesc = Texture2DDesc(desc.Width, desc.Height, desc.ColorFormat,
                                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto compositeClear = CD3DX12_CLEAR_VALUE(desc.ColorFormat, SecondaryColorClear);
        frame.PrimaryCompositeColor = GTexture(primaryDevice, compositeColorDesc, compositeColorName,
                                               TextureUsage::RenderTarget, &compositeClear);

        D3D12_QUERY_HEAP_DESC pipelineStatsHeapDesc{};
        pipelineStatsHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        pipelineStatsHeapDesc.Count = 1;
        pipelineStatsHeapDesc.NodeMask = secondaryDevice->GetNodeMask();
        ThrowIfFailed(secondaryDevice->GetDXDevice()->CreateQueryHeap(
            &pipelineStatsHeapDesc,
            IID_PPV_ARGS(&frame.SecondaryPipelineStatsQueryHeap)));
        frame.SecondaryPipelineStatsReadback = GResource(
            secondaryDevice,
            CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)),
            FrameName(L"SecondaryPipelineStatsReadback", frameIndex),
            nullptr,
            D3D12_RESOURCE_STATE_COPY_DEST,
            CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK));

        frame.ExpectedLocalToSharedColorBytes =
            CopyableTextureBytes(secondaryDevice, frame.SecondaryLocalColor.GetD3D12ResourceDesc());
        frame.ExpectedLocalToSharedDepthBytes =
            CopyableTextureBytes(secondaryDevice, frame.SecondaryLocalLinearDepth.GetD3D12ResourceDesc());
        frame.ExpectedSharedToLocalColorBytes =
            CopyableTextureBytes(primaryDevice, frame.PrimaryReceivedSecondaryColor.GetD3D12ResourceDesc());
        frame.ExpectedSharedToLocalDepthBytes =
            CopyableTextureBytes(primaryDevice, frame.PrimaryReceivedSecondaryLinearDepth.GetD3D12ResourceDesc());

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Format = desc.ColorFormat;
        frame.SecondaryLocalColor.CreateRenderTargetView(&rtvDesc, &frame.SecondaryRtvDescriptors, 0);

        rtvDesc.Format = desc.LinearDepthFormat;
        frame.SecondaryLocalLinearDepth.CreateRenderTargetView(&rtvDesc, &frame.SecondaryRtvDescriptors, 1);

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc{};
        dsvViewDesc.Flags = D3D12_DSV_FLAG_NONE;
        dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvViewDesc.Format = desc.DepthStencilFormat;
        frame.SecondaryLocalDepthStencil.CreateDepthStencilView(&dsvViewDesc, &frame.SecondaryDsvDescriptor, 0);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Format = desc.ColorFormat;
        frame.PrimaryReceivedSecondaryColor.CreateShaderResourceView(&srvDesc, &frame.PrimarySrvDescriptors, 0);
        srvDesc.Format = desc.LinearDepthFormat;
        frame.PrimaryReceivedSecondaryLinearDepth.CreateShaderResourceView(&srvDesc, &frame.PrimarySrvDescriptors, 1);
        srvDesc.Format = desc.ColorFormat;
        frame.PrimaryCompositeColor.CreateShaderResourceView(&srvDesc, &frame.PrimaryCompositeDescriptors, 4);

        rtvDesc.Format = desc.ColorFormat;
        frame.PrimaryCompositeColor.CreateRenderTargetView(&rtvDesc, &frame.PrimaryCompositeRtvDescriptor, 0);

        AppendResourceInfo(colorName, secondaryDevice, desc.ColorFormat, desc.Width, desc.Height,
                           localColorDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(linearDepthName, secondaryDevice, desc.LinearDepthFormat, desc.Width, desc.Height,
                           linearDepthDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(dsvName, secondaryDevice, desc.DepthStencilFormat, desc.Width, desc.Height,
                           dsvDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(FrameName(L"CrossAdapterSecondaryColor.Prime", frameIndex), primaryDevice,
                           desc.ColorFormat, desc.Width, desc.Height,
                           D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(FrameName(L"CrossAdapterSecondaryColor.Shared", frameIndex), secondaryDevice,
                           desc.ColorFormat, desc.Width, desc.Height,
                           D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(FrameName(L"CrossAdapterSecondaryLinearDepth.Prime", frameIndex), primaryDevice,
                           desc.LinearDepthFormat, desc.Width, desc.Height,
                           D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(FrameName(L"CrossAdapterSecondaryLinearDepth.Shared", frameIndex), secondaryDevice,
                           desc.LinearDepthFormat, desc.Width, desc.Height,
                           D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(receivedColorName, primaryDevice, desc.ColorFormat, desc.Width, desc.Height,
                           receivedColorDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(receivedDepthName, primaryDevice, desc.LinearDepthFormat, desc.Width, desc.Height,
                           receivedDepthDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(compositeColorName, primaryDevice, desc.ColorFormat, desc.Width, desc.Height,
                           compositeColorDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
    }

    initialized = true;
    return true;
}

void MultiGpuVoxelRenderTargets::Reset()
{
    initialized = false;
    frames.clear();
    resourceTable.clear();
    currentDesc = {};
}

bool MultiGpuVoxelRenderTargets::IsInitialized() const
{
    return initialized;
}

const std::wstring& MultiGpuVoxelRenderTargets::GetFailureMessage() const
{
    return failureMessage;
}

const std::vector<MultiGpuVoxelResourceInfo>& MultiGpuVoxelRenderTargets::GetResourceTable() const
{
    return resourceTable;
}

std::vector<MultiGpuVoxelFrameRenderTargets>& MultiGpuVoxelRenderTargets::GetFrames()
{
    return frames;
}

const std::vector<MultiGpuVoxelFrameRenderTargets>& MultiGpuVoxelRenderTargets::GetFrames() const
{
    return frames;
}

UINT MultiGpuVoxelRenderTargets::GetFrameCount() const
{
    return static_cast<UINT>(frames.size());
}
