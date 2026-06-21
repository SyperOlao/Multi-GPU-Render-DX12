#pragma once

#include "Source/Benchmark/AutomaticBenchmarkRunner.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct BenchmarkControllerContext
{
    VoxelBenchmarkProfiler& Profiler;
    std::function<VoxelBenchmarkProfiler::FrameMetadata()> BuildMetadata;
    std::function<void(const std::wstring&)> Log;
    std::function<bool()> IsVSyncEnabled;
    std::function<void(bool)> SetVSync;
    std::function<void()> Flush;
    std::function<void(VoxelExecutionMode)> ApplyExecutionMode;
    std::function<void(int)> ApplyVoxelCount;
    std::function<void(float)> ApplySecondaryShare;
    bool MultiGpuAvailable = false;
};

class BenchmarkController
{
public:
    void StartManual(const BenchmarkControllerContext& context);
    void StopManual(const BenchmarkControllerContext& context);
    void StartAutomatic(const BenchmarkControllerContext& context);
    void StopAutomatic(const BenchmarkControllerContext& context);
    void UpdateAutomatic(const BenchmarkControllerContext& context);
    void RestoreVSyncAfterManualCompletion(const BenchmarkControllerContext& context, bool benchmarkWasActive) const;
    void Shutdown(const BenchmarkControllerContext& context);

    bool WasVSyncEnabled() const { return benchmarkVSyncWasEnabled; }
    bool IsAutomaticActive() const { return automaticBenchmarkActive; }
    bool HasAutomaticConfigs() const { return !automaticBenchmarkConfigs.empty(); }
    size_t GetAutomaticIndex() const { return automaticBenchmarkIndex; }
    size_t GetAutomaticCount() const { return automaticBenchmarkConfigs.size(); }
    const std::filesystem::path& GetAutomaticSummaryPath() const { return automaticBenchmarkSummaryPath; }

private:
    std::filesystem::path benchmarkDirectory = L"VoxelBenchmarkResults";
    bool benchmarkVSyncWasEnabled = true;

    std::vector<AutomaticBenchmarkConfig> automaticBenchmarkConfigs;
    std::vector<VoxelBenchmarkProfiler::BenchmarkSummary> automaticBenchmarkSummaries;
    size_t automaticBenchmarkIndex = 0;
    bool automaticBenchmarkActive = false;
    bool automaticBenchmarkStopRequested = false;
    std::filesystem::path automaticBenchmarkSummaryPath;

    void StartAutomaticTest(const BenchmarkControllerContext& context);
    void ApplyBenchmarkVoxelCount(const BenchmarkControllerContext& context, int totalCount) const;
    void WriteAutomaticSummary(const BenchmarkControllerContext& context);
};
