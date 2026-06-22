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
    std::function<void(bool)> ApplySpatialLodEnabled;
    std::function<void(uint32_t)> ApplyTemporalInterval;
    std::function<BenchmarkConfigurationApplyResult(const AutomaticBenchmarkConfig&)> ApplyBenchmarkConfiguration;
    std::function<void()> ResetDeterministicBenchmarkState;
    bool MultiGpuAvailable = false;
    bool VisualValidationPassed = false;
    std::string VisualValidationRunId;
    std::string VisualValidationReason;
    std::string CurrentBuildHash;
    std::string CurrentShaderHash;
    std::string ValidationBuildHash;
    std::string ValidationShaderHash;
    std::string ValidationAdapterPairIdentity;
    bool TwoAdapterVerificationPassed = false;
    std::string TwoAdapterVerificationRunId;
    std::string TwoAdapterAdapterPairIdentity;
    std::string TwoAdapterBuildHash;
    std::string TwoAdapterVerificationReason;
};

class BenchmarkController
{
public:
    void StartManual(const BenchmarkControllerContext& context);
    void StopManual(const BenchmarkControllerContext& context);
    bool StartAutomatic(const BenchmarkControllerContext& context,
                        BenchmarkSuite suite = BenchmarkSuite::Full,
                        uint32_t seedOverride = 0);
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
    const std::filesystem::path& GetSuiteStatusPath() const { return automaticBenchmarkStatusPath; }
    void SetBenchmarkDirectory(const std::filesystem::path& path) { benchmarkDirectory = path; }

private:
    std::filesystem::path benchmarkDirectory = L"VoxelBenchmarkResults";
    bool benchmarkVSyncWasEnabled = true;

    std::vector<AutomaticBenchmarkConfig> automaticBenchmarkConfigs;
    std::vector<VoxelBenchmarkProfiler::BenchmarkSummary> automaticBenchmarkSummaries;
    struct ExecutionManifestState
    {
        std::filesystem::path Path;
        AutomaticBenchmarkConfig Config{};
        BenchmarkConfigurationApplyResult Resolved{};
        std::string Status;
        std::string Reason;
    };
    std::vector<ExecutionManifestState> executionManifestStates;
    size_t automaticBenchmarkIndex = 0;
    bool automaticBenchmarkActive = false;
    bool automaticBenchmarkStopRequested = false;
    std::filesystem::path automaticBenchmarkSummaryPath;
    std::filesystem::path automaticBenchmarkStatusPath;
    BenchmarkSuite activeSuite = BenchmarkSuite::Full;
    uint32_t activeSuiteSeed = 0;
    std::string activeSuiteRunId;

    void StartAutomaticTest(const BenchmarkControllerContext& context);
    void ApplyBenchmarkVoxelCount(const BenchmarkControllerContext& context, int totalCount) const;
    void WriteAutomaticSummary(const BenchmarkControllerContext& context);
    void FinalizeAutomaticArtifacts(const BenchmarkControllerContext& context,
                                    const char* status,
                                    const std::string& reason);
};
