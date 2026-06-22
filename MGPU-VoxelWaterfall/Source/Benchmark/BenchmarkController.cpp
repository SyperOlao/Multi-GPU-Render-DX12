#include "Source/Benchmark/BenchmarkController.h"

#include "Source/Benchmark/BenchmarkCsvWriter.h"
#include "Source/Benchmark/ResearchArtifactWriter.h"

#include <algorithm>
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

    bool IsTemporalMode(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    std::string ValidationModeFamily(const VoxelExecutionMode mode)
    {
        return IsTemporalMode(mode) ? "Temporal" : "Full";
    }

    std::string ValidationConfigKey(const AutomaticBenchmarkConfig& config)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << AutomaticBenchmarkRunner::SuiteName(config.Suite)
               << "|family=" << ValidationModeFamily(config.Mode)
               << "|preset=" << config.Preset
               << "|static_budget_label=" << config.RequestedLabelCount
               << "|static=" << config.RequestedStaticBudget
               << "|dynamic=" << config.RequestedDynamicBudget
               << "|share=" << std::fixed << std::setprecision(2) << config.SecondaryShare
               << "|lod=" << (config.SpatialLodEnabled ? "on" : "off")
               << "|interval=" << config.TemporalInterval;
        return stream.str();
    }

    std::filesystem::path ExecutionManifestPath(const std::filesystem::path& benchmarkDirectory,
                                                const AutomaticBenchmarkConfig& config)
    {
        return benchmarkDirectory /
            ("VoxelBenchmark_" + std::string(AutomaticBenchmarkRunner::SuiteName(config.Suite)) + "_" +
             SanitizeFileToken(config.ConfigId) + "_manifest.json");
    }

    std::string BuildGateFailureReason(const BenchmarkControllerContext& context,
                                       const std::vector<AutomaticBenchmarkConfig>& configs)
    {
        const std::vector<std::string> requiredProvenanceFields = {
            "build.executable_sha256",
            "build.shader_bytecode_set_sha256",
            "adapter.luid_pair",
            "adapter.primary_driver_version",
            "adapter.secondary_driver_version",
            "validation.protocol_sha256",
            "validation.case_config_sha256",
            "validation.camera_sha256",
            "render.resolution",
            "render.color_format",
            "render.depth_format",
            "render.sample_count",
            "workload.static_seed",
            "workload.dynamic_seed",
            "workload.requested_static_count",
            "workload.requested_dynamic_count",
            "workload.actual_static_count",
            "workload.actual_dynamic_count",
            "workload.temporal_interval",
            "workload.spatial_lod",
            "workload.partition_strategy",
            "workload.chunk_size",
            "runtime.toolchain"
        };
        if (context.ValidationJsonPath.empty() || !std::filesystem::exists(context.ValidationJsonPath))
            return "missing validation evidence file: voxel_visual_validation.json";
        if (context.ValidationCsvPath.empty() || !std::filesystem::exists(context.ValidationCsvPath))
            return "missing validation evidence file: voxel_visual_validation.csv";
        if (context.TwoAdapterJsonPath.empty() || !std::filesystem::exists(context.TwoAdapterJsonPath))
            return "missing two-adapter preflight evidence file: two_adapter_preflight.json";
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
            return "visual validation shader set hash does not match current shader set";
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
        if (!context.ValidationProtocolHash.empty())
        {
            const auto it = context.ValidationProvenance.Fields.find("validation.protocol_sha256");
            if (it == context.ValidationProvenance.Fields.end() || it->second != context.ValidationProtocolHash)
                return "visual validation protocol hash does not match benchmark protocol";
        }
        if (!context.ValidationCaseConfigHash.empty())
        {
            const auto it = context.ValidationProvenance.Fields.find("validation.case_config_sha256");
            if (it == context.ValidationProvenance.Fields.end() || it->second != context.ValidationCaseConfigHash)
                return "visual validation case/config hash does not match benchmark config";
        }
        if (!context.ValidationCameraHash.empty())
        {
            const auto it = context.ValidationProvenance.Fields.find("validation.camera_sha256");
            if (it == context.ValidationProvenance.Fields.end() || it->second != context.ValidationCameraHash)
                return "visual validation camera hash does not match benchmark camera";
        }
        if (const auto mismatch = ResearchProvenance::CompareCompatible(
                context.CurrentProvenance, context.ValidationProvenance, requiredProvenanceFields);
            !mismatch.empty())
        {
            return "visual validation " + mismatch;
        }
        if (const auto mismatch = ResearchProvenance::CompareCompatible(
                context.CurrentProvenance, context.TwoAdapterProvenance,
                {"build.executable_sha256", "adapter.luid_pair",
                 "adapter.primary_driver_version", "adapter.secondary_driver_version",
                 "render.color_format", "render.depth_format", "runtime.toolchain"});
            !mismatch.empty())
        {
            return "two-adapter verification " + mismatch;
        }
        for (const auto& config : configs)
        {
            const auto key = ValidationConfigKey(config);
            const auto it = std::find_if(
                context.ValidationCoverage.begin(),
                context.ValidationCoverage.end(),
                [&](const BenchmarkValidationCoverage& coverage)
                {
                    return coverage.Passed &&
                        coverage.ConfigKey == key &&
                        coverage.ModeName == config.ModeName &&
                        coverage.ProtocolHash == context.ValidationProtocolHash &&
                        !coverage.ConfigHash.empty() &&
                        !coverage.CameraHash.empty();
                });
            if (it == context.ValidationCoverage.end())
            {
                return "missing matching PASS visual validation case for benchmark config: " +
                    config.ConfigId;
            }
            if (config.TemporalInterval > 1 || config.SpatialLodEnabled)
            {
                const auto fidelityIt = std::find_if(
                    context.ValidationCoverage.begin(),
                    context.ValidationCoverage.end(),
                    [&](const BenchmarkValidationCoverage& coverage)
                    {
                        return coverage.Passed &&
                            coverage.ModeName == config.ModeName &&
                            coverage.ProtocolHash == context.ValidationProtocolHash &&
                            coverage.CaseId.find("|fidelity|") != std::string::npos;
                    });
                if (fidelityIt == context.ValidationCoverage.end())
                {
                    return "missing approximation-fidelity PASS visual validation case for benchmark config: " +
                        config.ConfigId;
                }
            }
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
        const std::string& gateReason,
        const std::string& createdUtc,
        const std::string& startUtc,
        const std::string& endUtc)
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
        artifact.CreatedUtc = createdUtc;
        artifact.StartUtc = startUtc;
        artifact.EndUtc = endUtc;
        artifact.Provenance = context.CurrentProvenance;
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
                          const BenchmarkControllerContext& context,
                          const std::string& createdUtc,
                          const std::string& startUtc,
                          const std::string& endUtc)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ostringstream json;
        json << "{\n"
             << "  \"suite\":\"" << AutomaticBenchmarkRunner::SuiteName(suite) << "\",\n"
             << "  \"run_id\":\"" << EscapeJson(runId) << "\",\n"
             << "  \"status\":\"" << status << "\",\n"
             << "  \"reason\":\"" << EscapeJson(reason) << "\",\n"
             << "  \"randomization_seed\":" << seed << ",\n"
             << "  \"warmup_frames\":" << warmupFrames << ",\n"
             << "  \"measured_frames\":" << measuredFrames << ",\n"
             << "  \"execution_count\":" << executionCount << ",\n"
             << "  \"created_utc\":\"" << EscapeJson(createdUtc) << "\",\n"
             << "  \"start_utc\":\"" << EscapeJson(startUtc) << "\",\n"
             << "  \"end_utc\":\"" << EscapeJson(endUtc) << "\",\n"
             << "  \"visual_validation_run_id\":\"" << EscapeJson(context.VisualValidationRunId) << "\",\n"
             << "  \"two_gpu_verification_run_id\":\"" << EscapeJson(context.TwoAdapterVerificationRunId) << "\",\n"
             << "  \"current_build_hash\":\"" << EscapeJson(context.CurrentBuildHash) << "\",\n"
             << "  \"current_shader_set_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
             << "  \"current_shader_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
             << "  \"validation_build_hash\":\"" << EscapeJson(context.ValidationBuildHash) << "\",\n"
             << "  \"validation_shader_set_hash\":\"" << EscapeJson(context.ValidationShaderHash) << "\",\n"
             << "  \"validation_shader_hash\":\"" << EscapeJson(context.ValidationShaderHash) << "\",\n"
             << "  \"validation_adapter_pair\":\"" << EscapeJson(context.ValidationAdapterPairIdentity) << "\",\n"
             << "  \"two_gpu_adapter_pair\":\"" << EscapeJson(context.TwoAdapterAdapterPairIdentity) << "\",\n"
             << ResearchProvenance::ToJson(context.CurrentProvenance, 2) << "\n"
             << "}\n";
        ResearchProvenance::WriteTextFileAtomic(path, json.str());
    }

    void WriteExecutionManifest(const std::filesystem::path& path,
                                const AutomaticBenchmarkConfig& config,
                                const std::string& runId,
                                const BenchmarkConfigurationApplyResult& resolved,
                                const BenchmarkControllerContext& context,
                                const char* status,
                                const std::string& reason,
                                const std::string& startUtc,
                                const std::string& endUtc)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ostringstream json;
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
             << "  \"requested_static_budget_label\":" << config.RequestedLabelCount << ",\n"
             << "  \"requested_static_budget\":" << config.RequestedStaticBudget << ",\n"
             << "  \"requested_dynamic_budget\":" << config.RequestedDynamicBudget << ",\n"
             << "  \"resolved_requested_label_count\":" << resolved.RequestedLabelCount << ",\n"
             << "  \"resolved_requested_static_budget\":" << resolved.RequestedStaticBudget << ",\n"
             << "  \"resolved_requested_dynamic_budget\":" << resolved.RequestedDynamicBudget << ",\n"
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
             << "  \"shader_set_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
             << "  \"shader_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
             << "  \"start_utc\":\"" << EscapeJson(startUtc) << "\",\n"
             << "  \"end_utc\":\"" << EscapeJson(endUtc) << "\",\n"
             << "  \"status\":\"" << status << "\",\n"
             << "  \"reason\":\"" << EscapeJson(reason) << "\",\n"
             << ResearchProvenance::ToJson(context.CurrentProvenance, 2) << "\n"
             << "}\n";
        ResearchProvenance::WriteTextFileAtomic(path, json.str());
    }

    void ApplyResolvedCountsToSummary(VoxelBenchmarkProfiler::BenchmarkSummary& summary,
                                      const AutomaticBenchmarkConfig& config,
                                      const BenchmarkConfigurationApplyResult& resolved)
    {
        summary.RequestedLabelCount =
            config.RequestedLabelCount != 0 ? config.RequestedLabelCount : config.TotalCount;
        summary.RequestedStaticBudget =
            config.RequestedStaticBudget != 0 ? config.RequestedStaticBudget : config.TotalCount;
        summary.RequestedDynamicBudget = config.RequestedDynamicBudget;
        summary.ResolvedConfigHash = resolved.ResolvedConfigHash;

        const bool hasResolvedCounts =
            resolved.ActualTotalCount != 0 ||
            resolved.ActualStaticCount != 0 ||
            resolved.ActualDynamicCount != 0;
        if (hasResolvedCounts)
        {
            summary.ActualStaticVoxelCount = resolved.ActualStaticCount;
            summary.ActualDynamicVoxelCount = resolved.ActualDynamicCount;
            summary.TotalVoxelCount =
                resolved.ActualTotalCount != 0
                    ? resolved.ActualTotalCount
                    : resolved.ActualStaticCount + resolved.ActualDynamicCount;
        }
        else if (summary.TotalVoxelCount == 0 &&
                 (summary.ActualStaticVoxelCount != 0 || summary.ActualDynamicVoxelCount != 0))
        {
            summary.TotalVoxelCount =
                summary.ActualStaticVoxelCount + summary.ActualDynamicVoxelCount;
        }
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
                                          const uint32_t seedOverride,
                                          const uint32_t repetitionOverride)
{
    StopAutomatic(context);

    automaticBenchmarkConfigs.clear();
    automaticBenchmarkSummaries.clear();
    executionManifestStates.clear();
    automaticBenchmarkIndex = 0;
    automaticBenchmarkStopRequested = false;
    automaticBenchmarkTerminalStatus = ResearchRunStatus::Invalid;
    activeSuite = suite;
    automaticBenchmarkConfigs = AutomaticBenchmarkRunner::BuildConfigs(suite, seedOverride, repetitionOverride);
    activeSuiteSeed = automaticBenchmarkConfigs.empty() ? seedOverride : automaticBenchmarkConfigs.front().RandomizationSeed;
    activeSuiteRunId = MakeRunId(suite, activeSuiteSeed);
    activeSuiteCreatedUtc = UtcTimestamp();
    activeSuiteStartUtc.clear();
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
    const auto gateFailure = BuildGateFailureReason(context, automaticBenchmarkConfigs);
    auto artifactContext = BuildArtifactContext(context, suite, activeSuiteRunId, activeSuiteSeed,
                                                warmupFrames, measuredFrames,
                                                automaticBenchmarkConfigs.size(),
                                                benchmarkDirectory,
                                                automaticBenchmarkStatusPath,
                                                automaticBenchmarkSummaryPath,
                                                gateFailure.empty() ? "PENDING" : "BLOCKED",
                                                gateFailure,
                                                activeSuiteCreatedUtc,
                                                gateFailure.empty() ? "" : activeSuiteCreatedUtc,
                                                gateFailure.empty() ? "" : activeSuiteCreatedUtc);
    ResearchArtifactWriter::WriteSuiteManifest(artifactContext, automaticBenchmarkConfigs);
    ResearchArtifactWriter::WriteEnvironment(artifactContext, context.BuildMetadata());
    ResearchArtifactWriter::WriteReproductionReadme(artifactContext);
    if (!gateFailure.empty())
    {
        WriteSuiteStatus(automaticBenchmarkStatusPath, suite, activeSuiteRunId, activeSuiteSeed,
                         warmupFrames, measuredFrames, automaticBenchmarkConfigs.size(),
                         "BLOCKED", gateFailure, context,
                         activeSuiteCreatedUtc, activeSuiteCreatedUtc, activeSuiteCreatedUtc);
        artifactContext.GateStatus = "BLOCKED";
        artifactContext.GateReason = gateFailure;
        ResearchArtifactWriter::WriteSuiteManifest(artifactContext, automaticBenchmarkConfigs);
        ResearchArtifactWriter::WriteReproductionReadme(artifactContext);
        ResearchArtifactWriter::WriteRunsCsv(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WriteRawFramesCsv(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WritePairedRunsCsv(artifactContext, automaticBenchmarkSummaries);
        ResearchArtifactWriter::WriteInvalidRecords(artifactContext, automaticBenchmarkSummaries);
        BenchmarkCsvWriter::WriteAutomaticSummary(automaticBenchmarkSummaryPath, automaticBenchmarkSummaries);
        automaticBenchmarkTerminalStatus = ResearchRunStatus::Blocked;
        context.Log(L"\nAutomatic voxel benchmark blocked: " + ToWide(gateFailure.c_str()));
        return false;
    }

    benchmarkVSyncWasEnabled = context.IsVSyncEnabled();
    if (benchmarkVSyncWasEnabled)
        context.SetVSync(false);

    automaticBenchmarkActive = true;
    activeSuiteStartUtc = activeSuiteStartUtc.empty() ? UtcTimestamp() : activeSuiteStartUtc;
    WriteSuiteStatus(automaticBenchmarkStatusPath, suite, activeSuiteRunId, activeSuiteSeed,
                     warmupFrames, measuredFrames, automaticBenchmarkConfigs.size(),
                     "RUNNING", "", context,
                     activeSuiteCreatedUtc, activeSuiteStartUtc, "");
    artifactContext.GateStatus = "RUNNING";
    artifactContext.GateReason.clear();
    artifactContext.StartUtc = activeSuiteStartUtc;
    artifactContext.EndUtc.clear();
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
            ApplyResolvedCountsToSummary(summary, config, BenchmarkConfigurationApplyResult{});
            for (const auto& state : executionManifestStates)
            {
                if (state.Config.ConfigId == config.ConfigId)
                {
                    ApplyResolvedCountsToSummary(summary, config, state.Resolved);
                    break;
                }
            }
            summary.SecondaryShare = config.SecondaryShare;
            summary.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
            summary.TemporalPolicy =
                config.TemporalInterval <= 1 ? "Full" : "Decimated";
            summary.Repetition = config.Repetition;
            const auto manifestPath = ExecutionManifestPath(benchmarkDirectory, config);
            const std::string finalStatus = summary.Valid ? "COMPLETE" : "INVALID";
            const std::string finalReason = summary.Valid
                                                ? ""
                                                : (!summary.ValidityReason.empty()
                                                       ? summary.ValidityReason
                                                       : summary.SkipReason);
            for (auto& state : executionManifestStates)
            {
                if (state.Path == manifestPath)
                {
                    state.Status = finalStatus;
                    state.Reason = finalReason;
                    if (state.EndUtc.empty())
                        state.EndUtc = UtcTimestamp();
                    WriteExecutionManifest(state.Path, state.Config, activeSuiteRunId,
                                           state.Resolved, context,
                                           state.Status.c_str(), state.Reason,
                                           state.StartUtc, state.EndUtc);
                    break;
                }
            }
        }
        automaticBenchmarkSummaries.push_back(summary);
        context.Log(L"\nFinished benchmark CSV: " + summary.CsvPath.wstring());
        ++automaticBenchmarkIndex;
    }

    if (context.Profiler.IsActive())
        return;

    if (automaticBenchmarkStopRequested || automaticBenchmarkIndex >= automaticBenchmarkConfigs.size())
    {
        automaticBenchmarkActive = false;
        automaticBenchmarkStopRequested = false;
        context.SetVSync(benchmarkVSyncWasEnabled);
        FinalizeAutomaticArtifacts(context, ResearchRunStatus::Complete, "");
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
        BenchmarkConfigurationApplyResult resolved{};
        resolved.Passed = false;
        resolved.ActualMode = "Skipped";
        resolved.Reason = "secondary hardware adapter unavailable";
        const auto manifestPath = ExecutionManifestPath(benchmarkDirectory, config);
        const auto timestamp = UtcTimestamp();
        WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context,
                               "INVALID", resolved.Reason, timestamp, timestamp);
        executionManifestStates.push_back({manifestPath, config, resolved, "INVALID", resolved.Reason,
                                           timestamp, timestamp});
        context.Log(L"\nSkipped benchmark " + ToWide(config.ModeName) +
            L" / " + ToWide(config.Preset) +
            L": secondary hardware adapter unavailable");
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = "Skipped";
        skipped.Preset = config.Preset;
        ApplyResolvedCountsToSummary(skipped, config, resolved);
        skipped.SecondaryShare = config.SecondaryShare;
        skipped.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
        skipped.TemporalPolicy = config.TemporalInterval <= 1 ? "Full" : "Decimated";
        skipped.PairId = config.PairId;
        skipped.SessionId = config.SessionId;
        skipped.BlockId = config.BlockId;
        skipped.Repetition = config.Repetition;
        skipped.Valid = false;
        skipped.SkipReason = resolved.Reason;
        automaticBenchmarkSummaries.push_back(skipped);
        ++automaticBenchmarkIndex;
        return;
    }
    if (requestsMultiGpu && !context.TwoAdapterVerificationPassed)
    {
        const auto reason = context.TwoAdapterVerificationReason.empty()
                                ? "two-adapter runtime verification has not passed for this build/adapter pair"
                                : context.TwoAdapterVerificationReason;
        BenchmarkConfigurationApplyResult resolved{};
        resolved.Passed = false;
        resolved.ActualMode = "Skipped";
        resolved.Reason = reason;
        const auto manifestPath = ExecutionManifestPath(benchmarkDirectory, config);
        const auto timestamp = UtcTimestamp();
        WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context,
                               "INVALID", resolved.Reason, timestamp, timestamp);
        executionManifestStates.push_back({manifestPath, config, resolved, "INVALID", resolved.Reason,
                                           timestamp, timestamp});
        context.Log(L"\nSkipped benchmark " + ToWide(config.ModeName) +
            L" / " + ToWide(config.Preset) +
            L": " + ToWide(reason.c_str()));
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = "Skipped";
        skipped.Preset = config.Preset;
        ApplyResolvedCountsToSummary(skipped, config, resolved);
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
        const uint32_t requestedStaticBudget =
            config.RequestedStaticBudget != 0 ? config.RequestedStaticBudget : config.TotalCount;
        ApplyBenchmarkVoxelCount(context, static_cast<int>(requestedStaticBudget));
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

    const auto manifestPath = ExecutionManifestPath(benchmarkDirectory, config);
    if (!resolved.Passed)
    {
        const auto timestamp = UtcTimestamp();
        WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context,
                               "INVALID", resolved.Reason, timestamp, timestamp);
        executionManifestStates.push_back({manifestPath, config, resolved, "INVALID", resolved.Reason,
                                           timestamp, timestamp});
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = resolved.ActualMode.empty() ? "Invalid" : resolved.ActualMode;
        skipped.Preset = config.Preset;
        ApplyResolvedCountsToSummary(skipped, config, resolved);
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
    const auto coverageIt = std::find_if(
        context.ValidationCoverage.begin(),
        context.ValidationCoverage.end(),
        [&](const BenchmarkValidationCoverage& coverage)
        {
            return coverage.Passed &&
                coverage.ModeName == config.ModeName &&
                coverage.ConfigHash == resolved.ResolvedConfigHash &&
                coverage.ProtocolHash == context.ValidationProtocolHash &&
                (!(config.TemporalInterval > 1 || config.SpatialLodEnabled) ||
                 coverage.CaseId.find("|fidelity|") != std::string::npos);
        });
    if (coverageIt == context.ValidationCoverage.end())
    {
        const std::string reason =
            "resolved benchmark config lacks exact matching PASS visual validation case";
        const auto timestamp = UtcTimestamp();
        WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context,
                               "INVALID", reason, timestamp, timestamp);
        executionManifestStates.push_back({manifestPath, config, resolved, "INVALID", reason,
                                           timestamp, timestamp});
        VoxelBenchmarkProfiler::BenchmarkSummary skipped{};
        skipped.RequestedMode = config.ModeName;
        skipped.ActualMode = resolved.ActualMode.empty() ? "Invalid" : resolved.ActualMode;
        skipped.Preset = config.Preset;
        ApplyResolvedCountsToSummary(skipped, config, resolved);
        skipped.SecondaryShare = config.SecondaryShare;
        skipped.SpatialLodPolicy = config.SpatialLodEnabled ? "ThreeLevel" : "Off";
        skipped.TemporalPolicy = config.TemporalInterval <= 1 ? "Full" : "Decimated";
        skipped.PairId = config.PairId;
        skipped.SessionId = config.SessionId;
        skipped.BlockId = config.BlockId;
        skipped.Repetition = config.Repetition;
        skipped.RepetitionCount = config.RepetitionCount;
        skipped.Valid = false;
        skipped.SkipReason = reason;
        automaticBenchmarkSummaries.push_back(skipped);
        ++automaticBenchmarkIndex;
        return;
    }
    const auto executionStartUtc = UtcTimestamp();
    WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context, "RUNNING", "",
                           executionStartUtc, "");
    executionManifestStates.push_back({manifestPath, config, resolved, "RUNNING", "",
                                       executionStartUtc, ""});

    const std::string fileName = "VoxelBenchmark_" + std::string(config.ModeName) + "_" +
        config.Preset + "_static_budget_label" + std::to_string(config.RequestedLabelCount) +
        "_static" + std::to_string(config.RequestedStaticBudget) +
        "_dynamic" + std::to_string(config.RequestedDynamicBudget) +
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
        const std::string reason = "profiler failed to start benchmark execution";
        const auto timestamp = UtcTimestamp();
        WriteExecutionManifest(manifestPath, config, activeSuiteRunId, resolved, context, "INVALID", reason,
                               executionStartUtc, timestamp);
        if (!executionManifestStates.empty())
        {
            auto& state = executionManifestStates.back();
            if (state.Path == manifestPath)
            {
                state.Status = "INVALID";
                state.Reason = reason;
                state.EndUtc = timestamp;
            }
        }
        context.Log(L"\nFailed to start benchmark " + ToWide(config.ModeName));
        ++automaticBenchmarkIndex;
        return;
    }

    context.Log(L"\nStarted benchmark: " + ToWide(config.ModeName) +
        L" / " + ToWide(config.Preset) +
        L" / requested static " + std::to_wstring(config.RequestedStaticBudget) +
        L" / requested dynamic " + std::to_wstring(config.RequestedDynamicBudget) +
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
                                                "",
                                                activeSuiteCreatedUtc,
                                                activeSuiteStartUtc,
                                                "");
    ResearchArtifactWriter::WriteRunsCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteRawFramesCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WritePairedRunsCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteInvalidRecords(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteTelemetryCsv(artifactContext, automaticBenchmarkSummaries);

    if (automaticBenchmarkSummaries.empty())
        return;

    if (BenchmarkCsvWriter::WriteAutomaticSummary(automaticBenchmarkSummaryPath, automaticBenchmarkSummaries))
        context.Log(L"\nAutomatic benchmark summary written: " + automaticBenchmarkSummaryPath.wstring());
}

void BenchmarkController::FinalizeAutomaticArtifacts(const BenchmarkControllerContext& context,
                                                     const ResearchRunStatus requestedStatus,
                                                     const std::string& reason)
{
    const uint32_t warmupFrames = automaticBenchmarkConfigs.empty()
                                      ? 0
                                      : automaticBenchmarkConfigs.front().WarmupFrameCount;
    const uint32_t measuredFrames = automaticBenchmarkConfigs.empty()
                                        ? 0
                                        : automaticBenchmarkConfigs.front().MeasuredFrameCount;

    ResearchRunStatus finalStatus = requestedStatus;
    std::string finalReason = reason;

    const auto endUtc = UtcTimestamp();

    for (auto& state : executionManifestStates)
    {
        if (state.Status == "RUNNING")
        {
            state.Status = "INTERRUPTED";
            state.Reason = "execution did not complete before suite finalization";
            finalStatus = ResearchRunStatus::Interrupted;
        }
        if (state.EndUtc.empty() && state.Status != "RUNNING" && state.Status != "PENDING")
            state.EndUtc = endUtc;
        WriteExecutionManifest(state.Path, state.Config, activeSuiteRunId, state.Resolved, context,
                               state.Status.c_str(), state.Reason,
                               state.StartUtc, state.EndUtc);
    }

    if (requestedStatus == ResearchRunStatus::Complete)
    {
        ResearchStateValidationInput validation{};
        validation.Suite = activeSuite;
        validation.Configs = automaticBenchmarkConfigs;
        validation.Summaries = automaticBenchmarkSummaries;
        for (const auto& state : executionManifestStates)
        {
            validation.ExecutionConfigIds.push_back(state.Config.ConfigId);
            validation.ExecutionStatuses.push_back(state.Status);
            validation.ExecutionWarmupFrames.push_back(state.Config.WarmupFrameCount);
            validation.ExecutionMeasuredFrames.push_back(state.Config.MeasuredFrameCount);
            validation.ExecutionRepetitions.push_back(state.Config.Repetition);
        }
        const auto validationResult = ResearchProvenance::ValidateCompleteSuite(
            validation, warmupFrames, measuredFrames);
        finalStatus = validationResult.Status;
        if (!validationResult.Reason.empty())
            finalReason = validationResult.Reason;
    }

    automaticBenchmarkTerminalStatus = finalStatus;

    WriteSuiteStatus(automaticBenchmarkStatusPath, activeSuite, activeSuiteRunId, activeSuiteSeed,
                     warmupFrames, measuredFrames, automaticBenchmarkConfigs.size(),
                     ResearchProvenance::StatusName(finalStatus).c_str(), finalReason, context,
                     activeSuiteCreatedUtc,
                     activeSuiteStartUtc.empty() ? activeSuiteCreatedUtc : activeSuiteStartUtc,
                     endUtc);

    auto artifactContext = BuildArtifactContext(context, activeSuite, activeSuiteRunId, activeSuiteSeed,
                                                warmupFrames, measuredFrames,
                                                automaticBenchmarkConfigs.size(),
                                                benchmarkDirectory,
                                                automaticBenchmarkStatusPath,
                                                automaticBenchmarkSummaryPath,
                                                ResearchProvenance::StatusName(finalStatus),
                                                finalReason,
                                                activeSuiteCreatedUtc,
                                                activeSuiteStartUtc.empty() ? activeSuiteCreatedUtc : activeSuiteStartUtc,
                                                endUtc);

    ResearchArtifactWriter::WriteSuiteManifest(artifactContext, automaticBenchmarkConfigs);
    ResearchArtifactWriter::WriteEnvironment(artifactContext, context.BuildMetadata());
    ResearchArtifactWriter::WriteReproductionReadme(artifactContext);
    ResearchArtifactWriter::WriteRunsCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteRawFramesCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WritePairedRunsCsv(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteInvalidRecords(artifactContext, automaticBenchmarkSummaries);
    ResearchArtifactWriter::WriteTelemetryCsv(artifactContext, automaticBenchmarkSummaries);

    if (BenchmarkCsvWriter::WriteAutomaticSummary(automaticBenchmarkSummaryPath, automaticBenchmarkSummaries))
        context.Log(L"\nAutomatic benchmark summary written: " + automaticBenchmarkSummaryPath.wstring());
}
