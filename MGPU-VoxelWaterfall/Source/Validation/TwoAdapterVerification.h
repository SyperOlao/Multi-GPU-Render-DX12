#pragma once

#include "Source/Devices/DeviceSelectionPolicy.h"
#include "Source/Rendering/RenderPipeline.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace PEPEngine::Graphics
{
    class GDevice;
}

enum class TwoAdapterVerificationStatus
{
    Pass,
    PassHardwareDirect,
    PassHardwareCopyOnly,
    Fail,
    Blocked
};

struct TwoAdapterQueueCalibration
{
    std::string QueueName;
    std::string HResult;
    uint64_t Frequency = 0;
    uint64_t GpuTimestamp = 0;
    uint64_t CpuTimestamp = 0;
    bool Valid = false;
};

struct TwoAdapterRuntimeEvidence
{
    bool Attempted = false;
    uint32_t FramesObserved = 0;
    std::string FrameConfigId;
    std::string ProtocolHash;
    std::string ConfigHash;
    std::string BuildHash;
    std::string ShaderHash;
    std::string PrimaryDriverVersion;
    std::string SecondaryDriverVersion;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    std::string ColorFormat = "R8G8B8A8_UNORM";
    std::string DepthFormat = "R32_FLOAT";
    VoxelExecutionMode RequestedMode = VoxelExecutionMode::MultiGpuFull;
    VoxelExecutionMode ActualMode = VoxelExecutionMode::SingleGpuFull;
    CrossAdapterTransferMode TransferMode = CrossAdapterTransferMode::Unavailable;
    bool Fallback = true;
    std::string FallbackReason;
    bool AnyActualMultiMode = false;
    uint32_t PrimaryPartitionVoxels = 0;
    uint32_t SecondaryPartitionVoxels = 0;
    uint32_t TotalSecondaryComputeDispatchCount = 0;
    uint32_t MaxSecondaryGraphicsDrawCount = 0;
    uint32_t MaxSecondaryRenderedVoxelCount = 0;
    uint64_t TotalColorTransferBytes = 0;
    uint64_t TotalDepthTransferBytes = 0;
    bool AnyCompositeSubmitted = false;
    bool AnySecondaryComputeFenceValue = false;
    bool AnySecondaryGraphicsFenceValue = false;
    bool AnySecondaryLocalToSharedCopyFenceValue = false;
    bool AnyCrossAdapterRenderReadyFenceValue = false;
    bool AnyPrimarySharedToLocalCopyFenceValue = false;
    bool AnyPrimarySecondaryImageReadyFenceValue = false;
    bool AnyFinalPresentFenceValue = false;
    uint32_t SecondaryComputeDispatchCount = 0;
    uint32_t SecondaryGraphicsDrawCount = 0;
    uint32_t SecondaryIndirectDrawCount = 0;
    uint32_t SecondaryRenderedVoxelCount = 0;
    uint64_t SecondaryPrimitiveEstimate = 0;
    uint64_t SecondaryPipelineIAPrimitives = 0;
    uint64_t SecondaryPipelineVSInvocations = 0;
    uint64_t SecondaryPipelinePSInvocations = 0;
    uint64_t SecondaryPipelineCInvocations = 0;
    uint64_t SecondaryPipelineCPrimitives = 0;
    uint32_t SecondaryGraphicsCommandListSubmissionCount = 0;
    uint32_t LocalToSharedCommandListSubmissionCount = 0;
    uint32_t SharedToLocalCommandListSubmissionCount = 0;
    uint32_t SecondaryIndirectArgumentMaxCommandCount = 0;
    uint32_t SecondaryIndirectArgumentResolvedDrawCount = 0;
    uint64_t ExpectedColorLocalToSharedBytes = 0;
    uint64_t ExpectedDepthLocalToSharedBytes = 0;
    uint64_t ExpectedColorSharedToLocalBytes = 0;
    uint64_t ExpectedDepthSharedToLocalBytes = 0;
    uint64_t ColorLocalToSharedBytes = 0;
    uint64_t DepthLocalToSharedBytes = 0;
    uint64_t ColorSharedToLocalBytes = 0;
    uint64_t DepthSharedToLocalBytes = 0;
    std::string ColorLocalToSharedSource;
    std::string ColorLocalToSharedDestination;
    std::string DepthLocalToSharedSource;
    std::string DepthLocalToSharedDestination;
    std::string ColorSharedToLocalSource;
    std::string ColorSharedToLocalDestination;
    std::string DepthSharedToLocalSource;
    std::string DepthSharedToLocalDestination;
    std::string CopyOperation = "CopyResource";
    std::string LocalToSharedPath = "texture_to_shared_texture";
    std::string SharedToLocalPath = "shared_texture_to_texture";
    std::string BridgeResourceDimension = "TEXTURE2D";
    uint64_t BridgeColorBytes = 0;
    uint64_t BridgeDepthBytes = 0;
    uint64_t BridgeColorRowPitch = 0;
    uint64_t BridgeDepthRowPitch = 0;
    uint32_t SecondaryGraphicsTimestampBeginQuery = 0;
    uint32_t SecondaryGraphicsTimestampEndQuery = 0;
    uint32_t LocalToSharedTimestampBeginQuery = 0;
    uint32_t LocalToSharedTimestampEndQuery = 0;
    uint32_t SharedToLocalTimestampBeginQuery = 0;
    uint32_t SharedToLocalTimestampEndQuery = 0;
    uint64_t ParticleTransferBytes = 0;
    uint64_t SecondaryComputeFenceValue = 0;
    uint64_t SecondaryGraphicsFenceValue = 0;
    uint64_t SecondaryLocalToSharedCopyFenceValue = 0;
    uint64_t CrossAdapterRenderReadyFenceValue = 0;
    uint64_t PrimarySharedToLocalCopyFenceValue = 0;
    uint64_t PrimarySecondaryImageReadyFenceValue = 0;
    uint64_t FinalPresentFenceValue = 0;
    bool CompositeSubmitted = false;
    bool Passed = false;
    std::vector<std::string> Reasons;
    std::vector<TwoAdapterQueueCalibration> QueueCalibrations;
};

struct TwoAdapterVerificationResult
{
    std::string VerificationRunId;
    std::string BuildHash;
    std::string AdapterPairIdentity;
    TwoAdapterVerificationStatus Status = TwoAdapterVerificationStatus::Blocked;
    std::vector<std::string> Reasons;
    std::filesystem::path JsonPath;
    std::filesystem::path TextPath;
    TwoAdapterRuntimeEvidence Runtime;
    std::string PreflightJsonFragment;
    std::string PreflightText;
};

class TwoAdapterVerificationRunner
{
public:
    static std::string StatusName(TwoAdapterVerificationStatus status);

    TwoAdapterVerificationResult RunPreflight(
        const std::vector<std::shared_ptr<PEPEngine::Graphics::GDevice>>& devices,
        const SelectedDevices& selectedDevices,
        const std::string& buildHash,
        const std::filesystem::path& outputDirectory) const;

    void ExportRuntimeEvidence(TwoAdapterVerificationResult& result,
                               const TwoAdapterRuntimeEvidence& runtime) const;
};
