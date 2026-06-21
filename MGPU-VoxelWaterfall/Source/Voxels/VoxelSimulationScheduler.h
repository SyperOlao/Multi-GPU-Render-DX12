#pragma once

#include "GCommandQueue.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Voxels/VoxelTypes.h"

struct VoxelSimulationSchedulerContext
{
    VoxelSceneWorkload& Workload;
    VoxelExecutionMode ExecutionMode = VoxelExecutionMode::SingleGpuFull;
    bool MultiGpuAvailable = false;
    uint64_t& SimulationFrameIndex;
    uint32_t TimestampHeapIndex = 0;
    double FrameDeltaTime = 0.0;
    double& SimulationAccumulator;
    double& SimulationTime;
    uint32_t& SimulationStepsThisFrame;
    float& InterpolationAlpha;
    uint32_t& RecycledVoxelCount;
    uint32_t& AliveVoxelCount;
    uint32_t& ExpectedVoxelCount;

    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> PrimaryComputeQueue;
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> SecondaryComputeQueue;

    UINT64& PrimaryComputeFenceValue;
    UINT64& SecondaryComputeFenceValue;
    UINT64& CurrentFrameComputeFenceValue;

    VoxelBenchmarkProfiler& BenchmarkProfiler;
};

struct VoxelSimulationSchedulerResult
{
    bool UsedMultiGpuMode = false;
    bool SecondaryWorkThisFrame = false;
    bool PrimaryComputeSubmitted = false;
    bool SecondaryComputeSubmitted = false;
    uint32_t SecondaryConfiguredUpdateInterval = 1;
    uint32_t SecondaryEffectiveUpdateInterval = 1;
    uint64_t FixedSimulationStepIndex = 0;
    uint32_t SecondaryStepsSinceLastUpdate = 0;
    float SecondaryInterpolationPhase = 0.0f;
    float SecondaryCoarseDeltaTime = 1.0f / 60.0f;
    UINT64 PrimaryComputeFenceValue = 0;
    UINT64 SecondaryComputeFenceValue = 0;
};

class VoxelSimulationScheduler
{
public:
    VoxelSimulationSchedulerResult DispatchFrame(const VoxelSimulationSchedulerContext& context) const;

private:
    static constexpr double FixedSimulationDeltaTime = 1.0 / 60.0;
    static constexpr double MaxAccumulatedSimulationTime = 0.25;

    static uint32_t GetEffectiveUpdateInterval(VoxelExecutionMode mode, const VoxelAdapterPartition& partition);
    static bool ShouldDispatchPartition(const VoxelAdapterPartition& partition,
                                        uint64_t fixedStepIndex,
                                        uint32_t effectiveInterval);
    static float CalculateInterpolationPhase(const VoxelSimulationSchedulerContext& context,
                                             const VoxelAdapterPartition& partition,
                                             uint32_t effectiveInterval);
    static void PreparePartitionDispatch(const VoxelSimulationSchedulerContext& context,
                                         VoxelAdapterPartition& partition,
                                         uint32_t effectiveInterval,
                                         uint64_t fixedStepIndex);
    static void MarkPartitionUpdated(VoxelAdapterPartition& partition,
                                     uint64_t fixedStepIndex);
};
