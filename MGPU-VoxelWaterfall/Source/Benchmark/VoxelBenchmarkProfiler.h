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
        PrimaryBaseGraphics,
        SecondaryCompute,
        SecondaryGraphics,
        SecondaryLocalToSharedCopy,
        PrimarySharedToLocalCopy,
        Composite,
        FinalResolveUi,
        Count
    };

    struct FrameMetadata
    {
        uint64_t FrameIndex = 0;
        std::string RequestedMode;
        std::string ActualMode;
        std::string TemporalPolicy;
        std::string FallbackReason;
        uint32_t PrimaryPartitionVoxelCount = 0;
        uint32_t SecondaryPartitionVoxelCount = 0;
        uint32_t TotalVoxelCount = 0;
        uint32_t UpdatedVoxelCount = 0;
        uint32_t SimulationStepsThisFrame = 0;
        uint32_t Seed = 0;
        float SecondaryShare = 0.0f;
        uint32_t TemporalDecimationInterval = 1;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        std::wstring PrimaryAdapterName;
        std::wstring SecondaryAdapterName;
        double CpuWaitMs = 0.0;
        uint64_t TotalCrossAdapterBytes = 0;
        uint64_t ParticleTransferBytes = 0;
        uint32_t SecondaryDrawCalls = 0;
        bool ReusedSecondaryImage = false;
        bool VisualValidationPassed = false;
        std::chrono::steady_clock::time_point CpuFrameStart{};
    };

    struct BenchmarkSummary
    {
        std::string RequestedMode;
        std::string ActualMode;
        std::string Preset;
        std::string TemporalPolicy;
        std::string SkipReason;
        std::wstring PrimaryAdapterName;
        std::wstring SecondaryAdapterName;
        uint32_t TotalVoxelCount = 0;
        float SecondaryShare = 0.0f;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        uint32_t Repetition = 0;
        double AverageCpuFrameMs = 0.0;
        double MedianCpuFrameMs = 0.0;
        double P95CpuFrameMs = 0.0;
        double P99CpuFrameMs = 0.0;
        double StdDevCpuFrameMs = 0.0;
        double CpuFrameCi95HalfWidthMs = 0.0;
        double CriticalPathGpuMs = 0.0;
        double GpuWorkSumMs = 0.0;
        double PrimaryComputeMs = 0.0;
        double PrimaryGraphicsMs = 0.0;
        double SecondaryComputeMs = 0.0;
        double SecondaryGraphicsMs = 0.0;
        double TransferMs = 0.0;
        double CompositeMs = 0.0;
        uint64_t AverageTransferBytes = 0;
        uint64_t AverageParticleTransferBytes = 0;
        double AverageSecondaryDrawCalls = 0.0;
        double ReusedSecondaryImageRate = 0.0;
        double SpeedupVsMatchingSingleGpu = 0.0;
        double Efficiency = 0.0;
        bool VisualValidationPassed = false;
        std::filesystem::path CsvPath;
    };

    struct TimingSnapshot
    {
        bool Valid = false;
        double PrimaryComputeMs = 0.0;
        double PrimaryGraphicsMs = 0.0;
        double SecondaryComputeMs = 0.0;
        double SecondaryGraphicsMs = 0.0;
        double TransferMs = 0.0;
        double CompositeMs = 0.0;
        double FinalResolveUiMs = 0.0;
        double CriticalPathGpuMs = 0.0;
        double GpuWorkSumMs = 0.0;
        uint64_t TransferBytes = 0;
        uint64_t ParticleTransferBytes = 0;
        uint32_t SecondaryDrawCalls = 0;
        bool ReusedSecondaryImage = false;
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
    bool IsComplete() const { return rowsWritten >= RecordedFrameCount; }
    float GetProgress() const;
    uint32_t GetWarmupFramesSeen() const { return std::min(framesSeen, WarmupFrameCount); }
    uint32_t GetRowsWritten() const { return rowsWritten; }
    const std::filesystem::path& GetCsvPath() const { return csvPath; }

    bool Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata);
    bool Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata,
               const std::string& fileName, const std::string& presetName, uint32_t repetition);
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

    static constexpr uint32_t WarmupFrameCount = 100;
    static constexpr uint32_t RecordedFrameCount = 500;

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
        double CpuFrameMs = 0.0;
    };

    std::array<QueueContext, QueueCount> queues{};
    std::array<FrameRecord, RingFrameCount> frames{};
    FrameRecord* currentFrame = nullptr;
    LARGE_INTEGER qpcFrequency{};
    bool initialized = false;
    bool active = false;
    uint32_t framesSeen = 0;
    uint32_t rowsWritten = 0;
    uint32_t currentRepetition = 0;
    std::ofstream csv;
    std::filesystem::path csvPath;
    std::string currentPresetName;
    BenchmarkSummary completedSummary{};
    TimingSnapshot latestTimingSnapshot{};
    bool completedSummaryReady = false;

    std::vector<double> cpuFrameMsSamples;
    std::vector<double> criticalPathGpuMsSamples;
    std::vector<double> gpuWorkSumMsSamples;
    std::vector<double> primaryComputeMsSamples;
    std::vector<double> primaryGraphicsMsSamples;
    std::vector<double> secondaryComputeMsSamples;
    std::vector<double> secondaryGraphicsMsSamples;
    std::vector<double> transferMsSamples;
    std::vector<double> compositeMsSamples;
    std::vector<uint64_t> transferBytesSamples;
    std::vector<uint64_t> particleTransferBytesSamples;
    std::vector<double> secondaryDrawCallSamples;
    std::vector<double> reusedSecondaryImageSamples;

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
    void CreateQueueResources(QueueContext& context);
    void CalibrateQueues();
    void ResetSamples();
    void FinalizeCompletedSummary();
    static double Average(const std::vector<double>& values);
    static double AverageUint64(const std::vector<uint64_t>& values);
    static double StdDev(const std::vector<double>& values);
    static double Percentile(std::vector<double> values, double percentile);
};
