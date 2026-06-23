#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"

#include "GDevice.h"
#include "GDescriptorHeap.h"
#include "d3dUtil.h"

#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <stdexcept>
#include <sstream>

using namespace PEPEngine::Graphics;

namespace
{
    constexpr float SecondaryColorClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    constexpr float LinearDepthClear[4] = {FLT_MAX, 0.0f, 0.0f, 0.0f};

    std::wstring FrameName(const wchar_t* baseName, const UINT frameIndex)
    {
        return std::wstring(baseName) + L" Frame " + std::to_wstring(frameIndex);
    }

    UINT64 Align64K(const UINT64 value)
    {
        return (value + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1) &
            ~(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1);
    }

    void ValidateCopyOnlyFootprint(const D3D12_RESOURCE_DESC& textureDesc,
                                   const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                   const UINT64 rowSizeInBytes,
                                   const UINT64 totalBytes)
    {
        if (totalBytes == 0)
            throw std::runtime_error("copy-only bridge footprint has zero total bytes");
        if (footprint.Offset != 0)
            throw std::runtime_error("copy-only bridge footprint offset must be zero");
        if (footprint.Footprint.Format != textureDesc.Format ||
            footprint.Footprint.Width != textureDesc.Width ||
            footprint.Footprint.Height != textureDesc.Height ||
            footprint.Footprint.Depth != 1)
        {
            throw std::runtime_error("copy-only bridge footprint does not match texture descriptor");
        }
        if (footprint.Footprint.RowPitch < rowSizeInBytes ||
            (footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) != 0)
        {
            throw std::runtime_error("copy-only bridge footprint row pitch is invalid");
        }
        if (textureDesc.MipLevels != 1 || textureDesc.DepthOrArraySize != 1)
            throw std::runtime_error("copy-only bridge expects exactly one texture subresource");
    }

    void DumpCopyOnlyBridgeDescriptor(const std::wstring& name,
                                      const D3D12_RESOURCE_DESC& textureDesc,
                                      const D3D12_RESOURCE_DESC& bufferDesc,
                                      const MultiGpuVoxelFrameRenderTargets::CopyOnlyBridge& bridge)
    {
#if defined(_DEBUG)
        std::wostringstream stream;
        stream << L"[CopyOnlyBridge] name=" << name
               << L" sourceDimension=" << textureDesc.Dimension
               << L" sourceWidth=" << textureDesc.Width
               << L" sourceHeight=" << textureDesc.Height
               << L" sourceFormat=" << textureDesc.Format
               << L" sourceLayout=" << textureDesc.Layout
               << L" sourceFlags=" << textureDesc.Flags
               << L" bridgeDimension=" << bufferDesc.Dimension
               << L" bridgeWidth=" << bufferDesc.Width
               << L" bridgeHeight=" << bufferDesc.Height
               << L" bridgeFormat=" << bufferDesc.Format
               << L" bridgeLayout=" << bufferDesc.Layout
               << L" bridgeFlags=" << bufferDesc.Flags
               << L" footprintOffset=" << bridge.Footprint.Offset
               << L" footprintWidth=" << bridge.Footprint.Footprint.Width
               << L" footprintHeight=" << bridge.Footprint.Footprint.Height
               << L" footprintFormat=" << bridge.Footprint.Footprint.Format
               << L" footprintRowPitch=" << bridge.Footprint.Footprint.RowPitch
               << L" numRows=" << bridge.NumRows
               << L" rowSizeInBytes=" << bridge.RowSizeInBytes
               << L" totalBytes=" << bridge.TotalBytes
               << L"\n";
        OutputDebugStringW(stream.str().c_str());
#endif
    }

    MultiGpuVoxelFrameRenderTargets::CopyOnlyBridge CreateCopyOnlyBridge(
        const std::shared_ptr<GDevice>& primaryDevice,
        const std::shared_ptr<GDevice>& secondaryDevice,
        const D3D12_RESOURCE_DESC& textureDesc,
        const std::wstring& name)
    {
        MultiGpuVoxelFrameRenderTargets::CopyOnlyBridge bridge{};
        bridge.DebugLabel = name;
        bridge.SourceFormat = textureDesc.Format;
        bridge.SourceWidth = static_cast<UINT>(textureDesc.Width);
        bridge.SourceHeight = textureDesc.Height;
        bridge.SourceSubresourceCount = textureDesc.MipLevels * textureDesc.DepthOrArraySize;
        primaryDevice->GetDXDevice()->GetCopyableFootprints(
            &textureDesc,
            0,
            1,
            0,
            &bridge.Footprint,
            &bridge.NumRows,
            &bridge.RowSizeInBytes,
            &bridge.TotalBytes);

        ValidateCopyOnlyFootprint(textureDesc, bridge.Footprint, bridge.RowSizeInBytes, bridge.TotalBytes);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT secondaryFootprint{};
        UINT secondaryNumRows = 0;
        UINT64 secondaryRowSizeInBytes = 0;
        UINT64 secondaryTotalBytes = 0;
        secondaryDevice->GetDXDevice()->GetCopyableFootprints(
            &textureDesc,
            0,
            1,
            0,
            &secondaryFootprint,
            &secondaryNumRows,
            &secondaryRowSizeInBytes,
            &secondaryTotalBytes);
        ValidateCopyOnlyFootprint(textureDesc, secondaryFootprint, secondaryRowSizeInBytes, secondaryTotalBytes);
        if (secondaryFootprint.Footprint.Format != bridge.Footprint.Footprint.Format ||
            secondaryFootprint.Footprint.Width != bridge.Footprint.Footprint.Width ||
            secondaryFootprint.Footprint.Height != bridge.Footprint.Footprint.Height ||
            secondaryFootprint.Footprint.Depth != bridge.Footprint.Footprint.Depth ||
            secondaryFootprint.Footprint.RowPitch != bridge.Footprint.Footprint.RowPitch ||
            secondaryTotalBytes != bridge.TotalBytes)
        {
            throw std::runtime_error("copy-only bridge footprints differ between primary and secondary adapters");
        }

        auto bridgeDesc = CD3DX12_RESOURCE_DESC::Buffer(bridge.TotalBytes);
        bridgeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        assert(bridgeDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(bridgeDesc.Format == DXGI_FORMAT_UNKNOWN);
        assert(bridgeDesc.Height == 1);
        assert(bridgeDesc.Width >= bridge.TotalBytes);
        assert(bridge.SourceSubresourceCount == 1 && "CopyOnly bridge supports exactly one subresource");
        assert(bridge.SourceFormat == bridge.Footprint.Footprint.Format &&
               "CopyOnly bridge footprint format must match source texture format");
        assert(bridge.SourceWidth == bridge.Footprint.Footprint.Width &&
               bridge.SourceHeight == bridge.Footprint.Footprint.Height &&
               "CopyOnly bridge footprint dimensions must match source texture dimensions");
        DumpCopyOnlyBridgeDescriptor(name, textureDesc, bridgeDesc, bridge);

        const UINT64 heapBytes = Align64K(bridge.TotalBytes);
        const CD3DX12_HEAP_DESC heapDesc(
            heapBytes,
            D3D12_HEAP_TYPE_DEFAULT,
            0,
            D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);

        ThrowIfFailed(primaryDevice->GetDXDevice()->CreateHeap(
            &heapDesc,
            IID_PPV_ARGS(&bridge.PrimeHeap)));

        HANDLE heapHandle = nullptr;
        ThrowIfFailed(primaryDevice->GetDXDevice()->CreateSharedHandle(
            bridge.PrimeHeap.Get(),
            nullptr,
            GENERIC_ALL,
            nullptr,
            &heapHandle));

        const HRESULT openResult = secondaryDevice->GetDXDevice()->OpenSharedHandle(
            heapHandle,
            IID_PPV_ARGS(&bridge.SharedHeap));
        CloseHandle(heapHandle);
        ThrowIfFailed(openResult);

        bridge.PrimeBuffer = GResource(
            primaryDevice,
            bridgeDesc,
            bridge.PrimeHeap,
            name + L".PrimeCopyBuffer",
            nullptr,
            D3D12_RESOURCE_STATE_COMMON);
        bridge.SharedBuffer = GResource(
            secondaryDevice,
            bridgeDesc,
            bridge.SharedHeap,
            name + L".SecondaryCopyBuffer",
            nullptr,
            D3D12_RESOURCE_STATE_COMMON);

        return bridge;
    }

    VoxelFinalResolveSrvMetadata BuildFinalResolveSrvMetadata(
        const GTexture& texture,
        const GDescriptor& descriptor,
        const UINT descriptorOffset,
        const UINT frameIndex,
        const uint64_t resourceGeneration,
        const uint64_t descriptorGeneration,
        const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
    {
        const auto resource = texture.GetD3D12Resource();
        const auto desc = texture.GetD3D12ResourceDesc();
        VoxelFinalResolveSrvMetadata metadata{};
        metadata.ResourceAddress = reinterpret_cast<uint64_t>(resource.Get());
        metadata.ResourceGeneration = resourceGeneration;
        metadata.DescriptorGeneration = descriptorGeneration;
        metadata.FrameResourceIndex = frameIndex;
        metadata.Format = srvDesc.Format;
        metadata.Width = static_cast<UINT>(desc.Width);
        metadata.Height = desc.Height;
        metadata.ViewDimension = srvDesc.ViewDimension;
        metadata.GpuHandle = descriptor.GetGPUHandle(descriptorOffset);
        return metadata;
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

    if (desc.TransferMode == CrossAdapterTransferMode::Unavailable)
    {
        failureMessage = L"MultiGpu unavailable: cross-adapter transfer mode is unavailable";
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
        auto depthDesc = Texture2DDesc(desc.Width, desc.Height, desc.LinearDepthFormat, D3D12_RESOURCE_FLAG_NONE);
        if (desc.TransferMode == CrossAdapterTransferMode::DirectCrossAdapterTexture)
        {
            GCrossAdapterResource colorBridge(
                colorDesc,
                primaryDevice,
                secondaryDevice,
                L"Voxel Capability Color Bridge");
            GCrossAdapterResource depthBridge(
                depthDesc,
                primaryDevice,
                secondaryDevice,
                L"Voxel Capability Depth Bridge");
        }
        else
        {
            auto colorBridge = CreateCopyOnlyBridge(
                primaryDevice,
                secondaryDevice,
                colorDesc,
                L"Voxel Capability Color Bridge");
            auto depthBridge = CreateCopyOnlyBridge(
                primaryDevice,
                secondaryDevice,
                depthDesc,
                L"Voxel Capability Depth Bridge");
            if (!colorBridge.IsValid() || !depthBridge.IsValid())
                throw std::runtime_error("copy-only bridge was not initialized");
        }
    }
    catch (...)
    {
        failureMessage =
            L"MultiGpu unavailable: failed to allocate/open copy-only cross-adapter bridge buffers"
            L"; transferMode=" + std::wstring(CrossAdapterTransferModeNameW(desc.TransferMode)) +
            L"; primary=" + primaryDevice->GetName() +
            L"; secondary=" + secondaryDevice->GetName() +
            L"; resourceDimension=BUFFER"
            L"; resourceFlags=D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER"
            L"; heapFlags=D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER"
            L"; colorFormat=" + std::to_wstring(static_cast<int>(desc.ColorFormat)) +
            L"; depthFormat=" + std::to_wstring(static_cast<int>(desc.LinearDepthFormat));
        return false;
    }

    return true;
}

bool MultiGpuVoxelRenderTargets::Initialize(
    const std::shared_ptr<GDevice>& primaryDevice,
    const std::shared_ptr<GDevice>& secondaryDevice,
    const MultiGpuVoxelRenderTargetDesc& desc,
    const uint64_t renderTargetGeneration,
    const uint64_t descriptorGeneration)
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
        frame.TransferMode = desc.TransferMode;
        frame.RenderTargetGeneration = renderTargetGeneration;
        frame.DescriptorGeneration = descriptorGeneration;

        frame.SecondaryRtvDescriptors = secondaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
        frame.SecondaryDsvDescriptor = secondaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
        frame.PrimarySrvDescriptors = primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
        frame.PrimaryCompositeDescriptors =
            primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
        frame.PrimaryCompositeFinalResolveSrv =
            primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
        frame.PrimaryCompositeRtvDescriptor =
            primaryDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        assert(!frame.PrimarySrvDescriptors.IsNull() &&
               frame.PrimarySrvDescriptors.GetDescriptorHeap()->GetDevice() == primaryDevice &&
               "received-secondary SRV heap must belong to the primary device");
        assert(!frame.PrimaryCompositeDescriptors.IsNull() &&
               frame.PrimaryCompositeDescriptors.GetDescriptorHeap()->GetDevice() == primaryDevice &&
               "primary composite pass SRV heap must belong to the primary device");
        assert(!frame.PrimaryCompositeFinalResolveSrv.IsNull() &&
               frame.PrimaryCompositeFinalResolveSrv.GetDescriptorHeap()->GetDevice() == primaryDevice &&
               "primary composite final-resolve SRV heap must belong to the primary device");

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

        auto bridgeDepthDesc = Texture2DDesc(desc.Width, desc.Height, desc.LinearDepthFormat, D3D12_RESOURCE_FLAG_NONE);
        frame.LinearDepthTransferBytes = CopyableTextureBytes(primaryDevice, bridgeDepthDesc);
        if (desc.TransferMode == CrossAdapterTransferMode::DirectCrossAdapterTexture)
        {
            frame.CrossAdapterColor = std::make_shared<GCrossAdapterResource>(
                bridgeColorDesc, primaryDevice, secondaryDevice, FrameName(L"CrossAdapterSecondaryColor", frameIndex));
            frame.CrossAdapterLinearDepth = std::make_shared<GCrossAdapterResource>(
                bridgeDepthDesc, primaryDevice, secondaryDevice,
                FrameName(L"CrossAdapterSecondaryLinearDepth", frameIndex));
        }
        else
        {
            frame.CopyOnlyColor = CreateCopyOnlyBridge(
                primaryDevice,
                secondaryDevice,
                bridgeColorDesc,
                FrameName(L"CrossAdapterSecondaryColor", frameIndex));
            frame.CopyOnlyLinearDepth = CreateCopyOnlyBridge(
                primaryDevice,
                secondaryDevice,
                bridgeDepthDesc,
                FrameName(L"CrossAdapterSecondaryLinearDepth", frameIndex));
            frame.ColorTransferBytes = frame.CopyOnlyColor.TotalBytes;
            frame.LinearDepthTransferBytes = frame.CopyOnlyLinearDepth.TotalBytes;
        }

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
        frame.PrimaryReceivedSecondaryColorSrvMetadata =
            BuildFinalResolveSrvMetadata(
                frame.PrimaryReceivedSecondaryColor,
                frame.PrimarySrvDescriptors,
                0,
                frameIndex,
                renderTargetGeneration,
                descriptorGeneration,
                srvDesc);
        srvDesc.Format = desc.LinearDepthFormat;
        frame.PrimaryReceivedSecondaryLinearDepth.CreateShaderResourceView(&srvDesc, &frame.PrimarySrvDescriptors, 1);
        srvDesc.Format = desc.ColorFormat;
        frame.PrimaryCompositeColor.CreateShaderResourceView(&srvDesc, &frame.PrimaryCompositeFinalResolveSrv, 0);
        frame.PrimaryCompositeFinalResolveSrvMetadata =
            BuildFinalResolveSrvMetadata(
                frame.PrimaryCompositeColor,
                frame.PrimaryCompositeFinalResolveSrv,
                0,
                frameIndex,
                renderTargetGeneration,
                descriptorGeneration,
                srvDesc);

        rtvDesc.Format = desc.ColorFormat;
        frame.PrimaryCompositeColor.CreateRenderTargetView(&rtvDesc, &frame.PrimaryCompositeRtvDescriptor, 0);

        AppendResourceInfo(colorName, secondaryDevice, desc.ColorFormat, desc.Width, desc.Height,
                           localColorDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(linearDepthName, secondaryDevice, desc.LinearDepthFormat, desc.Width, desc.Height,
                           linearDepthDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        AppendResourceInfo(dsvName, secondaryDevice, desc.DepthStencilFormat, desc.Width, desc.Height,
                           dsvDesc.Flags, D3D12_RESOURCE_STATE_COMMON);
        if (desc.TransferMode == CrossAdapterTransferMode::CopyOnlyCrossAdapter)
        {
            AppendResourceInfo(FrameName(L"CrossAdapterSecondaryColor.PrimeCopyBuffer", frameIndex), primaryDevice,
                               DXGI_FORMAT_UNKNOWN, static_cast<UINT>(frame.CopyOnlyColor.TotalBytes), 1,
                               D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
            AppendResourceInfo(FrameName(L"CrossAdapterSecondaryColor.SecondaryCopyBuffer", frameIndex), secondaryDevice,
                               DXGI_FORMAT_UNKNOWN, static_cast<UINT>(frame.CopyOnlyColor.TotalBytes), 1,
                               D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
            AppendResourceInfo(FrameName(L"CrossAdapterSecondaryLinearDepth.PrimeCopyBuffer", frameIndex),
                               primaryDevice,
                               DXGI_FORMAT_UNKNOWN, static_cast<UINT>(frame.CopyOnlyLinearDepth.TotalBytes), 1,
                               D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
            AppendResourceInfo(FrameName(L"CrossAdapterSecondaryLinearDepth.SecondaryCopyBuffer", frameIndex),
                               secondaryDevice,
                               DXGI_FORMAT_UNKNOWN, static_cast<UINT>(frame.CopyOnlyLinearDepth.TotalBytes), 1,
                               D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
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
        }
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
