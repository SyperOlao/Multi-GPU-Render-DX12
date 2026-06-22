#include "Source/Benchmark/BenchmarkController.h"

#include "Source/Benchmark/BenchmarkCsvWriter.h"
#include "Source/Benchmark/ResearchArtifactWriter.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
    std::wstring ToWide(const char* value)
    {
        return std::wstring(value, value + std::strlen(value));
    }

    std::string EscapeJson(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    std::string UtcTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_s(&utc, &time);
        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return stream.str();
    }

    std::string MakeRunId(const BenchmarkSuite suite, const uint32_t seed)
    {
        std::ostringstream stream;
        stream << AutomaticBenchmarkRunner::SuiteName(suite) << "-" << std::hex << seed << "-"
               << UtcTimestamp();
        auto value = stream.str();
        for (auto& ch : value)
        {
            if (ch == ':' || ch == '-')
                ch = '_';
        }
        return value;
    }

    std::string SanitizeFileToken(std::string value)
    {
        for (auto& ch : value)
        {
            if (ch == ':' || ch == '/' || ch == '\\' || ch == '?' || ch == '*' ||
                ch == '"' || ch == '<' || ch == '>' || ch == '|')
            {
                ch = '_';
            }
        }
        return value;
    }

    bool RequestsMultiGpu(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::MultiGpuFull ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    std::string BuildGateFailureReason(const BenchmarkControllerContext& context)
    {
        if (!context.VisualValidationPassed)
        {
            return context.VisualValidationReason.empty()
                       ? "deterministic visual validation has not passed"
                       : "deterministic visual validation has not passed: " + context.VisualValidationReason;
        }
        if (context.CurrentBuildHash.empty() || context.ValidationBuildHash.empty() ||
            context.CurrentBuildHash != context.ValidationBuildHash)
        {
            return "visual validation build hash does not match current executable";
        }
        if (context.CurrentShaderHash.empty() || context.ValidationShaderHash.empty() ||
            context.CurrentShaderHash != context.ValidationShaderHash)
        {
            return "visual validation shader hash does not match current shader";
        }
        if (!context.TwoAdapterVerificationPassed)
        {
            return context.TwoAdapterVerificationReason.empty()
                       ? "two-hardware-adapter runtime verification has not passed"
                       : "two-hardware-adapter runtime verification has not passed: " +
                         context.TwoAdapterVerificationReason;
        }
        if (context.CurrentBuildHash.empty() || context.TwoAdapterBuildHash.empty() ||
            context.CurrentBuildHash != context.TwoAdapterBuildHash)
        {
            return "two-adapter verification build hash does not match current executable";
        }
        if (context.ValidationAdapterPairIdentity.empty() ||
            context.TwoAdapterAdapterPairIdentity.empty() ||
            context.ValidationAdapterPairIdentity != context.TwoAdapterAdapterPairIdentity)
        {
            return "visual validation adapter pair does not match two-adapter verification pair";
        }
        return {};
    }

    BenchmarkResearchArtifactContext BuildArtifactContext(
        const BenchmarkControllerContext& context,
        const BenchmarkSuite suite,
        const std::string& runId,
        const uint32_t seed,
        const uint32_t warmupFrames,
        const uint32_t measuredFrames,
        const size_t executionCount,
        const std::filesystem::path& outputDirectory,
        const std::filesystem::path& statusPath,
        const std::filesystem::path& summaryPath,
        const std::string& gateStatus,
        const std::string& gateReason)
    {
        BenchmarkResearchArtifactContext artifact{};
        artifact.Suite = suite;
        artifact.RunId = runId;
        artifact.RandomizationSeed = seed;
        artifact.WarmupFrames = warmupFrames;
        artifact.MeasuredFrames = measuredFrames;
        artifact.ExecutionCount = executionCount;
        artifact.CurrentBuildHash = context.CurrentBuildHash;
        artifact.CurrentShaderHash = context.CurrentShaderHash;
        artifact.ValidationRunId = context.VisualValidationRunId;
        artifact.TwoGpuVerificationRunId = context.TwoAdapterVerificationRunId;
        artifact.ValidationAdapterPairIdentity = context.ValidationAdapterPairIdentity;
        artifact.TwoGpuAdapterPairIdentity = context.TwoAdapterAdapterPairIdentity;
        artifact.GateStatus = gateStatus;
        artifact.GateReason = gateReason;
        artifact.OutputDirectory = outputDirectory;
        artifact.StatusPath = statusPath;
        artifact.SummaryPath = summaryPath;
        return artifact;
    }

    void WriteSuiteStatus(const std::filesystem::path& path,
                          const BenchmarkSuite suite,
                          const std::string& runId,
                          const uint32_t seed,
                          const uint32_t warmupFrames,
                          const uint32_t measuredFrames,
                          const size_t executionCount,
                          const char* status,
                          const std::string& reason,
                          const BenchmarkControllerContext& context)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream json(path, std::ios::out | std::ios::trunc);
        json << "{\n"
             << "  \"suite\":\"" << AutomaticBenchmarkRunner::SuiteName(suite) << "\",\n"
             << "  \"run_id\":\"" << EscapeJson(runId) << "\",\n"
             << "  \"status\":\"" << status << "\",\n"
             << "  \"reason\":\"" << EscapeJson(reason) << "\",\n"
             << "  \"randomization_seed\":" << seed << ",\n"
             << "  \"warmup_frames\":" << warmupFrames << ",\n"
             << "  \"measured_frames\":" << measuredFrames << ",\n"
             << "  \"execution_count\":" << executionCount << ",\n"
             << "  \"visual_validation_run_id\":\"" << EscapeJson(context.VisualValidationRunId) << "\",\n"
             << "  \"two_gpu_verification_run_id\":\"" << EscapeJson(context.TwoAdapterVerificationRunId) << "\",\n"
             << "  \"current_build_hash\":\"" << EscapeJson(context.CurrentBuildHash) << "\",\n"
             << "  \"current_shader_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
             << "  \"validation_build_hash\":\"" << EscapeJson(context.ValidationBuildHash) << "\",\n"
             << "  \"validation_shader_hash\":\"" << EscapeJson(context.ValidationShaderHash) << "\",\n"
             << "  \"validation_adapter_pair\":\"" << EscapeJson(context.ValidationAdapterPairIdentity) << "\",\n"
             << "  \"two_gpu_adapter_pair\":\"" << EscapeJson(context.TwoAdapterAdapterPairIdentity) << "\"\n"
             << "}\n";
    }

    void WriteExecutionManifest(const std::filesystem::path& path,
                                const AutomaticBenchmarkConfig& config,
                                const std::string& runId,
                                const BenchmarkConfigurationApplyResult& resolved,
                                const BenchmarkControllerContext& context,
                                const char* status,
                                const std::string& reason)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream json(path, std::ios::out | std::ios::trunc);
        json << "{\n"
             << "  \"suite\":\"" << AutomaticBenchmarkRunner::SuiteName(config.Suite) << "\",\n"
             << "  \"run_id\":\"" << EscapeJson(runId) << "\",\n"
             << "  \"config_id\":\"" << EscapeJson(config.ConfigId) << "\",\n"
             << "  \"pair_id\":\"" << EscapeJson(config.PairId) << "\",\n"
             << "  \"session_id\":\"" << EscapeJson(config.SessionId) << "\",\n"
             << "  \"block_id\":\"" << EscapeJson(config.BlockId) << "\",\n"
             << "  \"repetition\":" << config.Repetition << ",\n"
             << "  \"randomized_order_index\":" << config.OrderIndex << ",\n"
             << "  \"block_order_index\":" << config.BlockOrderIndex << ",\n"
             << "  \"pair_member_order\":" << config.PairMemberOrder << ",\n"
             << "  \"randomization_seed\":" << config.RandomizationSeed << ",\n"
             << "  \"warmup_frames\":" << config.WarmupFrameCount << ",\n"
             << "  \"measured_frames\":" << config.MeasuredFrameCount << ",\n"
             << "  \"requested_mode\":\"" << config.ModeName << "\",\n"
             << "  \"actual_mode\":\"" << EscapeJson(resolved.ActualMode) << "\",\n"
             << "  \"requested_total_count\":" << config.TotalCount << ",\n"
             << "  \"actual_static_count\":" << resolved.ActualStaticCount << ",\n"
             << "  \"actual_dynamic_count\":" << resolved.ActualDynamicCount << ",\n"
             << "  \"actual_total_count\":" << resolved.ActualTotalCount << ",\n"
             << "  \"secondary_share\":" << config.SecondaryShare << ",\n"
             << "  \"spatial_lod\":\"" << (config.SpatialLodEnabled ? "ThreeLevel" : "Off") << "\",\n"
             << "  \"temporal_interval\":" << config.TemporalInterval << ",\n"
             << "  \"resolved_config_hash\":\"" << EscapeJson(resolved.ResolvedConfigHash) << "\",\n"
             << "  \"validation_run_id\":\"" << EscapeJson(context.VisualValidationRunId) << "\",\n"
             << "  \"two_gpu_verification_run_id\":\"" << EscapeJson(context.TwoAdapterVerificationRunId) << "\",\n"
             << "  \"build_hash\":\"" << EscapeJson(context.CurrentBuildHash) << "\",\n"
             << "  \"shader_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
             << "  \"start_utc\":\"" << UtcTimestamp() << "\",\n"
             << "  \"end_utc\":\"\",\n"
             << "  \"status\":\"" << status << "\",\n"
             << "  \"reason\":\"" << EscapeJson(reason) << "\"\n"
             << "}\n";
    }
}

void BenchmarkController::StartManual(const BenchmarkControllerContext& context)
{
    benchmarkVSyncWasEnabled = context.IsVSyncEnabled();
    if (benchmarkVSyncWasEnabled)
        context.SetVSync(false);

    if (context.ResetDeterministicBenchmarkState)
        context.ResetDeterministicBenchmarkState();

    if (context.Profiler.Start(benchmarkDirectory, context.BuildMetadata()))
        context.Log(L"\nVoxel benchmark started: " + context.Profiler.GetCsvPath().wstring());
    else
        context.Log(L"\nVoxel benchmark failed to start");
}

void BenchmarkController::StopManual(const BenchmarkControllerContext& context)
{
    context.Profiler.Stop();
    context.SetVSync(benchmarkVSyncWasEnabled);
    context.Log(L"\nVoxel benchmark stopped");
}

bool BenchmarkController::StartAutomatic(const BenchmarkControllerContext& context,
                                         const BenchmarkSuite suite,
                                         const uint32_t seedOverride)
{
    StopAutomatic(context);

    automaticBenchmarkConfigs.clear();
    automaticBenchmarkSummaries.clear();
    automaticBenchmarkIndex = 0;
    automaticBenchmarkStopRequested = false;
    activeSuite = suite;
    automaticBenchmarkConfigs = AutomaticBenchmarkRunner::BuildConfigs(suite, seedOverride);
    activeSuiteSeed = automaticBenchmarkConfigs.empty() ? seedOverride : automaticBenchmarkConfigs.front().RandomizationSeed;
    activeSuiteRunId = MakeRunId(suite, activeSuiteSeed);
    automaticBenchmarkSummaryPath = benchmarkDirectory /
        "paired_summary.csv";
    automaticBenchmarkStatusPath = benchmarkDirectory /
        ("VoxelBenchmark_" + std::string(AutomaticBenchmarkRunner::SuiteName(suite)) + "_Status.json");

    const uint32_t warmupFrames = automaticBenchmarkConfigs.empty()
                                      ? 0
                                      : automaticBenchmarkConfigs.front().WarmupFrameCount;
    const uint32_t measuredFrames = automaticBenchmarkConfigs.empty()
                                        ? 0
                                        : automaticBenchmarkConfigs.front().MeasuredFrameCount;
    const auto gateFailure = BuildGateFailureReason(context);
    auto artifactContext = BuildArtifactContext(context, suite, activeSuiteRunId, activeSuiteSeed,
                                                warmupFrames, measuredFrames,
                                                automaticBenchmarkConfigs.size(),
                                                benchmarkDirectory,
                                                automaticBenchmarkStatusPath,
                                                automaticBenchmarkSummaryPath,
                                                gateFailure.empty() ? "PENDING" : "BLOCKED",
                                                gateFailure);
    ResearchArtifactWriter::WriteSuiteManifest(artifactContext, automaticBenchmarkConfigs);
    ResearchArtifactWriter::WriteEnvironment(artifactContext, context.BuildMetadata());
    ResearchArtifactWriter::WriteReproductionReadme(artifactContext);
    if (!gateFailure.empty())
    {
        WriteSuiteStatus(automaticBenchmarkStatusPath, suite, activeSuiteRunId, activeSuiteSeed,
                         warmupFrames, measuredFrames, automaticBenchmarkConfigs.size(),
                         "BLOCKED", gateFailure, context);
        artifactContext.GateStatus = "BLOCKED";
        ResearchArtifactWriter::WriteRunsCsv(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WriteRawFramesCsv(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WritePairedRunsCsv(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WriteInvalidRecords(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WriteTelemetryCsv(artifactContext);
        ResearchArtifactWriter::WriteMemoryTimelineCsv(artifactContext);
        BenchmarkCsvWriter::WriteAutomaticSummary(automaticBenchmarkSummaryPath, automaticBenchmarkSummaries);
        context.Log(L"\nAutomatic voxel benchmark blocked: " + ToWide(gateFailure.c_str()));
        return false;
    }

    benchmarkVSyncWasEnabled = context.IsVSyncEnabled();
    if (benchmarkVSyncWasEnabled)
        context.SetVSync(false);

    automaticBenchmarkActive = true;
    WriteSuiteStatus(automaticBenchmarkStatusPath, suite, activeSuiteRunId, activeSuiteSeed,
                     warmupFrames, measuredFrames, automaticBenchmarkConfigs.size(),
                     "RUNNING", "", context);
    artifactContext.GateStatus = "RUNNING";
    artifactContext.GateReason.clear();
    ResearchArtifactWriter::WriteSuiteManifest(artifactContext, automaticBenchmarkConfigs);
    context.Log(L"\nAutomatic voxel benchmark started");
    return true;
}

void BenchmarkController::StopAutomatic(const BenchmarkControllerContext& context)
{
    const bool shouldRestoreVSync = automaticBenchmarkActive || context.Profiler.IsActive();
    if (context.Profiler.IsActive())
        context.Profiler.Stop();

    if (automaticBenchmarkActive)
    {
        context.Log(L"\nAutomatic voxel benchmark stop requested");
        if (!automaticBenchmarkSummaries.empty())
            WriteAutomaticSummary(context);
    }

    automaticBenchmarkStopRequested = true;
    automaticBenchmarkActive = false;
    if (shouldRestoreVSync)
        context.SetVSync(benchmarkVSyncWasEnabled);
}

void BenchmarkController::UpdateAutomatic(const BenchmarkControllerContext& context)
{
    if (!automaticBenchmarkActive)
        return;

    if (context.Profiler.HasCompletedSummary())
    {
        auto summary = context.Profiler.ConsumeCompletedSummary();
        if (automaticBenchmarkIndex < automaticBenchmarkConfigs.size())
        {
            const auto& config = automaticBenchmarkConfigs[automaticBenchmarkIndex];
            summary.RequestedMode = config.ModeName;
            summary.Preset = config.Preset;
            summary.TotalVoxelCount = config.TotalCount;
            summary.SecondaryShare = config.SecondaryShare;
            summary.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
            summary.TemporalPolicy =
                config.TemporalInterval <= 1 ? "Full" : "Decimated";
            summary.Repetition = config.Repetition;
        }
        automaticBenchmarkSummaries.push_back(summary);
        context.Log(L"\nFinished benchmark CSV: " + summary.CsvPath.wstring());
        ++automaticBenchmarkIndex;
    }

    if (context.Profiler.IsActive())
        return;

    if (automaticBenchmarkStopRequested || automaticBenchmarkIndex >= automaticBenchmarkConfigs.size())
    {
        WriteAutomaticSummary(context);
        automaticBenchmarkActive = false;
        automaticBenchmarkStopRequested = false;
        context.SetVSync(benchmarkVSyncWasEnabled);
        WriteSuiteStatus(automaticBenchmarkStatusPath, activeSuite, activeSuiteRunId, activeSuiteSeed,
                         automaticBenchmarkConfigs.empty() ? 0 : automaticBenchmarkConfigs.front().WarmupFrameCount,
                         automaticBenchmarkConfigs.empty() ? 0 : automaticBenchmarkConfigs.front().MeasuredFrameCount,
                         automaticBenchmarkConfigs.size(),
                         "COMPLETE", "", context);
        context.Log(L"\nAutomatic voxel benchmark finished");
        return;
    }

    StartAutomaticTest(context);
}

void BenchmarkController::RestoreVSyncAfterManualCompletion(const BenchmarkControllerContext& context,
                                                            const bool benchmarkWasActive) const
{
    if (benchmarkWasActive && !context.Profiler.IsActive())
        context.SetVSync(benchmarkVSyncWasEnabled);
}

void BenchmarkController::Shutdown(const BenchmarkControllerContext& context)
{
    context.Profiler.Stop();
    context.SetVSync(benchmarkVSyncWasEnabled);
}

void BenchmarkController::StartAutomaticTest(const BenchmarkControllerContext& context)
{
    if (automaticBenchmarkIndex >= automaticBenchmarkConfigs.size())
        return;

    const auto& config = automaticBenchmarkConfigs[automaticBenchmarkIndex];
    const bool requestsMultiGpu = RequestsMultiGpu(config.Mode);
    if (requestsMultiGpu && !context.MultiGpuAvailable)
    {
        context.Log(L"\nSkipped benchmark " + ToWide(config.ModeName) +
            L" / " + ToWide(config.Preset) +
            L": secondary hardware adapter unavailable");
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = "Skipped";
        skipped.Preset = config.Preset;
        skipped.TotalVoxelCount = config.TotalCount;
        skipped.SecondaryShare = config.SecondaryShare;
        skipped.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
        skipped.TemporalPolicy = config.TemporalInterval <= 1 ? "Full" : "Decimated";
        skipped.PairId = config.PairId;
        skipped.SessionId = config.SessionId;
        skipped.BlockId = config.BlockId;
        skipped.Repetition = config.Repetition;
        skipped.Valid = false;
        skipped.SkipReason = "secondary hardware adapter unavailable";
        automaticBenchmarkSummaries.push_back(skipped);
        ++automaticBenchmarkIndex;
        return;
    }
    if (requestsMultiGpu && !context.TwoAdapterVerificationPassed)
    {
        const auto reason = context.TwoAdapterVerificationReason.empty()
                                ? "two-adapter runtime verification has not passed for this build/adapter pair"
                                : context.TwoAdapterVerificationReason;
        context.Log(L"\nSkipped benchmark " + ToWide(config.ModeName) +
            L" / " + ToWide(config.Preset) +
            L": " + ToWide(reason.c_str()));
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = "Skipped";
        skipped.Preset = config.Preset;
        skipped.TotalVoxelCount = config.TotalCount;
        skipped.SecondaryShare = config.SecondaryShare;
        skipped.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
        skipped.TemporalPolicy = config.TemporalInterval <= 1 ? "Full" : "Decimated";
        skipped.PairId = config.PairId;
        skipped.SessionId = config.SessionId;
        skipped.BlockId = config.BlockId;
        skipped.Repetition = config.Repetition;
        skipped.Valid = false;
        skipped.SkipReason = reason;
        automaticBenchmarkSummaries.push_back(skipped);
        ++automaticBenchmarkIndex;
        return;
    }

    BenchmarkConfigurationApplyResult resolved{};
    if (context.ApplyBenchmarkConfiguration)
    {
        resolved = context.ApplyBenchmarkConfiguration(config);
    }
    else
    {
        context.Flush();
        context.ApplyExecutionMode(config.Mode);
        ApplyBenchmarkVoxelCount(context, static_cast<int>(config.TotalCount));
        if (context.ApplySecondaryShare)
            context.ApplySecondaryShare(config.SecondaryShare);
        if (context.ApplySpatialLodEnabled)
            context.ApplySpatialLodEnabled(config.SpatialLodEnabled);
        if (context.ApplyTemporalInterval)
            context.ApplyTemporalInterval(config.TemporalInterval);
        if (context.ResetDeterministicBenchmarkState)
            context.ResetDeterministicBenchmarkState();
        context.Flush();
        resolved.Passed = true;
        resolved.ActualMode = config.ModeName;
    }

    const auto manifestPath = benchmarkDirectory /
        ("VoxelBenchmark_" + std::string(AutomaticBenchmarkRunner::SuiteName(config.Suite)) + "_" +
         SanitizeFileToken(config.ConfigId) + "_manifest.json");
    if (!resolved.Passed)
    {
        WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context,
                               "INVALID", resolved.Reason);
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = resolved.ActualMode.empty() ? "Invalid" : resolved.ActualMode;
        skipped.Preset = config.Preset;
        skipped.TotalVoxelCount = config.TotalCount;
        skipped.ActualStaticVoxelCount = resolved.ActualStaticCount;
        skipped.ActualDynamicVoxelCount = resolved.ActualDynamicCount;
        skipped.SecondaryShare = config.SecondaryShare;
        skipped.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
        skipped.TemporalPolicy = config.TemporalInterval <= 1 ? "Full" : "Decimated";
        skipped.PairId = config.PairId;
        skipped.SessionId = config.SessionId;
        skipped.BlockId = config.BlockId;
        skipped.Repetition = config.Repetition;
        skipped.RepetitionCount = config.RepetitionCount;
        skipped.Valid = false;
        skipped.SkipReason = resolved.Reason;
        automaticBenchmarkSummaries.push_back(skipped);
        ++automaticBenchmarkIndex;
        return;
    }
    WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context, "RUNNING", "");

    const std::string fileName = "VoxelBenchmark_" + std::string(config.ModeName) + "_" +
        config.Preset + "_" + std::to_string(config.TotalCount) +
        "_share" + std::to_string(static_cast<int>(config.SecondaryShare * 100.0f)) +
        "_" + (config.SpatialLodEnabled ? "SpatialLOD" : "NoSpatialLOD") +
        "_temporal" + std::to_string(config.TemporalInterval) +
        "_rep" + std::to_string(config.Repetition) + ".csv";
    if (!context.Profiler.Start(benchmarkDirectory, context.BuildMetadata(), fileName,
                                config.Preset, config.Repetition,
                                config.WarmupFrameCount, config.MeasuredFrameCount,
                                AutomaticBenchmarkRunner::SuiteName(config.Suite),
                                activeSuiteRunId,
                                config.ConfigId,
                                config.PairId,
                                config.SessionId,
                                config.BlockId,
                                config.OrderIndex,
                                config.BlockOrderIndex,
                                config.PairMemberOrder,
                                config.RandomizationSeed))
    {
        context.Log(L"\nFailed to start benchmark " + ToWide(config.ModeName));
        ++automaticBenchmarkIndex;
        return;
    }

    context.Log(L"\nStarted benchmark: " + ToWide(config.ModeName) +
        L" / " + ToWide(config.Preset) +
        L" / " + std::to_wstring(config.TotalCount) + L" voxels" +
        L" / secondary share " + std::to_wstring(config.SecondaryShare));
}

void BenchmarkController::ApplyBenchmarkVoxelCount(const BenchmarkControllerContext& context,
                                                   const int totalCount) const
{
    context.ApplyVoxelCount(totalCount);
}

void BenchmarkController::WriteAutomaticSummary(const BenchmarkControllerContext& context)
{
    const uint32_t warmupFrames = automaticBenchmarkConfigs.empty()
                                      ? 0
                                      : automaticBenchmarkConfigs.front().WarmupFrameCount;
    const uint32_t measuredFrames = automaticBenchmarkConfigs.empty()
                                        ? 0
                                        : automaticBenchmarkConfigs.front().MeasuredFrameCount;
    auto artifactContext = BuildArtifactContext(context, activeSuite, activeSuiteRunId, activeSuiteSeed,
                                                warmupFrames, measuredFrames,
                                                automaticBenchmarkConfigs.size(),
                                                benchmarkDirectory,
                                                automaticBenchmarkStatusPath,
                                                automaticBenchmarkSummaryPath,
                                                automaticBenchmarkActive ? "RUNNING" : "COMPLETE",
                                                "");
    ResearchArtifactWriter::WriteRunsCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteRawFramesCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WritePairedRunsCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteInvalidRecords(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteTelemetryCsv(artifactContext);
    ResearchArtifactWriter::WriteMemoryTimelineCsv(artifactContext);

    if (automaticBenchmarkSummaries.empty())
        return;

    if (BenchmarkCsvWriter::WriteAutomaticSummary(automaticBenchmarkSummaryPath, automaticBenchmarkSummaries))
        context.Log(L"\nAutomatic benchmark summary written: " + automaticBenchmarkSummaryPath.wstring());
}
