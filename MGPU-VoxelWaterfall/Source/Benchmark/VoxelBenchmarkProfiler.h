#pragma once

#include "GCommandList.h"
#include "GCommandQueue.h"
#include "GDevice.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class VoxelBenchmarkProfiler
{
public:
    enum class QueueId : uint8_t
    {
        PrimaryCompute = 0,
        PrimaryGraphics,
        SecondaryCompute,
        SecondaryGraphics,
        SecondaryCopy,
        PrimaryCopy,
        Count
    };

    enum class RangeId : uint8_t
    {
        PrimaryCompute = 0,
        PrimaryLodCompaction,
        PrimaryBaseGraphics,
        SecondaryCompute,
        SecondaryLodCompaction,
        SecondaryGraphics,
        SecondaryLocalToSharedCopy,
        PrimarySharedToLocalCopy,
        Composite,
        FinalResolveUi,
        NoLodFastPath,
        Count
    };

    struct FrameMetadata
    {
        uint64_t FrameIndex = 0;
        std::string ProfileName;
        std::string ScenePreset;
        std::string RequestedMode;
        std::string ActualMode;
        std::string TemporalPolicy;
        std::string SpatialLodPolicy;
        std::string PartitionStrategy;
        std::string LoadBalanceScenario;
        std::string CameraPath;
        std::string LightingPreset;
        std::string RenderResolutionPreset;
        float CameraFovDegrees = 0.0f;
        float CameraNearPlane = 0.0f;
        float CameraFarPlane = 0.0f;
        bool DynamicShadowsEnabled = false;
        std::string BenchmarkConfigClass;
        std::string BenchmarkConfigReason;
        std::string FallbackReason;
        uint32_t PrimaryPartitionVoxelCount = 0;
        uint32_t SecondaryPartitionVoxelCount = 0;
        uint32_t TotalVoxelCount = 0;
        uint32_t ActualStaticVoxelCount = 0;
        uint32_t ActualDynamicVoxelCount = 0;
        uint32_t StaticVoxelBudget = 0;
        uint32_t DynamicVoxelBudget = 0;
        float VoxelSize = 0.0f;
        uint32_t ChunkSizeX = 0;
        uint32_t ChunkSizeY = 0;
        uint32_t ChunkSizeZ = 0;
        uint32_t UpdatedVoxelCount = 0;
        uint32_t SimulationStepsThisFrame = 0;
        uint32_t SimulationDispatchCount = 0;
        std::string SchedulerMode;
        uint32_t RequestedFixedSteps = 0;
        uint32_t ExecutedFixedSteps = 0;
        uint32_t DroppedSteps = 0;
        double DroppedSimulationTime = 0.0;
        uint32_t LogicalUpdatedVoxelCount = 0;
        double WallDeltaMs = 0.0;
        double AcceptedSimulationDeltaMs = 0.0;
        uint64_t FrameResourceBackpressurePollCount = 0;
        uint32_t DrainedMessageCount = 0;
        uint64_t SuccessfulPresentCount = 0;
        double SimulationStepsPerWallSecond = 0.0;
        uint32_t Seed = 0;
        float SecondaryShare = 0.0f;
        uint32_t TemporalDecimationInterval = 1;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        std::string ResolvedConfigHash;
        std::wstring PrimaryAdapterName;
        std::wstring SecondaryAdapterName;
        uint32_t PrimaryVendorId = 0;
        uint32_t PrimaryDeviceId = 0;
        uint64_t PrimaryDedicatedVideoMemory = 0;
        std::string PrimaryAdapterLuid;
        uint32_t SecondaryVendorId = 0;
        uint32_t SecondaryDeviceId = 0;
        uint64_t SecondaryDedicatedVideoMemory = 0;
        std::string SecondaryAdapterLuid;
        std::string OperatingSystem;
        std::string BuildConfiguration;
        std::string GitCommit;
        std::string GitDirtyState;
        bool D3D12DebugLayerEnabled = false;
        double CpuWaitMs = 0.0;
        double PresentToPresentMs = 0.0;
        uint64_t TotalCrossAdapterBytes = 0;
        uint64_t ColorTransferBytes = 0;
        uint64_t DepthTransferBytes = 0;
        uint64_t ParticleTransferBytes = 0;
        uint64_t RenderOutputTransferBytes = 0;
        uint32_t SecondaryDrawCalls = 0;
        uint32_t PrimaryRenderedVoxelCount = 0;
        uint32_t SecondaryRenderedVoxelCount = 0;
        uint32_t PrimarySubmittedVoxelCount = 0;
        uint32_t SecondarySubmittedVoxelCount = 0;
        uint32_t PrimaryLod0Count = 0;
        uint32_t PrimaryLod1Count = 0;
        uint32_t PrimaryLod2Count = 0;
        uint32_t SecondaryLod0Count = 0;
        uint32_t SecondaryLod1Count = 0;
        uint32_t SecondaryLod2Count = 0;
        bool NoLodFastPath = false;
        uint32_t LodHashCapacity = 0;
        float LodHashLoadFactor = 0.0f;
        uint32_t LodDuplicateCount = 0;
        uint32_t LodProbeOverflowCount = 0;
        uint32_t LodMaxProbeCount = 0;
        float LodAverageProbeCount = 0.0f;
        bool VisualValidationHasResult = false;
        bool VisualValidationPassed = false;
        std::string VisualValidationRunId;
        std::string VisualValidationCaseId;
        std::string VisualValidationProtocolHash;
        std::string VisualValidationConfigHash;
        std::string VisualValidationCameraHash;
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
        std::chrono::steady_clock::time_point CpuFrameStart{};
    };

    struct BenchmarkSummary
    {
        std::string RequestedMode;
        std::string ActualMode;
        std::string Preset;
        std::string ProfileName;
        std::string PartitionStrategy;
        std::string LoadBalanceScenario;
        std::string BenchmarkConfigClass;
        std::string TemporalPolicy;
        std::string SpatialLodPolicy;
        std::string PairId;
        std::string SessionId;
        std::string BlockId;
        std::string SkipReason;
        std::wstring PrimaryAdapterName;
        std::wstring SecondaryAdapterName;
        uint32_t RequestedLabelCount = 0;
        uint32_t RequestedStaticBudget = 0;
        uint32_t RequestedDynamicBudget = 0;
        uint32_t TotalVoxelCount = 0;
        uint32_t ActualStaticVoxelCount = 0;
        uint32_t ActualDynamicVoxelCount = 0;
        std::string ResolvedConfigHash;
        float SecondaryShare = 0.0f;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        uint32_t Repetition = 0;
        uint32_t RepetitionCount = 1;
        uint32_t MeasuredFrameCount = 0;
        uint32_t ValidFrameCount = 0;
        uint32_t InvalidFrameCount = 0;
        bool Valid = true;
        std::string ValidityReason;
        std::string SpeedupStatistic = "mean_present_to_present_ms";
        double AveragePresentToPresentMs = 0.0;
        double MedianPresentToPresentMs = 0.0;
        double P95PresentToPresentMs = 0.0;
        double P99PresentToPresentMs = 0.0;
        double StdDevPresentToPresentMs = 0.0;
        double PresentToPresentCi95HalfWidthMs = 0.0;
        double AverageCpuSubmissionMs = 0.0;
        double MedianCpuSubmissionMs = 0.0;
        double P95CpuSubmissionMs = 0.0;
        double P99CpuSubmissionMs = 0.0;
        double StdDevCpuSubmissionMs = 0.0;
        double AverageCpuTotalFrameMs = 0.0;
        double MedianCpuTotalFrameMs = 0.0;
        double StdDevCpuTotalFrameMs = 0.0;
        double AverageCpuFrameMs = 0.0;
        double MedianCpuFrameMs = 0.0;
        double P95CpuFrameMs = 0.0;
        double P99CpuFrameMs = 0.0;
        double StdDevCpuFrameMs = 0.0;
        double CpuFrameCi95HalfWidthMs = 0.0;
        double CriticalPathGpuMs = 0.0;
        double GpuWorkSumMs = 0.0;
        double PrimaryComputeMs = 0.0;
        double PrimaryLodCompactionMs = 0.0;
        double PrimaryGraphicsMs = 0.0;
        double SecondaryComputeMs = 0.0;
        double SecondaryLodCompactionMs = 0.0;
        double SecondaryGraphicsMs = 0.0;
        double TransferMs = 0.0;
        double CompositeMs = 0.0;
        uint64_t AverageTransferBytes = 0;
        uint64_t AverageColorTransferBytes = 0;
        uint64_t AverageDepthTransferBytes = 0;
        uint64_t AverageParticleTransferBytes = 0;
        uint64_t AverageRenderOutputTransferBytes = 0;
        double AverageSecondaryDrawCalls = 0.0;
        double AveragePrimarySubmittedVoxelCount = 0.0;
        double AverageSecondarySubmittedVoxelCount = 0.0;
        double AveragePrimaryLod0Count = 0.0;
        double AveragePrimaryLod1Count = 0.0;
        double AveragePrimaryLod2Count = 0.0;
        double AverageSecondaryLod0Count = 0.0;
        double AverageSecondaryLod1Count = 0.0;
        double AverageSecondaryLod2Count = 0.0;
        uint64_t TotalExecutedFixedSteps = 0;
        uint64_t TotalLogicalUpdatedVoxelCount = 0;
        double SpeedupVsMatchingSingleGpu = 0.0;
        double Efficiency = 0.0;
        bool VisualValidationPassed = false;
        std::string VisualValidationCaseId;
        std::string VisualValidationProtocolHash;
        std::string VisualValidationConfigHash;
        std::string VisualValidationCameraHash;
        std::filesystem::path CsvPath;
    };

    struct TimingSnapshot
    {
        bool Valid = false;
        double PrimaryComputeMs = 0.0;
        double PrimaryLodCompactionMs = 0.0;
        double PrimaryGraphicsMs = 0.0;
        double SecondaryComputeMs = 0.0;
        double SecondaryLodCompactionMs = 0.0;
        double SecondaryGraphicsMs = 0.0;
        double TransferMs = 0.0;
        double CompositeMs = 0.0;
        double FinalResolveUiMs = 0.0;
        double CriticalPathGpuMs = 0.0;
        double GpuWorkSumMs = 0.0;
        uint64_t TransferBytes = 0;
        uint64_t ParticleTransferBytes = 0;
        uint32_t SecondaryDrawCalls = 0;
        bool VisualValidationPassed = false;
    };

    void Initialize(const std::shared_ptr<PEPEngine::Graphics::GDevice>& primaryDevice,
                    const std::shared_ptr<PEPEngine::Graphics::GDevice>& secondaryDevice,
                    const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& primaryComputeQueue,
                    const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& primaryGraphicsQueue,
                    const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& secondaryComputeQueue,
                    const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& secondaryGraphicsQueue,
                    const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& secondaryCopyQueue,
                    const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& primaryCopyQueue);

    bool IsInitialized() const { return initialized; }
    bool IsActive() const { return active; }
    bool IsComplete() const { return rowsWritten >= recordedFrameCount; }
    float GetProgress() const;
    uint32_t GetWarmupFramesSeen() const { return std::min(framesSeen, warmupFrameCount); }
    uint32_t GetRowsWritten() const { return rowsWritten; }
    uint32_t GetWarmupFrameCount() const { return warmupFrameCount; }
    uint32_t GetRecordedFrameCount() const { return recordedFrameCount; }
    const std::filesystem::path& GetCsvPath() const { return csvPath; }

    bool Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata);
    bool Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata,
               const std::string& fileName, const std::string& presetName, uint32_t repetition,
               uint32_t warmupFrames = DefaultWarmupFrameCount,
               uint32_t measuredFrames = DefaultRecordedFrameCount,
               const std::string& suiteName = "",
               const std::string& runId = "",
               const std::string& configId = "",
               const std::string& pairId = "",
               const std::string& sessionId = "",
               const std::string& blockId = "",
               uint32_t orderIndex = 0,
               uint32_t blockOrderIndex = 0,
               uint32_t pairMemberOrder = 0,
               uint32_t randomizationSeed = 0);
    void Stop();
    bool HasCompletedSummary() const { return completedSummaryReady; }
    BenchmarkSummary ConsumeCompletedSummary();
    const TimingSnapshot& GetLatestTimingSnapshot() const { return latestTimingSnapshot; }

    void BeginFrame(const FrameMetadata& metadata);
    void UpdateCurrentFrameMetadata(const FrameMetadata& metadata);
    void EndFrameCpu();
    void ProcessCompletedFrames();

    void BeginRange(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList, QueueId queue, RangeId range);
    void EndRange(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList, QueueId queue, RangeId range);
    void ResolveRange(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList, QueueId queue, RangeId range);
    void SetQueueFence(QueueId queue, uint64_t fenceValue);

    static constexpr uint32_t DefaultWarmupFrameCount = 100;
    static constexpr uint32_t DefaultRecordedFrameCount = 500;
    static constexpr uint32_t WarmupFrameCount = DefaultWarmupFrameCount;
    static constexpr uint32_t RecordedFrameCount = DefaultRecordedFrameCount;

private:
    static constexpr uint32_t RingFrameCount = 32;
    static constexpr uint32_t QueueCount = static_cast<uint32_t>(QueueId::Count);
    static constexpr uint32_t RangeCount = static_cast<uint32_t>(RangeId::Count);
    static constexpr uint32_t QueryCountPerFrame = RangeCount * 2;
    static constexpr uint32_t QueryCountPerQueue = RingFrameCount * QueryCountPerFrame;

    struct QueueContext
    {
        std::shared_ptr<PEPEngine::Graphics::GDevice> Device;
        std::shared_ptr<PEPEngine::Graphics::GCommandQueue> Queue;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> QueryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer;
        uint64_t Frequency = 1;
        uint64_t CalibrationGpuTimestamp = 0;
        uint64_t CalibrationCpuQpc = 0;
        HRESULT CalibrationHResult = E_FAIL;
        bool CalibrationValid = false;
        bool CalibrationMonotonic = false;
        bool Valid = false;
    };

    struct RangeRecord
    {
        bool Active = false;
        QueueId Queue = QueueId::PrimaryCompute;
    };

    struct RangeTiming
    {
        double Ms = 0.0;
        double StartQpc = 0.0;
        double EndQpc = 0.0;
    };

    struct FrameRecord
    {
        bool Active = false;
        bool CsvEligible = false;
        bool Written = false;
        uint32_t Slot = 0;
        FrameMetadata Metadata{};
        std::array<RangeRecord, RangeCount> Ranges{};
        std::array<uint64_t, QueueCount> FenceValues{};
        double CpuSubmissionMs = 0.0;
    };

    std::array<QueueContext, QueueCount> queues{};
    std::array<FrameRecord, RingFrameCount> frames{};
    FrameRecord* currentFrame = nullptr;
    LARGE_INTEGER qpcFrequency{};
    bool initialized = false;
    bool active = false;
    uint32_t framesSeen = 0;
    uint32_t rowsWritten = 0;
    uint32_t warmupFrameCount = DefaultWarmupFrameCount;
    uint32_t recordedFrameCount = DefaultRecordedFrameCount;
    uint32_t currentRepetition = 0;
    uint32_t currentOrderIndex = 0;
    uint32_t currentBlockOrderIndex = 0;
    uint32_t currentPairMemberOrder = 0;
    uint32_t currentRandomizationSeed = 0;
    std::ofstream csv;
    std::filesystem::path csvPath;
    std::string currentPresetName;
    std::string currentSuiteName;
    std::string currentRunId;
    std::string currentConfigId;
    std::string currentPairId;
    std::string currentSessionId;
    std::string currentBlockId;
    BenchmarkSummary completedSummary{};
    TimingSnapshot latestTimingSnapshot{};
    bool completedSummaryReady = false;

    std::vector<double> presentToPresentMsSamples;
    std::vector<double> cpuSubmissionMsSamples;
    std::vector<double> cpuTotalFrameMsSamples;
    std::vector<double> criticalPathGpuMsSamples;
    std::vector<double> gpuWorkSumMsSamples;
    std::vector<double> primaryComputeMsSamples;
    std::vector<double> primaryLodCompactionMsSamples;
    std::vector<double> primaryGraphicsMsSamples;
    std::vector<double> secondaryComputeMsSamples;
    std::vector<double> secondaryLodCompactionMsSamples;
    std::vector<double> secondaryGraphicsMsSamples;
    std::vector<double> transferMsSamples;
    std::vector<double> compositeMsSamples;
    std::vector<uint64_t> transferBytesSamples;
    std::vector<uint64_t> colorTransferBytesSamples;
    std::vector<uint64_t> depthTransferBytesSamples;
    std::vector<uint64_t> particleTransferBytesSamples;
    std::vector<uint64_t> renderOutputTransferBytesSamples;
    std::vector<double> secondaryDrawCallSamples;
    std::vector<double> primarySubmittedVoxelSamples;
    std::vector<double> secondarySubmittedVoxelSamples;
    std::vector<double> primaryLod0Samples;
    std::vector<double> primaryLod1Samples;
    std::vector<double> primaryLod2Samples;
    std::vector<double> secondaryLod0Samples;
    std::vector<double> secondaryLod1Samples;
    std::vector<double> secondaryLod2Samples;
    std::vector<uint64_t> executedFixedStepSamples;
    std::vector<uint64_t> logicalUpdatedVoxelSamples;
    std::vector<std::string> invalidReasons;

    static uint32_t ToIndex(QueueId id) { return static_cast<uint32_t>(id); }
    static uint32_t ToIndex(RangeId id) { return static_cast<uint32_t>(id); }
    static std::string ToUtf8(const std::wstring& value);
    static std::string EscapeCsv(const std::string& value);
    static std::string EscapeCsv(const std::wstring& value);
    static std::string SanitizeFileToken(const std::string& value);

    uint32_t QueryIndex(uint32_t slot, RangeId range, bool endQuery) const;
    uint64_t QueryOffset(uint32_t slot, RangeId range) const;
    bool IsQueueComplete(const QueueContext& queue, uint64_t fenceValue) const;
    bool IsFrameReady(const FrameRecord& frame) const;
    RangeTiming ReadRangeTiming(const FrameRecord& frame, RangeId range) const;
    void WriteFrame(const FrameRecord& frame);
    std::string ValidateFrameRecord(const FrameRecord& frame, bool timestampsValid) const;
    void CreateQueueResources(QueueContext& context);
    void CalibrateQueues();
    void ResetSamples();
    void FinalizeCompletedSummary();
    static double Average(const std::vector<double>& values);
    static double AverageUint64(const std::vector<uint64_t>& values);
    static double StdDev(const std::vector<double>& values);
    static double Percentile(std::vector<double> values, double percentile);
};
