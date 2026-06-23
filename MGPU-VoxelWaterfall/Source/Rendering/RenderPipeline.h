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
    double SecondaryGpuCpuWaitMs = 0.0;
    double CopyCpuWaitMs = 0.0;
    double PresentWaitMs = 0.0;
    double FrameResourceAcquireCpuMs = 0.0;
    double UpdateCpuMs = 0.0;
    double PrimaryRenderCpuMs = 0.0;
    double SecondaryRenderCpuMs = 0.0;
    double CrossAdapterCopyCpuMs = 0.0;
    double CompositeCpuMs = 0.0;
    double FinalResolveCpuMs = 0.0;
    double ImGuiCpuMs = 0.0;
    double MetricsCollectionCpuMs = 0.0;
    double CsvWriteCpuMs = 0.0;
    uint32_t DrainedMessageCount = 0;
    uint32_t CurrentFramePumpDepth = 0;
    uint32_t MaximumObservedFramePumpDepth = 0;
    uint64_t RejectedRecursiveFrameRequests = 0;
    uint32_t ActualClientWidth = 0;
    uint32_t ActualClientHeight = 0;
    uint32_t SwapchainWidth = 0;
    uint32_t SwapchainHeight = 0;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    uint32_t SsaaWidth = 0;
    uint32_t SsaaHeight = 0;
    uint32_t SsaaSampleMultiplier = 1;
    uint32_t SsaaLinearScale = 1;
    uint32_t TransferWidth = 0;
    uint32_t TransferHeight = 0;
    uint64_t EstimatedOffscreenMemoryBytes = 0;
    uint64_t EstimatedCrossAdapterBytesPerFrame = 0;
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
    int32_t SecondaryRectX = 0;
    int32_t SecondaryRectY = 0;
    uint32_t SecondaryRectWidth = 0;
    uint32_t SecondaryRectHeight = 0;
    double SecondaryRectAreaPercent = 0.0;
    UINT64 FullFrameTransferBytes = 0;
    UINT64 ActualTransferBytes = 0;
    double SavedTransferPercent = 0.0;
    bool SecondaryRectEmpty = false;
    bool SecondaryRectFallbackFull = false;
    uint32_t AdaptiveCalibrationFrameCount = 0;
    uint32_t AdaptiveCalibrationWarmupFrames = 120;
    double AdaptivePrimaryThroughputVoxelsPerMs = 0.0;
    double AdaptiveSecondaryThroughputVoxelsPerMs = 0.0;
    double AdaptiveTransferBandwidthBytesPerMs = 0.0;
    double AdaptiveCompositeOverheadMs = 0.0;
    double AdaptiveExpectedGainMs = 0.0;
    float AdaptiveRecommendedSecondaryShare = 0.0f;
    bool AdaptiveCalibrationComplete = false;
    bool AdaptiveFallbackRecommended = false;
    std::string AdaptiveReason;
    std::string AdaptiveWarning;
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
    std::string CopyOperation = "CopyResource";
    std::string LocalToSharedPath = "texture_to_shared_texture";
    std::string SharedToLocalPath = "shared_texture_to_texture";
    std::string BridgeResourceDimension = "TEXTURE2D";
    UINT64 BridgeColorBytes = 0;
    UINT64 BridgeDepthBytes = 0;
    UINT64 BridgeColorRowPitch = 0;
    UINT64 BridgeDepthRowPitch = 0;
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
    FinalResolveSource FinalResolveSourceMode = FinalResolveSource::PrimaryBase;
    std::string FinalResolveSourceName = "PrimaryBase";
    uint64_t FinalResolveSourceResourcePointer = 0;
    uint64_t FinalResolveSourceGeneration = 0;
    uint32_t FinalResolveSourceWidth = 0;
    uint32_t FinalResolveSourceHeight = 0;
    int FinalResolveSourceFormat = 0;
    uint64_t FinalResolveSourceDescriptorGpuHandle = 0;
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
    const VoxelFrameRenderPlan* VoxelRenderPlan = nullptr;
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
    const std::vector<VoxelFramePartitionRenderPlan>* SecondaryPartitions = nullptr;
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
    D3D12_RECT DirtyRect{};
    bool UseDirtyRect = false;
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
    D3D12_RECT DirtyRect{};
    bool UseDirtyRect = false;
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
    void ValidateCopyOnlyFrameStateBeforePrimaryRender(const MultiGpuVoxelFrameRenderTargets& targets) const;
    void SubmitPrimaryBasePass(const PrimaryBasePassContext& context) const;
    void SubmitSecondaryVoxelPass(const SecondaryVoxelGraphicsPassContext& context) const;
    void SubmitSecondaryLocalToSharedCopyPass(const SecondaryLocalToSharedCopyPassContext& context) const;
    void SubmitPrimarySharedToLocalCopyPass(const PrimarySharedToLocalCopyPassContext& context) const;
    void SubmitFinalCompositeAndPresentPass(const FinalCompositeAndPresentPassContext& context) const;
};
