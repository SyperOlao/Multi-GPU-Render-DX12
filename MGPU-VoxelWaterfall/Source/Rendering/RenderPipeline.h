#pragma once

#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"
#include "Source/Validation/VoxelVisualValidationRunner.h"
#include "Source/Voxels/VoxelSimulationScheduler.h"
#include "Source/Voxels/VoxelTypes.h"

#include <d3d12.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wrl.h>

struct FrameResource;
class VoxelGpuPartition;

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GCommandQueue;
}

struct VoxelFrameGraphTelemetry
{
    bool PrimaryComputeSubmitted = false;
    bool PrimaryBaseGraphicsSubmitted = false;
    bool SecondaryComputeSubmitted = false;
    bool SecondaryGraphicsSubmitted = false;
    uint32_t SecondaryConfiguredUpdateInterval = 1;
    uint32_t SecondaryEffectiveUpdateInterval = 1;
    uint64_t FixedSimulationStepIndex = 0;
    bool SecondarySimulationDispatchedThisFrame = false;
    uint32_t SecondaryStepsSinceLastUpdate = 0;
    float SecondaryInterpolationPhase = 0.0f;
    float SecondaryCoarseDeltaTime = 1.0f / 60.0f;
    VoxelSimulationSchedulerMode SchedulerMode = VoxelSimulationSchedulerMode::Interactive;
    uint32_t RequestedFixedSteps = 0;
    uint32_t ExecutedFixedSteps = 0;
    uint32_t DroppedSimulationSteps = 0;
    double DroppedSimulationTime = 0.0;
    uint32_t SimulationDispatchCount = 0;
    uint32_t LogicalUpdatedVoxelCount = 0;
    double WallDeltaMs = 0.0;
    double AcceptedSimulationDeltaMs = 0.0;
    uint64_t FrameResourceBackpressurePollCount = 0;
    double FrameResourceBackpressureMs = 0.0;
    uint32_t DrainedMessageCount = 0;
    uint64_t SuccessfulPresentCount = 0;
    double SimulationStepsPerWallSecond = 0.0;
    bool SecondaryRenderSubmitted = false;
    uint32_t SecondaryDrawCalls = 0;
    uint32_t SecondaryRenderedVoxelCount = 0;
    VoxelSpatialLodStats PrimarySpatialLodStats{};
    VoxelSpatialLodStats SecondarySpatialLodStats{};
    uint32_t PrimaryIndirectDrawCalls = 0;
    uint32_t SecondaryIndirectDrawCalls = 0;
    uint32_t FrameResourceIndex = 0;
    UINT64 PrimaryComputeFenceValue = 0;
    UINT64 PrimaryBaseGraphicsFenceValue = 0;
    UINT64 SecondaryComputeFenceValue = 0;
    UINT64 SecondaryGraphicsFenceValue = 0;
    UINT64 SecondaryLocalToSharedCopyFenceValue = 0;
    UINT64 CrossAdapterRenderReadyFenceValue = 0;
    UINT64 PrimarySharedToLocalCopyFenceValue = 0;
    UINT64 PrimarySecondaryImageReadyFenceValue = 0;
    UINT64 FinalPresentFenceValue = 0;
    UINT64 ColorBytesTransferred = 0;
    UINT64 DepthBytesTransferred = 0;
    UINT64 TotalCrossAdapterBytes = 0;
    UINT64 ParticleTransferBytes = 0;
    UINT64 RenderOutputTransferBytes = 0;
    UINT64 LocalToSharedColorBytes = 0;
    UINT64 LocalToSharedDepthBytes = 0;
    UINT64 SharedToLocalColorBytes = 0;
    UINT64 SharedToLocalDepthBytes = 0;
    UINT64 ExpectedLocalToSharedColorBytes = 0;
    UINT64 ExpectedLocalToSharedDepthBytes = 0;
    UINT64 ExpectedSharedToLocalColorBytes = 0;
    UINT64 ExpectedSharedToLocalDepthBytes = 0;
    uint32_t SecondaryGraphicsCommandListSubmissionCount = 0;
    uint32_t LocalToSharedCommandListSubmissionCount = 0;
    uint32_t SharedToLocalCommandListSubmissionCount = 0;
    std::string LocalToSharedColorSource;
    std::string LocalToSharedColorDestination;
    std::string LocalToSharedDepthSource;
    std::string LocalToSharedDepthDestination;
    std::string SharedToLocalColorSource;
    std::string SharedToLocalColorDestination;
    std::string SharedToLocalDepthSource;
    std::string SharedToLocalDepthDestination;
    uint32_t SecondaryGraphicsTimestampBeginQuery = 0;
    uint32_t SecondaryGraphicsTimestampEndQuery = 0;
    uint32_t LocalToSharedTimestampBeginQuery = 0;
    uint32_t LocalToSharedTimestampEndQuery = 0;
    uint32_t SharedToLocalTimestampBeginQuery = 0;
    uint32_t SharedToLocalTimestampEndQuery = 0;
    uint64_t SecondaryPipelineIAPrimitives = 0;
    uint64_t SecondaryPipelineVSInvocations = 0;
    uint64_t SecondaryPipelinePSInvocations = 0;
    uint64_t SecondaryPipelineCInvocations = 0;
    uint64_t SecondaryPipelineCPrimitives = 0;
    uint32_t SecondaryIndirectArgumentMaxCommandCount = 0;
    uint32_t SecondaryIndirectArgumentResolvedDrawCount = 0;
    bool CompositeSubmitted = false;
    bool CompositeUsedSecondaryImage = false;
    bool VisualValidationHasResult = false;
    bool VisualValidationPassed = false;
    std::string VisualValidationRunId;
    uint64_t VisualValidationSnapshotHash = 0;
    double VisualValidationColorMAE = 0.0;
    double VisualValidationColorRMSE = 0.0;
    double VisualValidationPSNR = 0.0;
    double VisualValidationMaxError = 0.0;
    double VisualValidationMismatchedPixelPercent = 0.0;
    double VisualValidationDepthRMSE = 0.0;
    double VisualValidationDepthMismatchPercent = 0.0;
    uint64_t VisualValidationPipelinePrimitiveCount = 0;
    std::string VisualValidationFailReason;
    VoxelCompositeDebugView CompositeDebugView = VoxelCompositeDebugView::FinalComposite;
    VoxelExecutionMode RequestedMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ActualMode = VoxelExecutionMode::SingleGpuFull;
};

std::optional<D3D12_QUERY_DATA_PIPELINE_STATISTICS> ReadCompletedSecondaryPipelineStatistics(
    const MultiGpuVoxelFrameRenderTargets& targets);

struct PrimaryBasePassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> RenderQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimaryComputeFence;
    UINT64 PrimaryComputeFenceValue = 0;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    UINT64& GraphicsPassFenceValue;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
    const VoxelRenderWorkload* VoxelWorkload = nullptr;
    std::vector<VoxelPartitionRenderResult>* PrimaryVoxelRenderResults = nullptr;
    std::function<void(const std::shared_ptr<PEPEngine::Graphics::GCommandList>&)> RecordPrimaryBaseCommands;
};

struct SecondaryVoxelGraphicsPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> SecondaryGraphicsQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> SecondaryComputeFence;
    UINT64 SecondaryComputeFenceValue = 0;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    std::vector<const VoxelAdapterPartition*> SecondaryPartitions;
    MultiGpuVoxelFrameRenderTargets& RenderTargets;
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
    std::vector<VoxelPartitionRenderResult>* SecondaryVoxelRenderResults = nullptr;
};

struct SecondaryLocalToSharedCopyPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> SecondaryCopyQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> SecondaryRenderFence;
    UINT64 SecondaryRenderFenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> CrossAdapterRenderReadyFence;
    UINT64& CrossAdapterRenderReadyFenceValue;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    MultiGpuVoxelFrameRenderTargets& RenderTargets;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
};

struct PrimarySharedToLocalCopyPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> PrimaryCopyQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> CrossAdapterRenderReadyFence;
    UINT64 CrossAdapterRenderReadyFenceValue = 0;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    MultiGpuVoxelFrameRenderTargets& RenderTargets;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
};

struct FinalCompositeAndPresentPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> RenderQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimaryBaseRenderFence;
    UINT64 PrimaryBaseRenderFenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimarySecondaryImageReadyFence;
    UINT64 PrimarySecondaryImageReadyFenceValue = 0;
    bool WaitForSecondaryImage = false;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    UINT64& GraphicsPassFenceValue;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
    std::function<void(const std::shared_ptr<PEPEngine::Graphics::GCommandList>&)> RecordFinalPresentCommands;
};

class RenderPipeline
{
public:
    void SubmitPrimaryBasePass(const PrimaryBasePassContext& context) const;
    void SubmitSecondaryVoxelPass(const SecondaryVoxelGraphicsPassContext& context) const;
    void SubmitSecondaryLocalToSharedCopyPass(const SecondaryLocalToSharedCopyPassContext& context) const;
    void SubmitPrimarySharedToLocalCopyPass(const PrimarySharedToLocalCopyPassContext& context) const;
    void SubmitFinalCompositeAndPresentPass(const FinalCompositeAndPresentPassContext& context) const;
};
