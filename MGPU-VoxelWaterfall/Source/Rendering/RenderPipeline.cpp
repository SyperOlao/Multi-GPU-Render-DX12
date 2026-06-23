#include "Source/Rendering/RenderPipeline.h"

#include "FrameResource.h"
#include "GCommandList.h"
#include "GCommandQueue.h"
#include "GResourceStateTracker.h"
#include "Source/Voxels/VoxelGpuPartition.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cstring>
#include <sstream>

using namespace PEPEngine::Graphics;

namespace
{
    std::string Narrow(const std::wstring& value)
    {
        std::string result;
        result.reserve(value.size());
        for (const wchar_t ch : value)
            result.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
        return result;
    }

    std::string ResourceIdentity(const GResource& resource)
    {
        const auto desc = resource.GetD3D12ResourceDesc();
        std::ostringstream stream;
        stream << Narrow(resource.GetName()) << "@"
               << static_cast<const void*>(resource.GetD3D12Resource().Get())
               << ":" << desc.Width << "x" << desc.Height
               << ":fmt" << static_cast<uint32_t>(desc.Format);
        return stream.str();
    }

    UINT64 ExpectedCopyableBytes(GResource& resource)
    {
        const auto desc = resource.GetD3D12ResourceDesc();
        UINT64 totalBytes = 0;
        resource.GetDevice()->GetDXDevice()->GetCopyableFootprints(
            &desc, 0, 1, 0, nullptr, nullptr, nullptr, &totalBytes);
        return totalBytes;
    }

    void AssertRenderOutputTransferByteBudget(const VoxelFrameGraphTelemetry& telemetry)
    {
        if (telemetry.RenderWidth == 0 || telemetry.RenderHeight == 0 ||
            telemetry.RenderOutputTransferBytes == 0)
        {
            return;
        }

        constexpr UINT64 BytesPerPixel = 4;
        const UINT64 maxColorAndDepthBytes =
            static_cast<UINT64>(telemetry.RenderWidth) *
            static_cast<UINT64>(telemetry.RenderHeight) *
            BytesPerPixel *
            2;
        assert(telemetry.RenderOutputTransferBytes <= maxColorAndDepthBytes &&
               "render-output transfer must not exceed logical color+depth resolution");
    }

    void RecomputeRenderOutputTransferTelemetry(VoxelFrameGraphTelemetry& telemetry)
    {
        telemetry.ColorBytesTransferred =
            std::max(telemetry.LocalToSharedColorBytes, telemetry.SharedToLocalColorBytes);
        telemetry.DepthBytesTransferred =
            std::max(telemetry.LocalToSharedDepthBytes, telemetry.SharedToLocalDepthBytes);
        telemetry.TotalCrossAdapterBytes =
            telemetry.ColorBytesTransferred + telemetry.DepthBytesTransferred;
        telemetry.RenderOutputTransferBytes = telemetry.TotalCrossAdapterBytes;
        AssertRenderOutputTransferByteBudget(telemetry);
    }

    UINT TextureSubresourceCount(const D3D12_RESOURCE_DESC& desc)
    {
        return static_cast<UINT>(desc.MipLevels) * desc.DepthOrArraySize;
    }

    bool IsCopyQueueStateAllowed(const D3D12_RESOURCE_STATES state)
    {
        return state == D3D12_RESOURCE_STATE_COMMON ||
            state == D3D12_RESOURCE_STATE_COPY_SOURCE ||
            state == D3D12_RESOURCE_STATE_COPY_DEST;
    }

    void EmitCopyOnlyContractLabel(const wchar_t* phase,
                                   const MultiGpuVoxelFrameRenderTargets::CopyOnlyBridge& bridge)
    {
#if defined(_DEBUG)
        std::wostringstream stream;
        stream << L"[CopyOnlyStateContract]"
               << L" phase=" << phase
               << L" label=\"" << bridge.DebugLabel << L"\""
               << L" format=" << bridge.SourceFormat
               << L" size=" << bridge.SourceWidth << L"x" << bridge.SourceHeight
               << L" rowPitch=" << bridge.Footprint.Footprint.RowPitch
               << L" totalBytes=" << bridge.TotalBytes
               << L"\n";
        OutputDebugStringW(stream.str().c_str());
#endif
    }

    void AssertTrackedResourceState(const GResource& resource,
                                    const D3D12_RESOURCE_STATES expected,
                                    const char* label)
    {
        D3D12_RESOURCE_STATES actual = D3D12_RESOURCE_STATE_COMMON;
        const bool hasState = GResourceStateTracker::TryGetCurrentState(resource.GetD3D12Resource(), actual);
        assert(hasState && "CopyOnlyCrossAdapter state contract requires a tracked resource state");
        assert(actual == expected && "CopyOnlyCrossAdapter resource state contract mismatch");
        (void)label;
    }

    void ValidateCopyOnlyBridgeInvariant(
        const MultiGpuVoxelFrameRenderTargets::CopyOnlyBridge& bridge,
        const GResource& sourceTexture,
        const GResource& destinationTexture,
        const char* label)
    {
        const auto sourceDesc = sourceTexture.GetD3D12ResourceDesc();
        const auto destinationDesc = destinationTexture.GetD3D12ResourceDesc();
        const auto primeBufferDesc = bridge.PrimeBuffer.GetD3D12ResourceDesc();
        const auto sharedBufferDesc = bridge.SharedBuffer.GetD3D12ResourceDesc();

        assert(bridge.IsValid() && "CopyOnly bridge must be valid");
        assert(sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               destinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               "CopyOnly bridge source/destination resources must be Texture2D");
        assert(primeBufferDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
               sharedBufferDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
               "CopyOnly cross-adapter bridge resources must be buffers");
        assert(sourceDesc.Format == destinationDesc.Format &&
               "CopyOnly source and received texture formats must match");
        assert(sourceDesc.Format == bridge.SourceFormat &&
               sourceDesc.Format == bridge.Footprint.Footprint.Format &&
               "CopyOnly footprint format must match source texture format");
        assert(sourceDesc.Width == destinationDesc.Width &&
               sourceDesc.Height == destinationDesc.Height &&
               "CopyOnly source and received texture dimensions must match");
        assert(bridge.SourceWidth == static_cast<UINT>(sourceDesc.Width) &&
               bridge.SourceHeight == sourceDesc.Height &&
               bridge.Footprint.Footprint.Width == sourceDesc.Width &&
               bridge.Footprint.Footprint.Height == sourceDesc.Height &&
               "CopyOnly footprint dimensions must match source texture dimensions");
        assert(TextureSubresourceCount(sourceDesc) == 1 &&
               TextureSubresourceCount(destinationDesc) == 1 &&
               bridge.SourceSubresourceCount == 1 &&
               "CopyOnly path expects exactly one subresource for source and destination textures");
        assert(bridge.Footprint.Offset == 0 &&
               "CopyOnly placed footprint offset must remain zero");
        assert((bridge.Footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0 &&
               "CopyOnly row pitch must satisfy D3D12_TEXTURE_DATA_PITCH_ALIGNMENT");
        assert(bridge.Footprint.Footprint.RowPitch >= bridge.RowSizeInBytes &&
               "CopyOnly row pitch must cover one unpadded texture row");
        assert(bridge.Footprint.Footprint.Depth == 1 &&
               "CopyOnly footprint depth must be 1");
        assert(bridge.NumRows == sourceDesc.Height &&
               "CopyOnly footprint row count must match source height");
        assert(bridge.TotalBytes > 0 &&
               primeBufferDesc.Width >= bridge.TotalBytes &&
               sharedBufferDesc.Width >= bridge.TotalBytes &&
               "CopyOnly bridge buffers must cover the full copy footprint");
        assert(primeBufferDesc.Format == DXGI_FORMAT_UNKNOWN &&
               sharedBufferDesc.Format == DXGI_FORMAT_UNKNOWN &&
               "CopyOnly bridge buffers must use DXGI_FORMAT_UNKNOWN");
        (void)label;
    }

    void ValidateCopyOnlyFrameInvariants(const MultiGpuVoxelFrameRenderTargets& targets)
    {
        if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
            return;

        ValidateCopyOnlyBridgeInvariant(targets.CopyOnlyColor,
                                        targets.SecondaryLocalColor,
                                        targets.PrimaryReceivedSecondaryColor,
                                        "CopyOnly color bridge");
        ValidateCopyOnlyBridgeInvariant(targets.CopyOnlyLinearDepth,
                                        targets.SecondaryLocalLinearDepth,
                                        targets.PrimaryReceivedSecondaryLinearDepth,
                                        "CopyOnly linear depth bridge");
    }

    void AssertCopyOnlyBeforeTextureToBufferCopy(const MultiGpuVoxelFrameRenderTargets& targets)
    {
        if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
            return;

        EmitCopyOnlyContractLabel(L"before texture-to-buffer copy", targets.CopyOnlyColor);
        EmitCopyOnlyContractLabel(L"before texture-to-buffer copy", targets.CopyOnlyLinearDepth);
        ValidateCopyOnlyFrameInvariants(targets);
        AssertTrackedResourceState(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COMMON,
                                   "SecondaryLocalColor before texture-to-buffer copy");
        AssertTrackedResourceState(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COMMON,
                                   "SecondaryLocalLinearDepth before texture-to-buffer copy");
        AssertTrackedResourceState(targets.CopyOnlyColor.SharedBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyColor.SharedBuffer before texture-to-buffer copy");
        AssertTrackedResourceState(targets.CopyOnlyLinearDepth.SharedBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyLinearDepth.SharedBuffer before texture-to-buffer copy");
    }

    void AssertCopyOnlyBeforeBufferToTextureCopy(const MultiGpuVoxelFrameRenderTargets& targets)
    {
        if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
            return;

        EmitCopyOnlyContractLabel(L"before buffer-to-texture copy", targets.CopyOnlyColor);
        EmitCopyOnlyContractLabel(L"before buffer-to-texture copy", targets.CopyOnlyLinearDepth);
        ValidateCopyOnlyFrameInvariants(targets);
        AssertTrackedResourceState(targets.CopyOnlyColor.PrimeBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyColor.PrimeBuffer before buffer-to-texture copy");
        AssertTrackedResourceState(targets.CopyOnlyLinearDepth.PrimeBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyLinearDepth.PrimeBuffer before buffer-to-texture copy");
        AssertTrackedResourceState(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COMMON,
                                   "PrimaryReceivedSecondaryColor before buffer-to-texture copy");
        AssertTrackedResourceState(targets.PrimaryReceivedSecondaryLinearDepth, D3D12_RESOURCE_STATE_COMMON,
                                   "PrimaryReceivedSecondaryLinearDepth before buffer-to-texture copy");
    }

    void AssertCopyOnlyBeforeCrossAdapterBufferHandoff(const MultiGpuVoxelFrameRenderTargets& targets)
    {
        if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
            return;

        EmitCopyOnlyContractLabel(L"before cross-adapter buffer copy", targets.CopyOnlyColor);
        EmitCopyOnlyContractLabel(L"before cross-adapter buffer copy", targets.CopyOnlyLinearDepth);
        ValidateCopyOnlyFrameInvariants(targets);
        AssertTrackedResourceState(targets.CopyOnlyColor.SharedBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyColor.SharedBuffer before cross-adapter buffer copy");
        AssertTrackedResourceState(targets.CopyOnlyLinearDepth.SharedBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyLinearDepth.SharedBuffer before cross-adapter buffer copy");
        AssertTrackedResourceState(targets.CopyOnlyColor.PrimeBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyColor.PrimeBuffer before cross-adapter buffer copy");
        AssertTrackedResourceState(targets.CopyOnlyLinearDepth.PrimeBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyLinearDepth.PrimeBuffer before cross-adapter buffer copy");
    }

    void AssertCopyOnlyAfterCopySubmission(const MultiGpuVoxelFrameRenderTargets& targets)
    {
        if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
            return;

        EmitCopyOnlyContractLabel(L"after submission", targets.CopyOnlyColor);
        EmitCopyOnlyContractLabel(L"after submission", targets.CopyOnlyLinearDepth);
        AssertTrackedResourceState(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COMMON,
                                   "SecondaryLocalColor after copy submission");
        AssertTrackedResourceState(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COMMON,
                                   "SecondaryLocalLinearDepth after copy submission");
        AssertTrackedResourceState(targets.CopyOnlyColor.SharedBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyColor.SharedBuffer after copy submission");
        AssertTrackedResourceState(targets.CopyOnlyLinearDepth.SharedBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyLinearDepth.SharedBuffer after copy submission");
        AssertTrackedResourceState(targets.CopyOnlyColor.PrimeBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyColor.PrimeBuffer after copy submission");
        AssertTrackedResourceState(targets.CopyOnlyLinearDepth.PrimeBuffer, D3D12_RESOURCE_STATE_COMMON,
                                   "CopyOnlyLinearDepth.PrimeBuffer after copy submission");
        AssertTrackedResourceState(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COMMON,
                                   "PrimaryReceivedSecondaryColor after copy submission");
        AssertTrackedResourceState(targets.PrimaryReceivedSecondaryLinearDepth, D3D12_RESOURCE_STATE_COMMON,
                                   "PrimaryReceivedSecondaryLinearDepth after copy submission");
    }

    void AssertCopyOnlyBeforeSecondaryRender(const MultiGpuVoxelFrameRenderTargets& targets)
    {
        if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
            return;

        EmitCopyOnlyContractLabel(L"before secondary render", targets.CopyOnlyColor);
        EmitCopyOnlyContractLabel(L"before secondary render", targets.CopyOnlyLinearDepth);
        ValidateCopyOnlyFrameInvariants(targets);
        AssertTrackedResourceState(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COMMON,
                                   "SecondaryLocalColor before secondary render");
        AssertTrackedResourceState(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COMMON,
                                   "SecondaryLocalLinearDepth before secondary render");
    }

    void CopyTextureToPlacedBuffer(
        const std::shared_ptr<GCommandList>& cmdList,
        const GResource& destinationBuffer,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& destinationFootprint,
        const GResource& sourceTexture,
        const D3D12_BOX* sourceBox = nullptr,
        const UINT destinationX = 0,
        const UINT destinationY = 0)
    {
        const auto destinationDesc = destinationBuffer.GetD3D12ResourceDesc();
        const auto sourceDesc = sourceTexture.GetD3D12ResourceDesc();

        assert(destinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        assert(IsCopyQueueStateAllowed(D3D12_RESOURCE_STATE_COPY_DEST) &&
               IsCopyQueueStateAllowed(D3D12_RESOURCE_STATE_COPY_SOURCE) &&
               "CopyOnly copy commands must use states valid on a COPY queue");
        assert(destinationFootprint.Footprint.Format == sourceDesc.Format);
        assert(destinationFootprint.Footprint.Width == sourceDesc.Width);
        assert(destinationFootprint.Footprint.Height == sourceDesc.Height);
        assert(TextureSubresourceCount(sourceDesc) == 1 &&
               "CopyOnly texture-to-buffer copy expects one source subresource");

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = destinationBuffer.GetD3D12Resource().Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = destinationFootprint;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = sourceTexture.GetD3D12Resource().Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        cmdList->GetGraphicsCommandList()->CopyTextureRegion(
            &destination,
            destinationX,
            destinationY,
            0,
            &source,
            sourceBox);
    }

    void CopyPlacedBufferToTexture(
        const std::shared_ptr<GCommandList>& cmdList,
        const GResource& destinationTexture,
        const GResource& sourceBuffer,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& sourceFootprint,
        const D3D12_BOX* sourceBox = nullptr,
        const UINT destinationX = 0,
        const UINT destinationY = 0)
    {
        const auto destinationDesc = destinationTexture.GetD3D12ResourceDesc();
        const auto sourceDesc = sourceBuffer.GetD3D12ResourceDesc();

        assert(destinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        assert(sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(IsCopyQueueStateAllowed(D3D12_RESOURCE_STATE_COPY_DEST) &&
               IsCopyQueueStateAllowed(D3D12_RESOURCE_STATE_COPY_SOURCE) &&
               "CopyOnly copy commands must use states valid on a COPY queue");
        assert(sourceFootprint.Footprint.Format == destinationDesc.Format);
        assert(sourceFootprint.Footprint.Width == destinationDesc.Width);
        assert(sourceFootprint.Footprint.Height == destinationDesc.Height);
        assert(TextureSubresourceCount(destinationDesc) == 1 &&
               "CopyOnly buffer-to-texture copy expects one destination subresource");

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = destinationTexture.GetD3D12Resource().Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = sourceBuffer.GetD3D12Resource().Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = sourceFootprint;

        cmdList->GetGraphicsCommandList()->CopyTextureRegion(
            &destination,
            destinationX,
            destinationY,
            0,
            &source,
            sourceBox);
    }

    D3D12_BOX BoxFromRect(const D3D12_RECT& rect)
    {
        D3D12_BOX box{};
        box.left = static_cast<UINT>(std::max<LONG>(0, rect.left));
        box.top = static_cast<UINT>(std::max<LONG>(0, rect.top));
        box.front = 0;
        box.right = static_cast<UINT>(std::max<LONG>(box.left, rect.right));
        box.bottom = static_cast<UINT>(std::max<LONG>(box.top, rect.bottom));
        box.back = 1;
        return box;
    }

    UINT RectWidth(const D3D12_RECT& rect)
    {
        return static_cast<UINT>(std::max<LONG>(0, rect.right - rect.left));
    }

    UINT RectHeight(const D3D12_RECT& rect)
    {
        return static_cast<UINT>(std::max<LONG>(0, rect.bottom - rect.top));
    }

    UINT64 DirtyBytesForFootprint(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                  const D3D12_RECT& rect)
    {
        return static_cast<UINT64>(footprint.Footprint.RowPitch) * RectHeight(rect);
    }

    void CopyTextureRectNoBarrier(
        const std::shared_ptr<GCommandList>& cmdList,
        GResource& destinationTexture,
        GResource& sourceTexture,
        const D3D12_RECT& rect)
    {
        const auto box = BoxFromRect(rect);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = destinationTexture.GetD3D12Resource().Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = sourceTexture.GetD3D12Resource().Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        cmdList->GetGraphicsCommandList()->CopyTextureRegion(
            &destination,
            static_cast<UINT>(rect.left),
            static_cast<UINT>(rect.top),
            0,
            &source,
            &box);
    }

}

std::optional<D3D12_QUERY_DATA_PIPELINE_STATISTICS> ReadCompletedSecondaryPipelineStatistics(
    const MultiGpuVoxelFrameRenderTargets& targets)
{
    if (!targets.SecondaryPipelineStatsReadback.IsValid())
        return std::nullopt;

    D3D12_QUERY_DATA_PIPELINE_STATISTICS stats{};
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, sizeof(stats)};
    if (FAILED(targets.SecondaryPipelineStatsReadback.GetD3D12Resource()->Map(0, &readRange, &mapped)) ||
        !mapped)
    {
        return std::nullopt;
    }

    std::memcpy(&stats, mapped, sizeof(stats));
    const D3D12_RANGE emptyRange{0, 0};
    targets.SecondaryPipelineStatsReadback.GetD3D12Resource()->Unmap(0, &emptyRange);
    return stats;
}

void RenderPipeline::ValidateCopyOnlyFrameStateBeforePrimaryRender(
    const MultiGpuVoxelFrameRenderTargets& targets) const
{
    if (targets.TransferMode != CrossAdapterTransferMode::CopyOnlyCrossAdapter)
        return;

    EmitCopyOnlyContractLabel(L"before primary render", targets.CopyOnlyColor);
    EmitCopyOnlyContractLabel(L"before primary render", targets.CopyOnlyLinearDepth);
    ValidateCopyOnlyFrameInvariants(targets);
    AssertCopyOnlyAfterCopySubmission(targets);
}

void RenderPipeline::SubmitPrimaryBasePass(const PrimaryBasePassContext& context) const
{
    assert(context.RenderQueue &&
           context.RenderQueue->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
           "Primary render must execute on a DIRECT queue");
    const auto cmdList = context.RenderQueue->GetCommandList();

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                         VoxelBenchmarkProfiler::RangeId::PrimaryBaseGraphics);

    context.RecordPrimaryBaseCommands(cmdList);

    context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                       VoxelBenchmarkProfiler::RangeId::PrimaryBaseGraphics);
    context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                           VoxelBenchmarkProfiler::RangeId::PrimaryBaseGraphics);
    cmdList->EndQuery(context.TimestampHeapIndex + 1);
    cmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

    if (context.PrimaryComputeFenceValue != 0)
        context.RenderQueue->Wait(context.PrimaryComputeFence, context.PrimaryComputeFenceValue);

    context.CurrentFrameResource.PrimeRenderFenceValue = context.RenderQueue->ExecuteCommandList(cmdList);
    context.CurrentFrameResource.PrimaryBaseRenderFenceValue = context.CurrentFrameResource.PrimeRenderFenceValue;
    context.GraphicsPassFenceValue = context.CurrentFrameResource.PrimeRenderFenceValue;
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                            context.GraphicsPassFenceValue);

    if (context.Telemetry)
    {
        context.Telemetry->PrimaryBaseGraphicsSubmitted = true;
        context.Telemetry->PrimaryBaseGraphicsFenceValue = context.GraphicsPassFenceValue;
    }
}

void RenderPipeline::SubmitSecondaryVoxelPass(const SecondaryVoxelGraphicsPassContext& context) const
{
    assert(context.SecondaryGraphicsQueue &&
           context.SecondaryGraphicsQueue->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
           "Secondary render must execute on a DIRECT queue");
    const auto cmdList = context.SecondaryGraphicsQueue->GetCommandList();
    auto& targets = context.RenderTargets;
    AssertCopyOnlyBeforeSecondaryRender(targets);

    constexpr float secondaryColorClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    constexpr float linearDepthClear[4] = {FLT_MAX, 0.0f, 0.0f, 0.0f};

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
                                         VoxelBenchmarkProfiler::RangeId::SecondaryGraphics);
    if (targets.SecondaryPipelineStatsQueryHeap)
    {
        cmdList->GetGraphicsCommandList()->BeginQuery(
            targets.SecondaryPipelineStatsQueryHeap.Get(),
            D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
            0);
    }
    cmdList->SetViewports(&context.Viewport, 1);
    cmdList->SetScissorRects(&context.ScissorRect, 1);

    cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->TransitionBarrier(targets.SecondaryLocalDepthStencil, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->FlushResourceBarriers();

    cmdList->ClearRenderTarget(&targets.SecondaryRtvDescriptors, 0, secondaryColorClear);
    cmdList->ClearRenderTarget(&targets.SecondaryRtvDescriptors, 1, linearDepthClear);
    cmdList->ClearDepthStencil(&targets.SecondaryDsvDescriptor, 0, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0);
    cmdList->SetRenderTargets(2, &targets.SecondaryRtvDescriptors, 0, &targets.SecondaryDsvDescriptor, 0);

    uint32_t drawCallCount = 0;
    uint32_t submittedVoxelCount = 0;
    VoxelSpatialLodStats lodStats{};
    uint32_t indirectDrawCalls = 0;
    if (context.SecondaryPartitions)
    {
        for (const auto& partition : *context.SecondaryPartitions)
        {
            if (!partition.GpuPartition || partition.LogicalVoxelCount == 0)
                continue;

            partition.GpuPartition->UpdateFrameConstants();
            auto result = partition.GpuPartition->RecordRender(
                cmdList,
                VoxelPartitionRenderOutputMode::SecondaryColorAndLinearDepth,
                context.CurrentFrameResource.SecondaryPassConstantUploadBuffer.get(),
                &context.BenchmarkProfiler,
                VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
                VoxelBenchmarkProfiler::RangeId::SecondaryLodCompaction);
            result.LayerId = partition.LayerId;
            result.PartitionId = partition.PartitionId;
            result.LogicalVoxelCount = partition.LogicalVoxelCount;
            result.SceneGeneration = partition.SceneGeneration;
            result.PartitionGeneration = partition.PartitionGeneration;
            drawCallCount += result.DrawCallCount;
            submittedVoxelCount += result.SubmittedVoxelCount;
            lodStats.Lod0Rendered += result.LodStats.Lod0Rendered;
            lodStats.Lod1Rendered += result.LodStats.Lod1Rendered;
            lodStats.Lod2Rendered += result.LodStats.Lod2Rendered;
            lodStats.Aggregated += result.LodStats.Aggregated;
            if (result.UsedIndirectDraw)
                indirectDrawCalls += result.DrawCallCount;
            if (context.SecondaryVoxelRenderResults)
                context.SecondaryVoxelRenderResults->push_back(result);
        }
    }

    cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(targets.SecondaryLocalDepthStencil, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->FlushResourceBarriers();

    context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
                                       VoxelBenchmarkProfiler::RangeId::SecondaryGraphics);
    if (targets.SecondaryPipelineStatsQueryHeap)
    {
        cmdList->GetGraphicsCommandList()->EndQuery(
            targets.SecondaryPipelineStatsQueryHeap.Get(),
            D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
            0);
        cmdList->GetGraphicsCommandList()->ResolveQueryData(
            targets.SecondaryPipelineStatsQueryHeap.Get(),
            D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
            0,
            1,
            targets.SecondaryPipelineStatsReadback.GetD3D12Resource().Get(),
            0);
    }
    context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
                                           VoxelBenchmarkProfiler::RangeId::SecondaryGraphics);
    cmdList->EndQuery(context.TimestampHeapIndex + 1);
    cmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

    if (context.SecondaryComputeFenceValue != 0)
        context.SecondaryGraphicsQueue->Wait(context.SecondaryComputeFence, context.SecondaryComputeFenceValue);

    context.CurrentFrameResource.SecondaryRenderFenceValue =
        context.SecondaryGraphicsQueue->ExecuteCommandList(cmdList);
    AssertCopyOnlyBeforeTextureToBufferCopy(targets);
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
                                            context.CurrentFrameResource.SecondaryRenderFenceValue);

    if (context.Telemetry)
    {
        context.Telemetry->SecondaryGraphicsSubmitted = true;
        context.Telemetry->SecondaryGraphicsCommandListSubmissionCount += 1;
        context.Telemetry->SecondaryRenderSubmitted = true;
        context.Telemetry->SecondaryGraphicsFenceValue =
            context.CurrentFrameResource.SecondaryRenderFenceValue;
        context.Telemetry->SecondaryDrawCalls = drawCallCount;
        context.Telemetry->SecondaryRenderedVoxelCount = submittedVoxelCount;
        context.Telemetry->SecondarySpatialLodStats = lodStats;
        context.Telemetry->SecondaryIndirectDrawCalls = indirectDrawCalls;
        context.Telemetry->SecondaryGraphicsTimestampBeginQuery = context.TimestampHeapIndex;
        context.Telemetry->SecondaryGraphicsTimestampEndQuery = context.TimestampHeapIndex + 1;
        context.Telemetry->SecondaryIndirectArgumentMaxCommandCount = indirectDrawCalls > 0 ? 1u : 0u;
        context.Telemetry->SecondaryIndirectArgumentResolvedDrawCount = indirectDrawCalls;
    }
}

void RenderPipeline::SubmitSecondaryLocalToSharedCopyPass(
    const SecondaryLocalToSharedCopyPassContext& context) const
{
    assert(context.SecondaryCopyQueue &&
           context.SecondaryCopyQueue->GetType() == D3D12_COMMAND_LIST_TYPE_COPY &&
           "Secondary local-to-shared transfer must execute on a COPY queue");
    const auto cmdList = context.SecondaryCopyQueue->GetCommandList();
    auto& targets = context.RenderTargets;
    const bool copyOnly = targets.TransferMode == CrossAdapterTransferMode::CopyOnlyCrossAdapter;
    const auto* sharedColor = copyOnly
                                  ? &targets.CopyOnlyColor.SharedBuffer
                                  : &targets.CrossAdapterColor->GetSharedResource();
    const auto* sharedDepth = copyOnly
                                  ? &targets.CopyOnlyLinearDepth.SharedBuffer
                                  : &targets.CrossAdapterLinearDepth->GetSharedResource();

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCopy,
                                         VoxelBenchmarkProfiler::RangeId::SecondaryLocalToSharedCopy);
    AssertCopyOnlyBeforeTextureToBufferCopy(targets);
    if (copyOnly)
    {
        cmdList->TransitionBarrier(*sharedColor, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->TransitionBarrier(*sharedDepth, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    else
    {
        cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(*sharedColor, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->TransitionBarrier(*sharedDepth, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    cmdList->FlushResourceBarriers();

    if (copyOnly)
    {
        const bool useDirtyRect = context.UseDirtyRect && RectWidth(context.DirtyRect) > 0 &&
            RectHeight(context.DirtyRect) > 0;
        const auto dirtyBox = BoxFromRect(context.DirtyRect);
        CopyTextureToPlacedBuffer(
            cmdList,
            targets.CopyOnlyColor.SharedBuffer,
            targets.CopyOnlyColor.Footprint,
            targets.SecondaryLocalColor,
            useDirtyRect ? &dirtyBox : nullptr,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.left) : 0,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.top) : 0);
        CopyTextureToPlacedBuffer(
            cmdList,
            targets.CopyOnlyLinearDepth.SharedBuffer,
            targets.CopyOnlyLinearDepth.Footprint,
            targets.SecondaryLocalLinearDepth,
            useDirtyRect ? &dirtyBox : nullptr,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.left) : 0,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.top) : 0);
    }
    else
    {
        if (context.UseDirtyRect)
        {
            CopyTextureRectNoBarrier(cmdList, const_cast<GResource&>(*sharedColor),
                                     targets.SecondaryLocalColor, context.DirtyRect);
            CopyTextureRectNoBarrier(cmdList, const_cast<GResource&>(*sharedDepth),
                                     targets.SecondaryLocalLinearDepth, context.DirtyRect);
        }
        else
        {
            cmdList->CopyResourceNoBarrier(*sharedColor, targets.SecondaryLocalColor);
            cmdList->CopyResourceNoBarrier(*sharedDepth, targets.SecondaryLocalLinearDepth);
        }
    }

    cmdList->TransitionBarrier(*sharedColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(*sharedDepth, D3D12_RESOURCE_STATE_COMMON);
    if (!copyOnly)
    {
        cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COMMON);
        cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COMMON);
    }
    cmdList->FlushResourceBarriers();
    context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCopy,
                                       VoxelBenchmarkProfiler::RangeId::SecondaryLocalToSharedCopy);
    context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCopy,
                                           VoxelBenchmarkProfiler::RangeId::SecondaryLocalToSharedCopy);
    cmdList->EndQuery(context.TimestampHeapIndex + 1);
    cmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

    context.SecondaryCopyQueue->Wait(context.SecondaryRenderFence, context.SecondaryRenderFenceValue);
    context.CurrentFrameResource.SecondaryLocalToSharedCopyFenceValue =
        context.SecondaryCopyQueue->ExecuteCommandList(cmdList);
    AssertCopyOnlyAfterCopySubmission(targets);
    AssertCopyOnlyBeforeCrossAdapterBufferHandoff(targets);
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::SecondaryCopy,
                                            context.CurrentFrameResource.SecondaryLocalToSharedCopyFenceValue);

    context.CrossAdapterRenderReadyFenceValue++;
    context.CurrentFrameResource.CrossAdapterRenderReadyFenceValue =
        context.CrossAdapterRenderReadyFenceValue;
    context.SecondaryCopyQueue->Signal(
        context.CrossAdapterRenderReadyFence,
        context.CurrentFrameResource.CrossAdapterRenderReadyFenceValue);

    if (context.Telemetry)
    {
        const bool useDirtyRect = context.UseDirtyRect && RectWidth(context.DirtyRect) > 0 &&
            RectHeight(context.DirtyRect) > 0;
        const UINT64 fullColorBytes = copyOnly
                                          ? targets.CopyOnlyColor.TotalBytes
                                          : ExpectedCopyableBytes(targets.SecondaryLocalColor);
        const UINT64 fullDepthBytes = copyOnly
                                          ? targets.CopyOnlyLinearDepth.TotalBytes
                                          : ExpectedCopyableBytes(targets.SecondaryLocalLinearDepth);
        const UINT64 expectedColorBytes =
            useDirtyRect && copyOnly
                ? DirtyBytesForFootprint(targets.CopyOnlyColor.Footprint, context.DirtyRect)
                : fullColorBytes;
        const UINT64 expectedDepthBytes =
            useDirtyRect && copyOnly
                ? DirtyBytesForFootprint(targets.CopyOnlyLinearDepth.Footprint, context.DirtyRect)
                : fullDepthBytes;
        context.Telemetry->LocalToSharedCommandListSubmissionCount += 1;
        context.Telemetry->SecondaryLocalToSharedCopyFenceValue =
            context.CurrentFrameResource.SecondaryLocalToSharedCopyFenceValue;
        context.Telemetry->CrossAdapterRenderReadyFenceValue =
            context.CurrentFrameResource.CrossAdapterRenderReadyFenceValue;
        context.Telemetry->ExpectedLocalToSharedColorBytes = expectedColorBytes;
        context.Telemetry->ExpectedLocalToSharedDepthBytes = expectedDepthBytes;
        context.Telemetry->LocalToSharedColorBytes = expectedColorBytes;
        context.Telemetry->LocalToSharedDepthBytes = expectedDepthBytes;
        context.Telemetry->LocalToSharedColorSource = ResourceIdentity(targets.SecondaryLocalColor);
        context.Telemetry->LocalToSharedColorDestination = ResourceIdentity(*sharedColor);
        context.Telemetry->LocalToSharedDepthSource = ResourceIdentity(targets.SecondaryLocalLinearDepth);
        context.Telemetry->LocalToSharedDepthDestination = ResourceIdentity(*sharedDepth);
        context.Telemetry->CopyOperation = copyOnly ? "CopyTextureRegion" : "CopyResource";
        context.Telemetry->LocalToSharedPath = copyOnly ? "texture_to_placed_buffer" : "texture_to_shared_texture";
        context.Telemetry->BridgeResourceDimension = copyOnly ? "BUFFER" : "TEXTURE2D";
        context.Telemetry->BridgeColorBytes = copyOnly ? targets.CopyOnlyColor.TotalBytes : expectedColorBytes;
        context.Telemetry->BridgeDepthBytes = copyOnly ? targets.CopyOnlyLinearDepth.TotalBytes : expectedDepthBytes;
        context.Telemetry->BridgeColorRowPitch = copyOnly ? targets.CopyOnlyColor.Footprint.Footprint.RowPitch : 0;
        context.Telemetry->BridgeDepthRowPitch =
            copyOnly ? targets.CopyOnlyLinearDepth.Footprint.Footprint.RowPitch : 0;
        context.Telemetry->LocalToSharedTimestampBeginQuery = context.TimestampHeapIndex;
        context.Telemetry->LocalToSharedTimestampEndQuery = context.TimestampHeapIndex + 1;
        context.Telemetry->ParticleTransferBytes = 0;
        context.Telemetry->FullFrameTransferBytes = fullColorBytes + fullDepthBytes;
        context.Telemetry->ActualTransferBytes = expectedColorBytes + expectedDepthBytes;
        RecomputeRenderOutputTransferTelemetry(*context.Telemetry);
    }
}

void RenderPipeline::SubmitPrimarySharedToLocalCopyPass(
    const PrimarySharedToLocalCopyPassContext& context) const
{
    assert(context.PrimaryCopyQueue &&
           context.PrimaryCopyQueue->GetType() == D3D12_COMMAND_LIST_TYPE_COPY &&
           "Primary shared-to-local transfer must execute on a COPY queue");
    const auto cmdList = context.PrimaryCopyQueue->GetCommandList();
    auto& targets = context.RenderTargets;
    const bool copyOnly = targets.TransferMode == CrossAdapterTransferMode::CopyOnlyCrossAdapter;
    const auto* primeColor = copyOnly
                                 ? &targets.CopyOnlyColor.PrimeBuffer
                                 : &targets.CrossAdapterColor->GetPrimeResource();
    const auto* primeDepth = copyOnly
                                 ? &targets.CopyOnlyLinearDepth.PrimeBuffer
                                 : &targets.CrossAdapterLinearDepth->GetPrimeResource();

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCopy,
                                         VoxelBenchmarkProfiler::RangeId::PrimarySharedToLocalCopy);
    AssertCopyOnlyBeforeBufferToTextureCopy(targets);
    if (copyOnly)
    {
        cmdList->TransitionBarrier(*primeColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(*primeDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    else
    {
        cmdList->TransitionBarrier(*primeColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(*primeDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryLinearDepth, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    cmdList->FlushResourceBarriers();

    if (copyOnly)
    {
        const bool useDirtyRect = context.UseDirtyRect && RectWidth(context.DirtyRect) > 0 &&
            RectHeight(context.DirtyRect) > 0;
        const auto dirtyBox = BoxFromRect(context.DirtyRect);
        CopyPlacedBufferToTexture(
            cmdList,
            targets.PrimaryReceivedSecondaryColor,
            targets.CopyOnlyColor.PrimeBuffer,
            targets.CopyOnlyColor.Footprint,
            useDirtyRect ? &dirtyBox : nullptr,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.left) : 0,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.top) : 0);
        CopyPlacedBufferToTexture(
            cmdList,
            targets.PrimaryReceivedSecondaryLinearDepth,
            targets.CopyOnlyLinearDepth.PrimeBuffer,
            targets.CopyOnlyLinearDepth.Footprint,
            useDirtyRect ? &dirtyBox : nullptr,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.left) : 0,
            useDirtyRect ? static_cast<UINT>(context.DirtyRect.top) : 0);
    }
    else
    {
        if (context.UseDirtyRect)
        {
            CopyTextureRectNoBarrier(cmdList, targets.PrimaryReceivedSecondaryColor,
                                     const_cast<GResource&>(*primeColor), context.DirtyRect);
            CopyTextureRectNoBarrier(cmdList, targets.PrimaryReceivedSecondaryLinearDepth,
                                     const_cast<GResource&>(*primeDepth), context.DirtyRect);
        }
        else
        {
            cmdList->CopyResourceNoBarrier(targets.PrimaryReceivedSecondaryColor, *primeColor);
            cmdList->CopyResourceNoBarrier(targets.PrimaryReceivedSecondaryLinearDepth, *primeDepth);
        }
    }

    cmdList->TransitionBarrier(*primeColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(*primeDepth, D3D12_RESOURCE_STATE_COMMON);
    if (!copyOnly)
    {
        cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COMMON);
        cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryLinearDepth,
                                   D3D12_RESOURCE_STATE_COMMON);
    }
    cmdList->FlushResourceBarriers();
    context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCopy,
                                       VoxelBenchmarkProfiler::RangeId::PrimarySharedToLocalCopy);
    context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCopy,
                                           VoxelBenchmarkProfiler::RangeId::PrimarySharedToLocalCopy);
    cmdList->EndQuery(context.TimestampHeapIndex + 1);
    cmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

    context.PrimaryCopyQueue->Wait(context.CrossAdapterRenderReadyFence,
                                   context.CrossAdapterRenderReadyFenceValue);
    context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue =
        context.PrimaryCopyQueue->ExecuteCommandList(cmdList);
    AssertCopyOnlyAfterCopySubmission(targets);
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::PrimaryCopy,
                                            context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue);
    context.CurrentFrameResource.PrimeCopyFenceValue =
        context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue;
    context.CurrentFrameResource.PrimarySecondaryImageReadyFenceValue =
        context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue;

    if (context.Telemetry)
    {
        const bool useDirtyRect = context.UseDirtyRect && RectWidth(context.DirtyRect) > 0 &&
            RectHeight(context.DirtyRect) > 0;
        const UINT64 fullColorBytes = copyOnly
                                          ? targets.CopyOnlyColor.TotalBytes
                                          : ExpectedCopyableBytes(targets.PrimaryReceivedSecondaryColor);
        const UINT64 fullDepthBytes = copyOnly
                                          ? targets.CopyOnlyLinearDepth.TotalBytes
                                          : ExpectedCopyableBytes(targets.PrimaryReceivedSecondaryLinearDepth);
        const UINT64 expectedColorBytes =
            useDirtyRect && copyOnly
                ? DirtyBytesForFootprint(targets.CopyOnlyColor.Footprint, context.DirtyRect)
                : fullColorBytes;
        const UINT64 expectedDepthBytes =
            useDirtyRect && copyOnly
                ? DirtyBytesForFootprint(targets.CopyOnlyLinearDepth.Footprint, context.DirtyRect)
                : fullDepthBytes;
        context.Telemetry->SharedToLocalCommandListSubmissionCount += 1;
        context.Telemetry->PrimarySharedToLocalCopyFenceValue =
            context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue;
        context.Telemetry->PrimarySecondaryImageReadyFenceValue =
            context.CurrentFrameResource.PrimarySecondaryImageReadyFenceValue;
        context.Telemetry->ExpectedSharedToLocalColorBytes = expectedColorBytes;
        context.Telemetry->ExpectedSharedToLocalDepthBytes = expectedDepthBytes;
        context.Telemetry->SharedToLocalColorBytes = expectedColorBytes;
        context.Telemetry->SharedToLocalDepthBytes = expectedDepthBytes;
        context.Telemetry->SharedToLocalColorSource = ResourceIdentity(*primeColor);
        context.Telemetry->SharedToLocalColorDestination = ResourceIdentity(targets.PrimaryReceivedSecondaryColor);
        context.Telemetry->SharedToLocalDepthSource = ResourceIdentity(*primeDepth);
        context.Telemetry->SharedToLocalDepthDestination = ResourceIdentity(targets.PrimaryReceivedSecondaryLinearDepth);
        context.Telemetry->CopyOperation = copyOnly ? "CopyTextureRegion" : "CopyResource";
        context.Telemetry->SharedToLocalPath = copyOnly ? "placed_buffer_to_texture" : "shared_texture_to_texture";
        context.Telemetry->BridgeResourceDimension = copyOnly ? "BUFFER" : "TEXTURE2D";
        context.Telemetry->BridgeColorBytes = copyOnly ? targets.CopyOnlyColor.TotalBytes : expectedColorBytes;
        context.Telemetry->BridgeDepthBytes = copyOnly ? targets.CopyOnlyLinearDepth.TotalBytes : expectedDepthBytes;
        context.Telemetry->BridgeColorRowPitch = copyOnly ? targets.CopyOnlyColor.Footprint.Footprint.RowPitch : 0;
        context.Telemetry->BridgeDepthRowPitch =
            copyOnly ? targets.CopyOnlyLinearDepth.Footprint.Footprint.RowPitch : 0;
        context.Telemetry->SharedToLocalTimestampBeginQuery = context.TimestampHeapIndex;
        context.Telemetry->SharedToLocalTimestampEndQuery = context.TimestampHeapIndex + 1;
        context.Telemetry->FullFrameTransferBytes = fullColorBytes + fullDepthBytes;
        context.Telemetry->ActualTransferBytes = expectedColorBytes + expectedDepthBytes;
        RecomputeRenderOutputTransferTelemetry(*context.Telemetry);
    }
}

void RenderPipeline::SubmitFinalCompositeAndPresentPass(
    const FinalCompositeAndPresentPassContext& context) const
{
    const auto cmdList = context.RenderQueue->GetCommandList();

    context.RecordFinalPresentCommands(cmdList);

    if (context.PrimaryBaseRenderFenceValue != 0)
        context.RenderQueue->Wait(context.PrimaryBaseRenderFence, context.PrimaryBaseRenderFenceValue);
    if (context.WaitForSecondaryImage && context.PrimarySecondaryImageReadyFenceValue != 0)
    {
        context.RenderQueue->Wait(context.PrimarySecondaryImageReadyFence,
                                  context.PrimarySecondaryImageReadyFenceValue);
    }

    context.CurrentFrameResource.PrimeRenderFenceValue = context.RenderQueue->ExecuteCommandList(cmdList);
    context.GraphicsPassFenceValue = context.CurrentFrameResource.PrimeRenderFenceValue;
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                            context.GraphicsPassFenceValue);

    if (context.Telemetry)
        context.Telemetry->FinalPresentFenceValue = context.GraphicsPassFenceValue;
}
