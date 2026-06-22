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
    std::string FrameConfigId;
    VoxelExecutionMode RequestedMode = VoxelExecutionMode::MultiGpuFull;
    VoxelExecutionMode ActualMode = VoxelExecutionMode::SingleGpuFull;
    bool Fallback = true;
    std::string FallbackReason;
    uint32_t PrimaryPartitionVoxels = 0;
    uint32_t SecondaryPartitionVoxels = 0;
    uint32_t SecondaryComputeDispatchCount = 0;
    uint32_t SecondaryGraphicsDrawCount = 0;
    uint32_t SecondaryIndirectDrawCount = 0;
    uint32_t SecondaryRenderedVoxelCount = 0;
    uint64_t SecondaryPrimitiveEstimate = 0;
    uint64_t ColorLocalToSharedBytes = 0;
    uint64_t DepthLocalToSharedBytes = 0;
    uint64_t ColorSharedToLocalBytes = 0;
    uint64_t DepthSharedToLocalBytes = 0;
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
