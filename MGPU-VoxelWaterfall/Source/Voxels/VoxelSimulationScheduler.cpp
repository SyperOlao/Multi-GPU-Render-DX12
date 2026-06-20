#include "Source/Voxels/VoxelSimulationScheduler.h"

#include "CrossAdapterVoxelEmitter.h"
#include "GCommandList.h"
#include "VoxelWaterfallEmitter.h"

#include <algorithm>

uint32_t VoxelSimulationScheduler::EffectiveInterval(const VoxelSimulationSchedulerContext& context,
                                                     const size_t lodIndex)
{
    if (context.ExecutionMode == VoxelExecutionMode::PrimaryOnly ||
        context.ExecutionMode == VoxelExecutionMode::SplitMultiGpu ||
        lodIndex == NearVoxelWaterfall)
    {
        return 1;
    }

    return std::max<uint32_t>(1, context.Lods[lodIndex].UpdateInterval);
}

bool VoxelSimulationScheduler::ShouldUpdateLod(const VoxelSimulationSchedulerContext& context, const size_t lodIndex)
{
    const auto& lod = context.Lods[lodIndex];
    if (!lod.Enabled)
        return false;

    const uint32_t interval = EffectiveInterval(context, lodIndex);
    return interval == 1 || context.SimulationFrameIndex % interval == 0;
}

void VoxelSimulationScheduler::PrepareLodDispatch(const VoxelSimulationSchedulerContext& context,
                                                  const size_t lodIndex)
{
    auto& lod = context.Lods[lodIndex];
    const uint32_t interval = EffectiveInterval(context, lodIndex);
    const float deltaTime = std::min(FixedSimulationDeltaTime * static_cast<float>(interval), MaxSimulationDeltaTime);

    if (lod.CrossEmitter)
    {
        lod.CrossEmitter->SetUpdateInterval(interval);
        lod.CrossEmitter->SetSimulationDeltaTime(deltaTime);
    }
    else if (lod.Emitter)
    {
        lod.Emitter->SetUpdateInterval(interval);
        lod.Emitter->SetSimulationDeltaTime(deltaTime);
    }
}

void VoxelSimulationScheduler::MarkLodUpdated(const VoxelSimulationSchedulerContext& context, const size_t lodIndex)
{
    auto& lod = context.Lods[lodIndex];
    lod.UpdatedThisFrame = true;
    lod.LastSimulationFrame = context.SimulationFrameIndex;

    if (lod.CrossEmitter)
    {
        lod.CrossEmitter->SetLastSimulationFrame(context.SimulationFrameIndex);
        lod.UpdatedVoxelCount = lod.CrossEmitter->GetLastDispatchVoxelCount();
    }
    else if (lod.Emitter)
    {
        lod.Emitter->SetLastSimulationFrame(context.SimulationFrameIndex);
        lod.UpdatedVoxelCount = lod.Emitter->GetLastDispatchVoxelCount();
    }
}

VoxelSimulationSchedulerResult VoxelSimulationScheduler::DispatchFrame(
    const VoxelSimulationSchedulerContext& context) const
{
    VoxelSimulationSchedulerResult result{};
    result.UsedSplitMultiGpu = context.ExecutionMode != VoxelExecutionMode::PrimaryOnly &&
        context.SplitMultiGpuAvailable;

    for (auto& lod : context.Lods)
    {
        lod.UpdatedThisFrame = false;
        lod.UpdatedVoxelCount = 0;
    }

    const bool updateNear = ShouldUpdateLod(context, NearVoxelWaterfall);
    const bool updateMedium = ShouldUpdateLod(context, MediumVoxelWaterfall);
    const bool updateFar = ShouldUpdateLod(context, FarVoxelWaterfall);
    result.SecondaryWorkThisFrame = result.UsedSplitMultiGpu && (updateMedium || updateFar);

    context.PrimaryComputeQueue->Wait(context.RenderQueue);
    if (result.SecondaryWorkThisFrame)
        context.SecondaryComputeQueue->Wait(context.SecondRenderFence, context.SecondRenderFenceValue);

    {
        const auto cmdList = context.PrimaryComputeQueue->GetCommandList();
        cmdList->EndQuery(context.TimestampHeapIndex);

        if (updateNear && context.Lods[NearVoxelWaterfall].Emitter)
        {
            PrepareLodDispatch(context, NearVoxelWaterfall);
            context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCompute,
                                                 VoxelBenchmarkProfiler::RangeId::NearCompute);
            context.Lods[NearVoxelWaterfall].Emitter->Dispatch(cmdList);
            context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCompute,
                                               VoxelBenchmarkProfiler::RangeId::NearCompute);
            context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCompute,
                                                   VoxelBenchmarkProfiler::RangeId::NearCompute);
            MarkLodUpdated(context, NearVoxelWaterfall);
        }

        if (!result.UsedSplitMultiGpu)
        {
            for (size_t i = MediumVoxelWaterfall; i <= FarVoxelWaterfall; ++i)
            {
                if (!ShouldUpdateLod(context, i))
                    continue;

                PrepareLodDispatch(context, i);
                const auto range = i == MediumVoxelWaterfall
                                       ? VoxelBenchmarkProfiler::RangeId::MediumCompute
                                       : VoxelBenchmarkProfiler::RangeId::FarCompute;
                context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCompute, range);
                if (context.Lods[i].CrossEmitter)
                    context.Lods[i].CrossEmitter->Dispatch(cmdList);
                else if (context.Lods[i].Emitter)
                    context.Lods[i].Emitter->Dispatch(cmdList);
                context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCompute, range);
                context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryCompute, range);
                MarkLodUpdated(context, i);
            }
        }

        cmdList->EndQuery(context.TimestampHeapIndex + 1);
        cmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

        context.CurrentFrameComputeFenceValue = context.PrimaryComputeQueue->ExecuteCommandList(cmdList);
        context.PrimaryComputeFenceValue = context.CurrentFrameComputeFenceValue;
        context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::PrimaryCompute,
                                                context.PrimaryComputeFenceValue);
    }

    if (!result.SecondaryWorkThisFrame)
        return result;

    {
        const auto cmdList = context.SecondaryComputeQueue->GetCommandList();

        for (size_t i = MediumVoxelWaterfall; i <= FarVoxelWaterfall; ++i)
        {
            if (ShouldUpdateLod(context, i) && context.Lods[i].CrossEmitter)
            {
                PrepareLodDispatch(context, i);
                const auto range = i == MediumVoxelWaterfall
                                       ? VoxelBenchmarkProfiler::RangeId::MediumCompute
                                       : VoxelBenchmarkProfiler::RangeId::FarCompute;
                context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCompute, range);
                context.Lods[i].CrossEmitter->Dispatch(cmdList);
                context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCompute, range);
                context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::SecondaryCompute,
                                                       range);
                MarkLodUpdated(context, i);
            }
        }

        context.SecondaryComputeFenceValue = context.SecondaryComputeQueue->ExecuteCommandList(cmdList);
        context.SharedComputeFenceValue = context.SecondaryComputeFenceValue;
        context.SecondaryComputeQueue->Signal(context.SecondComputeFence, context.SharedComputeFenceValue);
        context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::SecondaryCompute,
                                                context.SecondaryComputeFenceValue);
    }

    {
        context.CrossAdapterCopyQueue->Wait(context.PrimeComputeFence, context.SharedComputeFenceValue);

        const auto cmdList = context.CrossAdapterCopyQueue->GetCommandList();
        context.BenchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::Transfer,
                                             VoxelBenchmarkProfiler::RangeId::CrossAdapterTransfer);
        for (size_t i = MediumVoxelWaterfall; i <= FarVoxelWaterfall; ++i)
        {
            if (context.Lods[i].CrossEmitter)
                context.Lods[i].CrossEmitter->CopySharedToPrimary(cmdList);
        }
        context.BenchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::Transfer,
                                           VoxelBenchmarkProfiler::RangeId::CrossAdapterTransfer);
        context.BenchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::Transfer,
                                               VoxelBenchmarkProfiler::RangeId::CrossAdapterTransfer);

        context.CrossAdapterDataReadyFenceValue = context.CrossAdapterCopyQueue->ExecuteCommandList(cmdList);
        context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::Transfer,
                                                context.CrossAdapterDataReadyFenceValue);
    }

    return result;
}
