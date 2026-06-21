#pragma once

#include "GCommandQueue.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Voxels/VoxelTypes.h"

struct VoxelSimulationSchedulerContext
{
    VoxelWaterfallWorkload& Workload;
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

    static void PreparePartitionDispatch(const VoxelSimulationSchedulerContext& context,
                                         VoxelPartitionState& partition);
    static void MarkPartitionUpdated(const VoxelSimulationSchedulerContext& context,
                                     VoxelPartitionState& partition);
};
