#include "Source/Rendering/RenderPipeline.h"

#include "FrameResource.h"
#include "GCommandList.h"
#include "GCommandQueue.h"
#include "Source/Voxels/VoxelGpuPartition.h"

#include <cfloat>

using namespace PEPEngine::Graphics;

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

    context.SecondaryPartition.UpdateFrameConstants();
    const auto statistics = context.SecondaryPartition.GetStatistics();
    context.SecondaryPartition.RecordRender(
        cmdList,
        VoxelPartitionRenderOutputMode::SecondaryColorAndLinearDepth,
        context.CurrentFrameResource.SecondaryPassConstantUploadBuffer.get());

    cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->TransitionBarrier(targets.SecondaryLocalDepthStencil, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->FlushResourceBarriers();

    context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryGraphics,
                                       VoxelBenchmarkProfiler::RangeId::SecondaryGraphics);
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
        context.Telemetry->SecondaryGraphicsFenceValue =
            context.CurrentFrameResource.SecondaryRenderFenceValue;
        context.Telemetry->SecondaryDrawCalls = statistics.ExpectedVoxelCount > 0 ? 1u : 0u;
        context.Telemetry->SecondaryRenderedVoxelCount = statistics.LastAliveVoxelCount;
    }
}

void RenderPipeline::SubmitSecondaryLocalToSharedCopyPass(
    const SecondaryLocalToSharedCopyPassContext& context) const
{
    const auto cmdList = context.SecondaryCopyQueue->GetCommandList();
    auto& targets = context.RenderTargets;
    const auto& sharedColor = targets.CrossAdapterColor->GetSharedResource();
    const auto& sharedDepth = targets.CrossAdapterLinearDepth->GetSharedResource();

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCopy,
                                         VoxelBenchmarkProfiler::RangeId::SecondaryLocalToSharedCopy);
    cmdList->TransitionBarrier(targets.SecondaryLocalColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->TransitionBarrier(targets.SecondaryLocalLinearDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->TransitionBarrier(sharedColor, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->TransitionBarrier(sharedDepth, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->FlushResourceBarriers();

    cmdList->CopyResourceNoBarrier(sharedColor, targets.SecondaryLocalColor);
    cmdList->CopyResourceNoBarrier(sharedDepth, targets.SecondaryLocalLinearDepth);

    cmdList->TransitionBarrier(sharedColor, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->TransitionBarrier(sharedDepth, D3D12_RESOURCE_STATE_COPY_DEST);
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
        context.Telemetry->SecondaryLocalToSharedCopyFenceValue =
            context.CurrentFrameResource.SecondaryLocalToSharedCopyFenceValue;
        context.Telemetry->CrossAdapterRenderReadyFenceValue =
            context.CurrentFrameResource.CrossAdapterRenderReadyFenceValue;
        context.Telemetry->ColorBytesTransferred = targets.ColorTransferBytes;
        context.Telemetry->DepthBytesTransferred = targets.LinearDepthTransferBytes;
        context.Telemetry->TotalCrossAdapterBytes =
            targets.ColorTransferBytes + targets.LinearDepthTransferBytes;
        context.Telemetry->ParticleTransferBytes = 0;
        context.Telemetry->RenderOutputTransferBytes = context.Telemetry->TotalCrossAdapterBytes;
    }
}

void RenderPipeline::SubmitPrimarySharedToLocalCopyPass(
    const PrimarySharedToLocalCopyPassContext& context) const
{
    const auto cmdList = context.PrimaryCopyQueue->GetCommandList();
    auto& targets = context.RenderTargets;
    const auto& primeColor = targets.CrossAdapterColor->GetPrimeResource();
    const auto& primeDepth = targets.CrossAdapterLinearDepth->GetPrimeResource();

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCopy,
                                         VoxelBenchmarkProfiler::RangeId::PrimarySharedToLocalCopy);
    cmdList->TransitionBarrier(primeColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->TransitionBarrier(primeDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryLinearDepth, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->FlushResourceBarriers();

    cmdList->CopyResourceNoBarrier(targets.PrimaryReceivedSecondaryColor, primeColor);
    cmdList->CopyResourceNoBarrier(targets.PrimaryReceivedSecondaryLinearDepth, primeDepth);

    cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(targets.PrimaryReceivedSecondaryLinearDepth,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
        context.Telemetry->PrimarySharedToLocalCopyFenceValue =
            context.CurrentFrameResource.PrimarySharedToLocalCopyFenceValue;
        context.Telemetry->PrimarySecondaryImageReadyFenceValue =
            context.CurrentFrameResource.PrimarySecondaryImageReadyFenceValue;
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
