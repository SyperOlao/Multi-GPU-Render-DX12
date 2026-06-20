#include "Source/Rendering/RenderPipeline.h"

#include "FrameResource.h"
#include "GCommandList.h"
#include "GCommandQueue.h"

using namespace PEPEngine::Graphics;

void RenderPipeline::RenderFrame(const RenderPipelineContext& context) const
{
    const auto cmdList = context.RenderQueue->GetCommandList();

    cmdList->EndQuery(context.TimestampHeapIndex);
    context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::Graphics,
                                         VoxelBenchmarkProfiler::RangeId::Graphics);

    context.RecordGraphicsCommands(cmdList);

    context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::Graphics,
                                       VoxelBenchmarkProfiler::RangeId::Graphics);
    context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::Graphics,
                                           VoxelBenchmarkProfiler::RangeId::Graphics);
    cmdList->EndQuery(context.TimestampHeapIndex + 1);
    cmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

    context.RenderQueue->Wait(context.PrimaryComputeQueue);
    if (context.SecondaryWorkThisFrame)
        context.RenderQueue->Wait(context.CrossAdapterCopyQueue);

    context.CurrentFrameResource.PrimeRenderFenceValue = context.RenderQueue->ExecuteCommandList(cmdList);
    context.GraphicsPassFenceValue = context.CurrentFrameResource.PrimeRenderFenceValue;
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::Graphics,
                                            context.GraphicsPassFenceValue);
    if (context.UsedSplitMultiGpu)
    {
        context.SharedRenderFenceValue = context.CurrentFrameResource.PrimeRenderFenceValue;
        context.RenderQueue->Signal(context.PrimeRenderFence, context.SharedRenderFenceValue);
    }
}

