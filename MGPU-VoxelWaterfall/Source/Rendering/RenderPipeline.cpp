#include "Source/Rendering/RenderPipeline.h"

#include "FrameResource.h"
#include "GCommandList.h"
#include "GCommandQueue.h"
#include "Source/Voxels/VoxelGpuPartition.h"

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

    void CopyTextureToPlacedBuffer(
        const std::shared_ptr<GCommandList>& cmdList,
        const GResource& destinationBuffer,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& destinationFootprint,
        const GResource& sourceTexture)
    {
        const auto destinationDesc = destinationBuffer.GetD3D12ResourceDesc();
        const auto sourceDesc = sourceTexture.GetD3D12ResourceDesc();

        assert(destinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        assert(destinationFootprint.Footprint.Format == sourceDesc.Format);
        assert(destinationFootprint.Footprint.Width == sourceDesc.Width);
        assert(destinationFootprint.Footprint.Height == sourceDesc.Height);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = destinationBuffer.GetD3D12Resource().Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = destinationFootprint;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = sourceTexture.GetD3D12Resource().Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        cmdList->GetGraphicsCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

    void CopyPlacedBufferToTexture(
        const std::shared_ptr<GCommandList>& cmdList,
        const GResource& destinationTexture,
        const GResource& sourceBuffer,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& sourceFootprint)
    {
        const auto destinationDesc = destinationTexture.GetD3D12ResourceDesc();
        const auto sourceDesc = sourceBuffer.GetD3D12ResourceDesc();

        assert(destinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        assert(sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(sourceFootprint.Footprint.Format == destinationDesc.Format);
        assert(sourceFootprint.Footprint.Width == destinationDesc.Width);
        assert(sourceFootprint.Footprint.Height == destinationDesc.Height);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = destinationTexture.GetD3D12Resource().Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = sourceBuffer.GetD3D12Resource().Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = sourceFootprint;

        cmdList->GetGraphicsCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
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

void RenderPipeline::SubmitPrimaryBasePass(const PrimaryBasePassContext& context) const
{
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
    const auto cmdList = context.SecondaryGraphicsQueue->GetCommandList();
    auto& targets = context.RenderTargets;

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
    for (const auto* partition : context.SecondaryPartitions)
    {
        if (!partition || !partition->GpuPartition || partition->VoxelCount() == 0)
            continue;

        partition->GpuPartition->UpdateFrameConstants();
        auto result = partition->GpuPartition->RecordRender(
            cmdList,
            VoxelPartitionRenderOutputMode::SecondaryColorAndLinearDepth,
            context.CurrentFrameResource.SecondaryPassConstantUploadBuffer.get(),
            &context.BenchmarkProfiler,
            VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
            VoxelBenchmarkProfiler::RangeId::SecondaryLodCompaction);
        result.PartitionId = partition->PartitionId;
        result.LogicalVoxelCount = partition->VoxelCount();
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
    if (copyOnly)
    {
        cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
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
        CopyTextureToPlacedBuffer(
            cmdList,
            targets.CopyOnlyColor.SharedBuffer,
            targets.CopyOnlyColor.Footprint,
            targets.SecondaryLocalColor);
        CopyTextureToPlacedBuffer(
            cmdList,
            targets.CopyOnlyLinearDepth.SharedBuffer,
            targets.CopyOnlyLinearDepth.Footprint,
            targets.SecondaryLocalLinearDepth);
    }
    else
    {
        cmdList->CopyResourceNoBarrier(*sharedColor, targets.SecondaryLocalColor);
        cmdList->CopyResourceNoBarrier(*sharedDepth, targets.SecondaryLocalLinearDepth);
    }

    cmdList->TransitionBarrier(*sharedColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(*sharedDepth, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COMMON);
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
        const UINT64 expectedColorBytes = copyOnly
                                              ? targets.CopyOnlyColor.TotalBytes
                                              : ExpectedCopyableBytes(targets.SecondaryLocalColor);
        const UINT64 expectedDepthBytes = copyOnly
                                              ? targets.CopyOnlyLinearDepth.TotalBytes
                                              : ExpectedCopyableBytes(targets.SecondaryLocalLinearDepth);
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
        context.Telemetry->ColorBytesTransferred += expectedColorBytes;
        context.Telemetry->DepthBytesTransferred += expectedDepthBytes;
        context.Telemetry->TotalCrossAdapterBytes =
            context.Telemetry->ColorBytesTransferred + context.Telemetry->DepthBytesTransferred;
        context.Telemetry->ParticleTransferBytes = 0;
        context.Telemetry->RenderOutputTransferBytes =
            context.Telemetry->LocalToSharedColorBytes + context.Telemetry->LocalToSharedDepthBytes +
            context.Telemetry->SharedToLocalColorBytes + context.Telemetry->SharedToLocalDepthBytes;
    }
}

void RenderPipeline::SubmitPrimarySharedToLocalCopyPass(
    const PrimarySharedToLocalCopyPassContext& context) const
{
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
    if (copyOnly)
    {
        cmdList->TransitionBarrier(*primeColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(*primeDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryLinearDepth, D3D12_RESOURCE_STATE_COPY_DEST);
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
        CopyPlacedBufferToTexture(
            cmdList,
            targets.PrimaryReceivedSecondaryColor,
            targets.CopyOnlyColor.PrimeBuffer,
            targets.CopyOnlyColor.Footprint);
        CopyPlacedBufferToTexture(
            cmdList,
            targets.PrimaryReceivedSecondaryLinearDepth,
            targets.CopyOnlyLinearDepth.PrimeBuffer,
            targets.CopyOnlyLinearDepth.Footprint);
    }
    else
    {
        cmdList->CopyResourceNoBarrier(targets.PrimaryReceivedSecondaryColor, *primeColor);
        cmdList->CopyResourceNoBarrier(targets.PrimaryReceivedSecondaryLinearDepth, *primeDepth);
    }

    cmdList->TransitionBarrier(*primeColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(*primeDepth, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryLinearDepth,
                               D3D12_RESOURCE_STATE_COMMON);
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
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::PrimaryCopy,
                                            context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue);
    context.CurrentFrameResource.PrimeCopyFenceValue =
        context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue;
    context.CurrentFrameResource.PrimarySecondaryImageReadyFenceValue =
        context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue;

    if (context.Telemetry)
    {
        const UINT64 expectedColorBytes = copyOnly
                                              ? targets.CopyOnlyColor.TotalBytes
                                              : ExpectedCopyableBytes(targets.PrimaryReceivedSecondaryColor);
        const UINT64 expectedDepthBytes = copyOnly
                                              ? targets.CopyOnlyLinearDepth.TotalBytes
                                              : ExpectedCopyableBytes(targets.PrimaryReceivedSecondaryLinearDepth);
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
        context.Telemetry->ColorBytesTransferred += expectedColorBytes;
        context.Telemetry->DepthBytesTransferred += expectedDepthBytes;
        context.Telemetry->TotalCrossAdapterBytes =
            context.Telemetry->ColorBytesTransferred + context.Telemetry->DepthBytesTransferred;
        context.Telemetry->RenderOutputTransferBytes =
            context.Telemetry->LocalToSharedColorBytes + context.Telemetry->LocalToSharedDepthBytes +
            context.Telemetry->SharedToLocalColorBytes + context.Telemetry->SharedToLocalDepthBytes;
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
