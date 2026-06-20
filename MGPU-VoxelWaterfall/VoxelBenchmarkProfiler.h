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

using namespace PEPEngine::Graphics;

class VoxelBenchmarkProfiler
{
public:
    enum class QueueId : uint8_t
    {
        PrimaryCompute = 0,
        SecondaryCompute,
        Transfer,
        Graphics,
        Count
    };

    enum class RangeId : uint8_t
    {
        NearCompute = 0,
        MediumCompute,
        FarCompute,
        CrossAdapterTransfer,
        Graphics,
        Count
    };

    struct FrameMetadata
    {
        uint64_t FrameIndex = 0;
        std::string ExecutionMode;
        uint32_t NearVoxelCount = 0;
        uint32_t MediumVoxelCount = 0;
        uint32_t FarVoxelCount = 0;
        uint32_t TotalVoxelCount = 0;
        uint32_t UpdatedVoxelCount = 0;
        uint32_t MediumUpdateInterval = 1;
        uint32_t FarUpdateInterval = 1;
        std::wstring PrimaryAdapterName;
        std::wstring SecondaryAdapterName;
        double PrimaryWaitMs = 0.0;
        double SecondaryWaitMs = 0.0;
        std::chrono::steady_clock::time_point CpuFrameStart{};
    };

    struct BenchmarkSummary
    {
        std::string Mode;
        std::string Preset;
        uint32_t TotalVoxelCount = 0;
        double AverageFrameMs = 0.0;
        double MedianFrameMs = 0.0;
        double P95FrameMs = 0.0;
        double AveragePrimaryComputeMs = 0.0;
        double AverageSecondaryComputeMs = 0.0;
        double AverageTransferMs = 0.0;
        double AverageSyncMs = 0.0;
        double AverageGraphicsMs = 0.0;
        bool Target60FpsReached = false;
        std::filesystem::path CsvPath;
    };

    void Initialize(const std::shared_ptr<GDevice>& primaryDevice,
                    const std::shared_ptr<GDevice>& secondaryDevice,
                    const std::shared_ptr<GCommandQueue>& primaryComputeQueue,
                    const std::shared_ptr<GCommandQueue>& secondaryComputeQueue,
                    const std::shared_ptr<GCommandQueue>& transferQueue,
                    const std::shared_ptr<GCommandQueue>& graphicsQueue);

    bool IsInitialized() const { return initialized; }
    bool IsActive() const { return active; }
    bool IsComplete() const { return rowsWritten >= RecordedFrameCount; }
    float GetProgress() const;
    uint32_t GetWarmupFramesSeen() const { return std::min(framesSeen, WarmupFrameCount); }
    uint32_t GetRowsWritten() const { return rowsWritten; }
    const std::filesystem::path& GetCsvPath() const { return csvPath; }

    bool Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata);
    bool Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata,
               const std::string& fileName, const std::string& presetName);
    void Stop();
    bool HasCompletedSummary() const { return completedSummaryReady; }
    BenchmarkSummary ConsumeCompletedSummary();

    void BeginFrame(const FrameMetadata& metadata);
    void UpdateCurrentFrameMetadata(const FrameMetadata& metadata);
    void EndFrameCpu();
    void ProcessCompletedFrames();

    void BeginRange(const std::shared_ptr<GCommandList>& cmdList, QueueId queue, RangeId range);
    void EndRange(const std::shared_ptr<GCommandList>& cmdList, QueueId queue, RangeId range);
    void ResolveRange(const std::shared_ptr<GCommandList>& cmdList, QueueId queue, RangeId range);
    void SetQueueFence(QueueId queue, uint64_t fenceValue);

    static constexpr uint32_t WarmupFrameCount = 100;
    static constexpr uint32_t RecordedFrameCount = 500;

private:
    static constexpr uint32_t RingFrameCount = 16;
    static constexpr uint32_t QueueCount = static_cast<uint32_t>(QueueId::Count);
    static constexpr uint32_t RangeCount = static_cast<uint32_t>(RangeId::Count);
    static constexpr uint32_t QueryCountPerFrame = RangeCount * 2;
    static constexpr uint32_t QueryCountPerQueue = RingFrameCount * QueryCountPerFrame;

    struct QueueContext
    {
        std::shared_ptr<GDevice> Device;
        std::shared_ptr<GCommandQueue> Queue;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> QueryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer;
        uint64_t Frequency = 1;
        bool Valid = false;
    };

    struct RangeRecord
    {
        bool Active = false;
        QueueId Queue = QueueId::PrimaryCompute;
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
    bool initialized = false;
    bool active = false;
    uint32_t framesSeen = 0;
    uint32_t rowsWritten = 0;
    std::ofstream csv;
    std::filesystem::path csvPath;
    std::string currentPresetName;
    BenchmarkSummary completedSummary{};
    bool completedSummaryReady = false;
    std::vector<double> frameMsSamples;
    std::vector<double> primaryComputeMsSamples;
    std::vector<double> secondaryComputeMsSamples;
    std::vector<double> transferMsSamples;
    std::vector<double> syncMsSamples;
    std::vector<double> graphicsMsSamples;

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
    double ReadRangeMs(const FrameRecord& frame, RangeId range) const;
    void WriteFrame(const FrameRecord& frame);
    void CreateQueueResources(QueueContext& context);
    void ResetSamples();
    void FinalizeCompletedSummary();
    static double Average(const std::vector<double>& values);
    static double Percentile(std::vector<double> values, double percentile);
};
