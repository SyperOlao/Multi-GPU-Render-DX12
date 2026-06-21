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

    bool IsTemporalMode(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
               mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }
}

uint32_t VoxelSimulationScheduler::GetEffectiveUpdateInterval(
    const VoxelExecutionMode mode,
    const VoxelPartitionState& partition)
{
    if (partition.PartitionId == VoxelPartitionId::SecondaryPartition && IsTemporalMode(mode))
        return std::max<uint32_t>(1, partition.UpdateInterval);

    return 1;
}

bool VoxelSimulationScheduler::ShouldDispatchPartition(
    const VoxelPartitionState& partition,
    const uint64_t fixedStepIndex,
    const uint32_t effectiveInterval)
{
    if (!HasDispatchableEmitter(partition))
        return false;

    if (!partition.GpuPartition->HasStartedSimulation())
        return true;

    return effectiveInterval <= 1 || fixedStepIndex % effectiveInterval == 0;
}

float VoxelSimulationScheduler::CalculateInterpolationPhase(
    const VoxelSimulationSchedulerContext& context,
    const VoxelPartitionState& partition,
    const uint32_t effectiveInterval)
{
    if (!partition.GpuPartition || !partition.GpuPartition->HasStartedSimulation())
        return 0.0f;

    const uint64_t completedStepsSinceLastUpdate =
        context.SimulationFrameIndex > partition.LastSimulationFrame
            ? context.SimulationFrameIndex - partition.LastSimulationFrame - 1
            : 0;
    const double phase =
        (static_cast<double>(completedStepsSinceLastUpdate) + context.InterpolationAlpha) /
        static_cast<double>(std::max<uint32_t>(1, effectiveInterval));
    return static_cast<float>(std::clamp(phase, 0.0, 1.0));
}

void VoxelSimulationScheduler::PreparePartitionDispatch(
    const VoxelSimulationSchedulerContext&,
    VoxelPartitionState& partition,
    const uint32_t effectiveInterval,
    const uint64_t fixedStepIndex)
{
    partition.EffectiveUpdateInterval = effectiveInterval;
    partition.CoarseDeltaTime = static_cast<float>(
        FixedSimulationDeltaTime * static_cast<double>(effectiveInterval));
    if (partition.GpuPartition)
    {
        partition.GpuPartition->SetUpdateInterval(effectiveInterval);
        partition.GpuPartition->SetSimulationDeltaTime(partition.CoarseDeltaTime);
        const double targetSimulationTime =
            static_cast<double>(fixedStepIndex + effectiveInterval) * FixedSimulationDeltaTime;
        partition.GpuPartition->SetSimulationTime(static_cast<float>(targetSimulationTime));
    }
}

void VoxelSimulationScheduler::MarkPartitionUpdated(
    VoxelPartitionState& partition,
    const uint64_t fixedStepIndex)
{
    partition.UpdatedThisFrame = true;
    partition.SimulationDispatchedThisFrame = true;
    partition.LastSimulationFrame = fixedStepIndex;

    if (partition.GpuPartition)
    {
        partition.GpuPartition->SetLastSimulationFrame(fixedStepIndex);
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
        partition.SimulationDispatchedThisFrame = false;
        partition.UpdatedVoxelCount = 0;
        partition.EffectiveUpdateInterval = GetEffectiveUpdateInterval(context.ExecutionMode, partition);
        partition.CoarseDeltaTime = static_cast<float>(
            FixedSimulationDeltaTime * static_cast<double>(partition.EffectiveUpdateInterval));
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
    for (uint32_t stepIndex = 0; stepIndex < stepsToRun && !needsSecondaryComputeQueue; ++stepIndex)
    {
        const uint64_t fixedStepIndex = context.SimulationFrameIndex + stepIndex;
        for (const auto& partition : context.Workload.Partitions)
        {
            const uint32_t effectiveInterval = GetEffectiveUpdateInterval(context.ExecutionMode, partition);
            needsSecondaryComputeQueue = needsSecondaryComputeQueue ||
                (partition.AdapterOwner == VoxelAdapterOwner::Secondary &&
                 ShouldDispatchPartition(partition, fixedStepIndex, effectiveInterval));
        }
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
        const uint64_t fixedStepIndex = context.SimulationFrameIndex;

        for (auto& partition : context.Workload.Partitions)
        {
            const uint32_t effectiveInterval = GetEffectiveUpdateInterval(context.ExecutionMode, partition);
            if (!ShouldDispatchPartition(partition, fixedStepIndex, effectiveInterval))
                continue;

            PreparePartitionDispatch(context, partition, effectiveInterval, fixedStepIndex);
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
            MarkPartitionUpdated(partition, fixedStepIndex);

            if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
                result.SecondaryWorkThisFrame = true;
        }

        context.SimulationTime += FixedSimulationDeltaTime;
        ++context.SimulationFrameIndex;
    }

    context.RecycledVoxelCount = 0;
    context.AliveVoxelCount = 0;
    context.ExpectedVoxelCount = 0;
    for (auto& partition : context.Workload.Partitions)
    {
        if (!partition.GpuPartition)
            continue;

        partition.EffectiveUpdateInterval = GetEffectiveUpdateInterval(context.ExecutionMode, partition);
        partition.StepsSinceLastUpdate =
            partition.GpuPartition->HasStartedSimulation() && context.SimulationFrameIndex > partition.LastSimulationFrame
                ? static_cast<uint32_t>(
                    std::min<uint64_t>(
                        context.SimulationFrameIndex - partition.LastSimulationFrame,
                        partition.EffectiveUpdateInterval))
                : 0;
        partition.InterpolationPhase = CalculateInterpolationPhase(
            context, partition, partition.EffectiveUpdateInterval);
        partition.GpuPartition->SetInterpolationAlpha(partition.InterpolationPhase);
        const auto statistics = partition.GpuPartition->GetStatistics();
        context.RecycledVoxelCount += partition.SimulationDispatchedThisFrame ? statistics.LastRecycledVoxelCount : 0;
        context.AliveVoxelCount += statistics.LastAliveVoxelCount;
        context.ExpectedVoxelCount += statistics.ExpectedVoxelCount;
    }

    const auto& secondaryPartition =
        context.Workload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)];
    result.SecondaryConfiguredUpdateInterval = std::max<uint32_t>(1, secondaryPartition.UpdateInterval);
    result.SecondaryEffectiveUpdateInterval = secondaryPartition.EffectiveUpdateInterval;
    result.FixedSimulationStepIndex = context.SimulationFrameIndex;
    result.SecondaryStepsSinceLastUpdate = secondaryPartition.StepsSinceLastUpdate;
    result.SecondaryInterpolationPhase = secondaryPartition.InterpolationPhase;
    result.SecondaryCoarseDeltaTime = secondaryPartition.CoarseDeltaTime;

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
