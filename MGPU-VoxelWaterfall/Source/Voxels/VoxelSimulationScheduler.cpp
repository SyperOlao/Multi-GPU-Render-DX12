#include "Source/Voxels/VoxelSimulationScheduler.h"

#include "GCommandList.h"
#include "Source/Voxels/VoxelGpuPartition.h"

#include <algorithm>
#include <cmath>

namespace
{
    VoxelBenchmarkProfiler::RangeId RangeForPartition(const VoxelPartitionState& partition)
    {
        return partition.PartitionId == VoxelPartitionId::PrimaryPartition
                   ? VoxelBenchmarkProfiler::RangeId::PrimaryCompute
                   : VoxelBenchmarkProfiler::RangeId::SecondaryCompute;
    }

    bool HasDispatchableEmitter(const VoxelPartitionState& partition)
    {
        return partition.GpuPartition && !partition.GlobalVoxelIds.empty();
    }

    VoxelBenchmarkProfiler::QueueId QueueForPartition(const VoxelPartitionState& partition)
    {
        return partition.AdapterOwner == VoxelAdapterOwner::Secondary
                   ? VoxelBenchmarkProfiler::QueueId::SecondaryCompute
                   : VoxelBenchmarkProfiler::QueueId::PrimaryCompute;
    }
}

void VoxelSimulationScheduler::PreparePartitionDispatch(
    const VoxelSimulationSchedulerContext& context,
    VoxelPartitionState& partition)
{
    partition.UpdateInterval = 1;
    if (partition.GpuPartition)
    {
        partition.GpuPartition->SetUpdateInterval(1);
        partition.GpuPartition->SetSimulationDeltaTime(static_cast<float>(FixedSimulationDeltaTime));
        partition.GpuPartition->SetSimulationTime(static_cast<float>(context.SimulationTime));
    }
}

void VoxelSimulationScheduler::MarkPartitionUpdated(
    const VoxelSimulationSchedulerContext& context,
    VoxelPartitionState& partition)
{
    partition.UpdatedThisFrame = true;
    partition.LastSimulationFrame = context.SimulationFrameIndex;

    if (partition.GpuPartition)
    {
        partition.GpuPartition->SetLastSimulationFrame(context.SimulationFrameIndex);
        partition.UpdatedVoxelCount = partition.GpuPartition->GetLastDispatchVoxelCount();
    }
}

VoxelSimulationSchedulerResult VoxelSimulationScheduler::DispatchFrame(
    const VoxelSimulationSchedulerContext& context) const
{
    VoxelSimulationSchedulerResult result{};
    result.UsedMultiGpuMode =
        (context.ExecutionMode == VoxelExecutionMode::MultiGpuFull ||
         context.ExecutionMode == VoxelExecutionMode::MultiGpuTemporalDecimation) &&
        context.MultiGpuAvailable;

    for (auto& partition : context.Workload.Partitions)
    {
        partition.UpdatedThisFrame = false;
        partition.UpdatedVoxelCount = 0;
    }

    const double clampedFrameDelta = std::clamp(context.FrameDeltaTime, 0.0, MaxAccumulatedSimulationTime);
    context.SimulationAccumulator = std::min(context.SimulationAccumulator + clampedFrameDelta,
                                             MaxAccumulatedSimulationTime);
    const auto stepsToRun = static_cast<uint32_t>(
        std::floor(context.SimulationAccumulator / FixedSimulationDeltaTime));
    context.SimulationAccumulator -= static_cast<double>(stepsToRun) * FixedSimulationDeltaTime;
    context.SimulationStepsThisFrame = stepsToRun;
    context.InterpolationAlpha = static_cast<float>(
        std::clamp(context.SimulationAccumulator / FixedSimulationDeltaTime, 0.0, 1.0));

    bool needsSecondaryComputeQueue = false;
    for (const auto& partition : context.Workload.Partitions)
    {
        needsSecondaryComputeQueue = needsSecondaryComputeQueue ||
            (HasDispatchableEmitter(partition) && partition.AdapterOwner == VoxelAdapterOwner::Secondary);
    }

    const auto primaryCmdList = context.PrimaryComputeQueue->GetCommandList();
    primaryCmdList->EndQuery(context.TimestampHeapIndex);

    std::shared_ptr<PEPEngine::Graphics::GCommandList> secondaryCmdList;
    if (stepsToRun > 0 && needsSecondaryComputeQueue)
    {
        secondaryCmdList = context.SecondaryComputeQueue->GetCommandList();
        secondaryCmdList->EndQuery(context.TimestampHeapIndex);
    }

    for (auto& partition : context.Workload.Partitions)
    {
        if (partition.GpuPartition)
            partition.GpuPartition->BeginSimulationFrame();
    }

    for (uint32_t stepIndex = 0; stepIndex < stepsToRun; ++stepIndex)
    {
        context.SimulationTime += FixedSimulationDeltaTime;
        ++context.SimulationFrameIndex;

        for (auto& partition : context.Workload.Partitions)
        {
            if (!HasDispatchableEmitter(partition))
                continue;

            PreparePartitionDispatch(context, partition);
            auto cmdList = primaryCmdList;
            if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
                cmdList = secondaryCmdList;
            if (!cmdList)
                continue;

            const auto range = RangeForPartition(partition);
            const auto queueId = QueueForPartition(partition);
            context.BenchmarkProfiler.BeginRange(cmdList, queueId, range);
            partition.GpuPartition->DispatchSimulation(cmdList);
            context.BenchmarkProfiler.EndRange(cmdList, queueId, range);
            context.BenchmarkProfiler.ResolveRange(cmdList, queueId, range);
            MarkPartitionUpdated(context, partition);

            if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
                result.SecondaryWorkThisFrame = true;
        }
    }

    context.RecycledVoxelCount = 0;
    context.AliveVoxelCount = 0;
    context.ExpectedVoxelCount = 0;
    for (auto& partition : context.Workload.Partitions)
    {
        if (!partition.GpuPartition)
            continue;

        partition.GpuPartition->SetInterpolationAlpha(context.InterpolationAlpha);
        const auto statistics = partition.GpuPartition->GetStatistics();
        context.RecycledVoxelCount += stepsToRun > 0 ? statistics.LastRecycledVoxelCount : 0;
        context.AliveVoxelCount += statistics.LastAliveVoxelCount;
        context.ExpectedVoxelCount += statistics.ExpectedVoxelCount;
    }

    primaryCmdList->EndQuery(context.TimestampHeapIndex + 1);
    primaryCmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));

    context.CurrentFrameComputeFenceValue = context.PrimaryComputeQueue->ExecuteCommandList(primaryCmdList);
    context.PrimaryComputeFenceValue = context.CurrentFrameComputeFenceValue;
    result.PrimaryComputeSubmitted = true;
    result.PrimaryComputeFenceValue = context.PrimaryComputeFenceValue;
    context.SecondaryComputeFenceValue = 0;
    if (secondaryCmdList)
    {
        secondaryCmdList->EndQuery(context.TimestampHeapIndex + 1);
        secondaryCmdList->ResolveQuery(context.TimestampHeapIndex, 2, context.TimestampHeapIndex * sizeof(UINT64));
        context.SecondaryComputeFenceValue = context.SecondaryComputeQueue->ExecuteCommandList(secondaryCmdList);
        result.SecondaryComputeSubmitted = true;
        result.SecondaryComputeFenceValue = context.SecondaryComputeFenceValue;
        context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::SecondaryCompute,
                                                context.SecondaryComputeFenceValue);
    }
    context.BenchmarkProfiler.SetQueueFence(VoxelBenchmarkProfiler::QueueId::PrimaryCompute,
                                            context.PrimaryComputeFenceValue);

    return result;
}
