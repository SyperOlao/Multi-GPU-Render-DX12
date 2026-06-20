#pragma once

#include "GCommandQueue.h"
#include "VoxelBenchmarkProfiler.h"
#include "Source/Voxels/VoxelTypes.h"

#include <wrl.h>

struct VoxelSimulationSchedulerContext
{
    VoxelLodArray& Lods;
    VoxelExecutionMode ExecutionMode = VoxelExecutionMode::PrimaryOnly;
    bool SplitMultiGpuAvailable = false;
    uint64_t SimulationFrameIndex = 0;
    uint32_t TimestampHeapIndex = 0;

    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> PrimaryComputeQueue;
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> SecondaryComputeQueue;
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> CrossAdapterCopyQueue;
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> RenderQueue;

    Microsoft::WRL::ComPtr<ID3D12Fence> PrimeComputeFence;
    Microsoft::WRL::ComPtr<ID3D12Fence> SecondComputeFence;
    Microsoft::WRL::ComPtr<ID3D12Fence> SecondRenderFence;

    UINT64 SecondRenderFenceValue = 0;
    UINT64& PrimaryComputeFenceValue;
    UINT64& SecondaryComputeFenceValue;
    UINT64& SharedComputeFenceValue;
    UINT64& CrossAdapterDataReadyFenceValue;
    UINT64& CurrentFrameComputeFenceValue;

    VoxelBenchmarkProfiler& BenchmarkProfiler;
};

struct VoxelSimulationSchedulerResult
{
    bool UsedSplitMultiGpu = false;
    bool SecondaryWorkThisFrame = false;
};

class VoxelSimulationScheduler
{
public:
    VoxelSimulationSchedulerResult DispatchFrame(const VoxelSimulationSchedulerContext& context) const;

private:
    static constexpr float FixedSimulationDeltaTime = 1.0f / 60.0f;
    static constexpr float MaxSimulationDeltaTime = 1.0f / 15.0f;

    static uint32_t EffectiveInterval(const VoxelSimulationSchedulerContext& context, size_t lodIndex);
    static bool ShouldUpdateLod(const VoxelSimulationSchedulerContext& context, size_t lodIndex);
    static void PrepareLodDispatch(const VoxelSimulationSchedulerContext& context, size_t lodIndex);
    static void MarkLodUpdated(const VoxelSimulationSchedulerContext& context, size_t lodIndex);
};
