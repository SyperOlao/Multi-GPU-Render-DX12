#include "Source/Validation/VoxelVisualValidationRunner.h"

#include "Source/Benchmark/ResearchProvenance.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace
{
    std::string BoolText(const bool value)
    {
        return value ? "true" : "false";
    }

    std::string ModeName(const VoxelExecutionMode mode)
    {
        switch (mode)
        {
        case VoxelExecutionMode::SingleGpuFull:
            return "SingleGpuFull";
        case VoxelExecutionMode::MultiGpuFull:
            return "MultiGpuFull";
        case VoxelExecutionMode::SingleGpuTemporalDecimation:
            return "SingleGpuTemporalDecimation";
        case VoxelExecutionMode::MultiGpuTemporalDecimation:
            return "MultiGpuTemporalDecimation";
        default:
            return "Unknown";
        }
    }

    std::string LodName(const VoxelSpatialLodMode mode)
    {
        return mode == VoxelSpatialLodMode::ThreeLevel ? "ThreeLevel" : "Off";
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

    std::string EscapeCsv(const std::string& value)
    {
        if (value.find_first_of(",\"\n\r") == std::string::npos)
            return value;

        std::string escaped = "\"";
        for (const char ch : value)
        {
            if (ch == '"')
                escaped += "\"\"";
            else
                escaped.push_back(ch);
        }
        escaped += '"';
        return escaped;
    }

    std::string HashHex(const uint64_t hash)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
        return stream.str();
    }

    std::string MatrixCsv(const float values[16])
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(6);
        for (uint32_t i = 0; i < 16; ++i)
        {
            if (i != 0)
                stream << ';';
            stream << values[i];
        }
        return stream.str();
    }

    void WriteJsonMatrix(std::ofstream& json, const char* name, const float values[16], const char* suffix)
    {
        json << "    \"" << name << "\": [";
        for (uint32_t i = 0; i < 16; ++i)
        {
            if (i != 0)
                json << ", ";
            json << values[i];
        }
        json << "]" << suffix << "\n";
    }

    void AppendJsonMatrix(std::ostringstream& json, const char* name, const float values[16], const char* suffix)
    {
        json << "\"" << name << "\":[";
        for (uint32_t i = 0; i < 16; ++i)
        {
            if (i != 0)
                json << ",";
            json << values[i];
        }
        json << "]" << suffix;
    }

    const VoxelVisualValidationComparisonInput* FindComparison(
        const std::vector<VoxelVisualValidationComparisonInput>& comparisons,
        const std::string& caseId)
    {
        const auto it = std::find_if(comparisons.begin(), comparisons.end(),
                                     [&](const auto& input) { return input.CaseId == caseId; });
        return it == comparisons.end() ? nullptr : &*it;
    }

    std::string PathString(const std::filesystem::path& path)
    {
        return path.empty() ? "" : path.string();
    }

    void AccumulateComparison(const VoxelVisualValidationComparisonInput& comparison,
                              const VoxelVisualValidationTolerances& tolerances,
                              VoxelVisualValidationCaseResult& result)
    {
        double rgbAbs = 0.0;
        double rgbSq = 0.0;
        double alphaAbs = 0.0;
        double depthAbs = 0.0;
        double depthSq = 0.0;
        double depthRel = 0.0;
        double maxRgb = 0.0;
        double maxDepth = 0.0;

        for (const auto& tile : comparison.TileStats)
        {
            result.ComparedPixelCount += tile.PixelCount;
            result.ForegroundUnionCount += tile.ForegroundUnionCount;
            result.ForegroundIntersectionCount += tile.ForegroundIntersectionCount;
            result.ColorMismatchCount += tile.ColorMismatchCount;
            result.DepthMismatchCount += tile.DepthMismatchCount;
            result.CombinedMismatchCount += tile.CombinedMismatchCount;
            result.CoverageMismatchCount += tile.CoverageMismatchCount;
            rgbAbs += tile.RgbAbsoluteErrorSum;
            rgbSq += tile.RgbSquaredErrorSum;
            alphaAbs += tile.AlphaAbsoluteErrorSum;
            depthAbs += tile.DepthAbsoluteErrorSum;
            depthSq += tile.DepthSquaredErrorSum;
            depthRel += tile.DepthRelativeErrorSum;
            maxRgb = std::max<double>(maxRgb, tile.RgbMaxAbsoluteError);
            maxDepth = std::max<double>(maxDepth, tile.DepthMaxAbsoluteError);
        }

        if (result.ComparedPixelCount == 0)
        {
            result.Passed = false;
            result.Blocked = true;
            result.Status = "BLOCKED";
            result.Reason = "validation tile statistics reported zero compared pixels";
            return;
        }

        const double pixelCount = std::max<double>(1.0, static_cast<double>(result.ComparedPixelCount));
        const double foregroundIntersection =
            std::max<double>(1.0, static_cast<double>(result.ForegroundIntersectionCount));
        result.ColorMAE = rgbAbs / pixelCount;
        result.ColorRMSE = std::sqrt(rgbSq / pixelCount);
        result.ColorPSNR = result.ColorRMSE <= std::numeric_limits<double>::epsilon()
                               ? std::numeric_limits<double>::infinity()
                               : 20.0 * std::log10(1.0 / result.ColorRMSE);
        result.MaxColorError = maxRgb;
        result.AlphaMAE = alphaAbs / pixelCount;
        result.DepthMAE = depthAbs / foregroundIntersection;
        result.DepthRMSE = std::sqrt(depthSq / foregroundIntersection);
        result.DepthRelativeMAE = depthRel / foregroundIntersection;
        result.MaxDepthError = maxDepth;
        result.ColorMismatchPercent = 100.0 * static_cast<double>(result.ColorMismatchCount) / pixelCount;
        result.DepthMismatchPercent = 100.0 * static_cast<double>(result.DepthMismatchCount) / foregroundIntersection;
        result.CombinedMismatchPercent = 100.0 * static_cast<double>(result.CombinedMismatchCount) / pixelCount;
        result.CoverageMismatchPercent = 100.0 * static_cast<double>(result.CoverageMismatchCount) / pixelCount;

        result.Passed =
            result.ColorMismatchPercent <= tolerances.MaxColorMismatchPercent &&
            result.DepthMismatchPercent <= tolerances.MaxDepthMismatchPercent &&
            result.CombinedMismatchPercent <= tolerances.MaxCombinedMismatchPercent &&
            result.CoverageMismatchPercent <= tolerances.MaxCombinedMismatchPercent;
        result.Blocked = false;
        result.Status = result.Passed ? "PASS" : "FAIL";
        if (!result.Passed)
            result.Reason = "numeric color/depth metrics exceeded validation tolerances";
    }
}

static_assert(sizeof(VoxelValidationTileStats) == 64,
              "VoxelValidationTileStats must match Shaders/VoxelValidationCompare.hlsl");

std::vector<VoxelVisualValidationCase> VoxelVisualValidationConfig::DefaultCases()
{
    std::vector<VoxelVisualValidationCase> cases;
    auto append = [&](const char* caseId,
                      const VoxelExecutionMode singleMode,
                      const VoxelExecutionMode multiMode,
                      const bool lod,
                      const uint32_t interval)
    {
        VoxelVisualValidationCase validationCase{};
        validationCase.CaseId = caseId;
        validationCase.ConfigKey = caseId;
        validationCase.ModeFamily = interval > 1 ? "Temporal" : "Full";
        validationCase.SingleMode = singleMode;
        validationCase.MultiMode = multiMode;
        validationCase.ReferenceMode = singleMode;
        validationCase.CandidateMode = multiMode;
        validationCase.SpatialLodEnabled = lod;
        validationCase.TemporalInterval = interval;
        validationCase.CheckpointId = "legacy_default";
        cases.push_back(std::move(validationCase));
    };
    append("full_lod_off",
           VoxelExecutionMode::SingleGpuFull,
           VoxelExecutionMode::MultiGpuFull,
           false,
           1u);
    append("full_lod_on",
           VoxelExecutionMode::SingleGpuFull,
           VoxelExecutionMode::MultiGpuFull,
           true,
           1u);
    append("temporal_lod_off",
           VoxelExecutionMode::SingleGpuTemporalDecimation,
           VoxelExecutionMode::MultiGpuTemporalDecimation,
           false,
           2u);
    append("temporal_lod_on",
           VoxelExecutionMode::SingleGpuTemporalDecimation,
           VoxelExecutionMode::MultiGpuTemporalDecimation,
           true,
           2u);
    return cases;
}

std::string VoxelVisualValidationRunner::BuildCanonicalProtocolJson(
    const VoxelVisualValidationConfig& config)
{
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::fixed << std::setprecision(8);
    const auto& snapshot = config.Snapshot;
    json << "{";
    json << "\"generator_version\":\"" << EscapeJson(snapshot.GeneratorVersion) << "\",";
    json << "\"protocol_version\":\"" << EscapeJson(snapshot.ProtocolVersion) << "\",";
    json << "\"runtime\":{";
    json << "\"adapter_pair\":\"" << EscapeJson(snapshot.AdapterPairIdentity) << "\",";
    json << "\"build_sha256\":\"" << EscapeJson(snapshot.BuildHash) << "\",";
    json << "\"color_format\":\"" << EscapeJson(snapshot.ColorFormat) << "\",";
    json << "\"depth_format\":\"" << EscapeJson(snapshot.LinearDepthFormat) << "\",";
    json << "\"driver_primary\":\"" << EscapeJson(snapshot.PrimaryDriverVersion) << "\",";
    json << "\"driver_secondary\":\"" << EscapeJson(snapshot.SecondaryDriverVersion) << "\",";
    json << "\"render_height\":" << snapshot.RenderHeight << ",";
    json << "\"render_width\":" << snapshot.RenderWidth << ",";
    json << "\"sample_count\":" << snapshot.SampleCount << ",";
    json << "\"shader_set_sha256\":\"" << EscapeJson(snapshot.ShaderSetHash) << "\"},";
    json << "\"tolerances\":{";
    json << "\"color\":" << config.Tolerances.ColorTolerance << ",";
    json << "\"depth\":" << config.Tolerances.DepthTolerance << ",";
    json << "\"max_color_mismatch_percent\":" << config.Tolerances.MaxColorMismatchPercent << ",";
    json << "\"max_combined_mismatch_percent\":" << config.Tolerances.MaxCombinedMismatchPercent << ",";
    json << "\"max_depth_mismatch_percent\":" << config.Tolerances.MaxDepthMismatchPercent << ",";
    json << "\"valid_depth_max\":" << config.Tolerances.ValidDepthMax << "},";
    json << "\"cases\":[";
    for (size_t i = 0; i < config.Cases.size(); ++i)
    {
        const auto& validationCase = config.Cases[i];
        if (i != 0)
            json << ",";
        json << "{";
        json << "\"actual_dynamic_count\":" << validationCase.ActualDynamicCount << ",";
        json << "\"actual_static_count\":" << validationCase.ActualStaticCount << ",";
        json << "\"actual_total_count\":" << validationCase.ActualTotalCount << ",";
        json << "\"camera_hash\":\"" << EscapeJson(validationCase.CameraHash) << "\",";
        json << "\"camera_mode\":\"" << EscapeJson(validationCase.CameraMode) << "\",";
        json << "\"case_id\":\"" << EscapeJson(validationCase.CaseId) << "\",";
        json << "\"candidate_config_hash\":\"" << EscapeJson(validationCase.CandidateConfigHash) << "\",";
        json << "\"candidate_mode\":\"" << EscapeJson(ModeName(validationCase.CandidateMode)) << "\",";
        json << "\"checkpoint_id\":\"" << EscapeJson(validationCase.CheckpointId) << "\",";
        json << "\"checkpoint_index\":" << validationCase.CheckpointIndex << ",";
        json << "\"color_format\":\"" << EscapeJson(validationCase.ColorFormat) << "\",";
        json << "\"config_hash\":\"" << EscapeJson(validationCase.ConfigHash) << "\",";
        json << "\"config_key\":\"" << EscapeJson(validationCase.ConfigKey) << "\",";
        json << "\"depth_format\":\"" << EscapeJson(validationCase.LinearDepthFormat) << "\",";
        json << "\"fixed_step_count\":" << validationCase.FixedStepCount << ",";
        json << "\"kind\":\"" << EscapeJson(validationCase.ValidationKind) << "\",";
        json << "\"lod_enabled\":" << BoolText(validationCase.SpatialLodEnabled) << ",";
        json << "\"mode_family\":\"" << EscapeJson(validationCase.ModeFamily) << "\",";
        json << "\"preset\":\"" << EscapeJson(validationCase.Preset) << "\",";
        json << "\"randomization_seed\":" << validationCase.RandomizationSeed << ",";
        json << "\"reference_config_hash\":\"" << EscapeJson(validationCase.ReferenceConfigHash) << "\",";
        json << "\"reference_mode\":\"" << EscapeJson(ModeName(validationCase.ReferenceMode)) << "\",";
        json << "\"render_height\":" << validationCase.RenderHeight << ",";
        json << "\"render_width\":" << validationCase.RenderWidth << ",";
        json << "\"requested_dynamic_budget\":" << validationCase.RequestedDynamicBudget << ",";
        json << "\"requested_label_count\":" << validationCase.RequestedLabelCount << ",";
        json << "\"requested_static_budget\":" << validationCase.RequestedStaticBudget << ",";
        json << "\"sample_count\":" << validationCase.SampleCount << ",";
        json << "\"secondary_share\":" << validationCase.SecondaryShare << ",";
        json << "\"suite\":\"" << EscapeJson(validationCase.Suite) << "\",";
        json << "\"temporal_interval\":" << validationCase.TemporalInterval << ",";
        AppendJsonMatrix(json, "view_matrix", validationCase.View, ",");
        AppendJsonMatrix(json, "projection_matrix", validationCase.Projection, "");
        json << "}";
    }
    json << "]}";
    return json.str();
}

std::string VoxelVisualValidationRunner::BuildProtocolHash(
    const VoxelVisualValidationConfig& config)
{
    return ResearchProvenance::Sha256Hex(BuildCanonicalProtocolJson(config));
}

uint64_t VoxelVisualValidationRunner::ComputeSnapshotHash(const VoxelVisualValidationSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(8)
           << "{\"adapter_pair\":\"" << EscapeJson(snapshot.AdapterPairIdentity)
           << "\",\"actual_dynamic_count\":" << snapshot.ActualDynamicCount
           << ",\"actual_static_count\":" << snapshot.ActualStaticCount
           << ",\"background\":\"" << EscapeJson(snapshot.Background)
           << "\",\"build_hash\":\"" << EscapeJson(snapshot.BuildHash)
           << "\",\"chunk\":[" << snapshot.ChunkSize.Width << ',' << snapshot.ChunkSize.Height
           << ',' << snapshot.ChunkSize.Depth << ']'
           << ",\"color_format\":\"" << EscapeJson(snapshot.ColorFormat)
           << "\",\"dynamic_seed\":" << snapshot.DynamicSeed
           << ",\"dynamic_shadows\":" << (snapshot.DynamicShadowsEnabled ? "true" : "false")
           << ",\"dynamic_voxel_size\":" << snapshot.DynamicVoxelSize
           << ",\"far_z\":" << snapshot.FarZ
           << ",\"fixed_delta_time\":" << snapshot.FixedDeltaTime
           << ",\"fixed_step_count\":" << snapshot.FixedStepCount
           << ",\"generator_version\":\"" << EscapeJson(snapshot.GeneratorVersion)
           << "\",\"lighting\":\"" << EscapeJson(snapshot.LightingPreset)
           << "\",\"linear_depth_format\":\"" << EscapeJson(snapshot.LinearDepthFormat)
           << "\",\"lod\":[" << static_cast<uint32_t>(snapshot.SpatialLod.Mode)
           << ',' << snapshot.SpatialLod.Lod0Distance
           << ',' << snapshot.SpatialLod.Lod1Distance
           << ',' << snapshot.SpatialLod.Hysteresis << ']'
           << ",\"near_z\":" << snapshot.NearZ
           << ",\"partition_strategy\":" << static_cast<uint32_t>(snapshot.PartitionStrategy)
           << ",\"projection\":[";
    for (uint32_t i = 0; i < 16; ++i)
        stream << (i == 0 ? "" : ",") << snapshot.Projection[i];
    stream << "],\"protocol_version\":\"" << EscapeJson(snapshot.ProtocolVersion)
           << "\",\"render_height\":" << snapshot.RenderHeight
           << ",\"render_width\":" << snapshot.RenderWidth
           << ",\"requested_dynamic_count\":" << snapshot.RequestedDynamicCount
           << ",\"requested_execution_mode\":" << static_cast<uint32_t>(snapshot.RequestedExecutionMode)
           << ",\"requested_static_count\":" << snapshot.RequestedStaticCount
           << ",\"schema_version\":" << snapshot.SchemaVersion
           << ",\"scene_preset\":\"" << EscapeJson(snapshot.ScenePreset)
           << "\",\"secondary_share\":" << snapshot.SecondaryShare
           << ",\"shader_set_hash\":\"" << EscapeJson(snapshot.ShaderSetHash)
           << "\",\"static_seed\":" << snapshot.StaticSeed
           << ",\"static_voxel_size\":" << snapshot.StaticVoxelSize
           << ",\"temporal_interval\":" << snapshot.TemporalInterval
           << ",\"temporal_policy\":" << static_cast<uint32_t>(snapshot.TemporalPolicy)
           << ",\"total_voxel_count\":" << snapshot.TotalVoxelCount
           << ",\"view\":[";
    for (uint32_t i = 0; i < 16; ++i)
        stream << (i == 0 ? "" : ",") << snapshot.View[i];
    stream << "],\"warmup_step_count\":" << snapshot.WarmupStepCount
           << ",\"workload_profile\":\"" << EscapeJson(snapshot.WorkloadProfile) << "\"}";
    const auto digest = ResearchProvenance::Sha256Hex(stream.str());
    return std::stoull(digest.substr(0, 16), nullptr, 16);
}

VoxelVisualValidationMetrics VoxelVisualValidationRunner::RunDeterministicSuite(
    const VoxelVisualValidationConfig& config,
    const std::filesystem::path& outputDirectory) const
{
    std::filesystem::create_directories(outputDirectory);

    VoxelVisualValidationConfig resolvedConfig = config;
    auto& snapshot = resolvedConfig.Snapshot;
    if (snapshot.ShaderSetHash == "unknown" && snapshot.ShaderHash != "unknown")
        snapshot.ShaderSetHash = snapshot.ShaderHash;
    if (snapshot.ShaderHash == "unknown" && snapshot.ShaderSetHash != "unknown")
        snapshot.ShaderHash = snapshot.ShaderSetHash;
    snapshot.SnapshotHash = snapshot.SnapshotHash != 0
                                ? snapshot.SnapshotHash
                                : ComputeSnapshotHash(snapshot);
    if (snapshot.ValidationRunId.empty())
        snapshot.ValidationRunId = HashHex(snapshot.SnapshotHash);

    VoxelVisualValidationMetrics metrics{};
    metrics.HasResult = true;
    metrics.Passed = false;
    metrics.Blocked = false;
    metrics.ValidationRunId = snapshot.ValidationRunId;
    metrics.SnapshotHash = snapshot.SnapshotHash;
    metrics.FailReason.clear();
    metrics.CsvPath = outputDirectory / "voxel_visual_validation.csv";
    metrics.JsonPath = outputDirectory / "voxel_visual_validation.json";

    const auto cases = resolvedConfig.Cases.empty()
                           ? VoxelVisualValidationConfig::DefaultCases()
                           : resolvedConfig.Cases;
    metrics.CaseResults.reserve(cases.size());
    for (const auto& validationCase : cases)
    {
        VoxelVisualValidationCaseResult result{};
        result.CaseId = validationCase.CaseId;
        result.ValidationRunId = snapshot.ValidationRunId;
        result.SnapshotHash = snapshot.SnapshotHash;
        result.ValidationKind = validationCase.ValidationKind;
        result.ModeFamily = validationCase.ModeFamily;
        result.ConfigKey = validationCase.ConfigKey;
        result.CheckpointId = validationCase.CheckpointId;
        result.ProtocolHash = validationCase.ProtocolHash.empty()
                                  ? snapshot.ProtocolHash
                                  : validationCase.ProtocolHash;
        result.ReferenceConfigHash = validationCase.ReferenceConfigHash;
        result.CandidateConfigHash = validationCase.CandidateConfigHash;
        result.RequestedSingleMode = validationCase.SingleMode;
        result.ActualSingleMode = validationCase.SingleMode;
        result.RequestedMultiMode = validationCase.MultiMode;
        result.ActualMultiMode = VoxelExecutionMode::SingleGpuFull;
        result.RequestedReferenceMode = validationCase.ReferenceMode;
        result.ActualReferenceMode = validationCase.ReferenceMode;
        result.RequestedCandidateMode = validationCase.CandidateMode;
        result.ActualCandidateMode = VoxelExecutionMode::SingleGpuFull;
        result.SpatialLodMode = validationCase.SpatialLodEnabled
                                    ? VoxelSpatialLodMode::ThreeLevel
                                    : VoxelSpatialLodMode::Off;
        result.TemporalInterval = validationCase.TemporalInterval;
        result.TotalVoxelCount = snapshot.TotalVoxelCount;
        result.ActualStaticCount = snapshot.ActualStaticCount;
        result.ActualDynamicCount = snapshot.ActualDynamicCount;
        if (const auto* comparison = FindComparison(resolvedConfig.CompletedComparisons, validationCase.CaseId))
        {
            result.ActualSingleMode = comparison->ActualSingleMode;
            result.ActualMultiMode = comparison->ActualMultiMode;
            result.ActualReferenceMode = comparison->ActualReferenceMode;
            result.ActualCandidateMode = comparison->ActualCandidateMode;
            result.ConfigHash = comparison->ConfigHash;
            result.CameraHash = comparison->CameraHash;
            result.AdapterPairIdentity = comparison->AdapterPairIdentity;
            result.SingleColorReferencePath = comparison->SingleColorReferencePath;
            result.SingleDepthReferencePath = comparison->SingleDepthReferencePath;
            result.MultiColorReferencePath = comparison->MultiColorReferencePath;
            result.MultiDepthReferencePath = comparison->MultiDepthReferencePath;
            result.DiffReferencePath = comparison->DiffReferencePath;

            const bool hasCompareEvidence =
                comparison->CaptureAvailable &&
                comparison->CompareShaderDispatched &&
                comparison->ReadbackComplete &&
                !comparison->TileStats.empty();
            if (hasCompareEvidence)
            {
                AccumulateComparison(*comparison, resolvedConfig.Tolerances, result);
                if (!comparison->BlockedReason.empty())
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = comparison->BlockedReason;
                }
                else if (comparison->ProtocolHash != result.ProtocolHash)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "validation comparison protocol hash did not match case protocol";
                }
                else if (comparison->ConfigHash != validationCase.ConfigHash ||
                         comparison->ReferenceConfigHash != validationCase.ReferenceConfigHash ||
                         comparison->CandidateConfigHash != validationCase.CandidateConfigHash)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "validation comparison config hash did not match resolved case config";
                }
                else if (comparison->CameraHash != validationCase.CameraHash)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "validation comparison camera hash did not match resolved case camera";
                }
                else if (comparison->RenderWidth != validationCase.RenderWidth ||
                         comparison->RenderHeight != validationCase.RenderHeight ||
                         comparison->SampleCount != validationCase.SampleCount ||
                         comparison->ColorFormat != validationCase.ColorFormat ||
                         comparison->LinearDepthFormat != validationCase.LinearDepthFormat)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "validation comparison render target dimensions/formats did not match protocol";
                }
                else if (comparison->ActualReferenceMode != validationCase.ReferenceMode)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "validation reference actual mode did not match requested mode";
                }
                else if (comparison->ActualCandidateMode != validationCase.CandidateMode)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "validation candidate actual mode did not match requested mode";
                }
                else if (comparison->ActualSingleMode != validationCase.SingleMode)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "deterministic Single path actual mode did not match requested mode";
                }
                else if (comparison->ActualMultiMode != validationCase.MultiMode)
                {
                    result.Passed = false;
                    result.Blocked = true;
                    result.Status = "BLOCKED";
                    result.Reason = "deterministic Multi path actual mode did not match requested mode";
                }
            }
            else if (!comparison->BlockedReason.empty())
                result.Reason = comparison->BlockedReason;
            else if (!comparison->CaptureAvailable)
                result.Reason = "deterministic Single/Multi color/depth capture was not supplied";
            else if (!comparison->CompareShaderDispatched)
                result.Reason = "VoxelValidationCompare.hlsl dispatch evidence was not supplied";
            else if (!comparison->ReadbackComplete)
                result.Reason = "validation tile statistics readback is not complete";
            else
                result.Reason = "validation tile statistics are empty";
        }
        else
        {
            result.Reason =
                "GPU color/depth comparison input was not supplied for this validation case";
        }

        if (result.Status.empty() || result.Status == "BLOCKED")
        {
            result.Status = "BLOCKED";
            result.Blocked = true;
            result.Passed = false;
        }
        metrics.CaseResults.push_back(std::move(result));
    }

    metrics.Blocked = std::any_of(metrics.CaseResults.begin(), metrics.CaseResults.end(),
                                  [](const auto& result) { return result.Blocked; });
    metrics.Passed = !metrics.Blocked &&
        std::all_of(metrics.CaseResults.begin(), metrics.CaseResults.end(),
                    [](const auto& result) { return result.Passed; });
    if (!metrics.Passed)
    {
        const auto failed = std::find_if(metrics.CaseResults.begin(), metrics.CaseResults.end(),
                                         [](const auto& result) { return !result.Passed; });
        metrics.FailReason = failed != metrics.CaseResults.end() ? failed->Reason : "visual validation failed";
    }
    if (!metrics.CaseResults.empty())
    {
        double colorMae = 0.0;
        double colorRmse = 0.0;
        double psnr = 0.0;
        double depthRmse = 0.0;
        double colorMismatch = 0.0;
        double depthMismatch = 0.0;
        uint32_t measuredCases = 0;
        for (const auto& result : metrics.CaseResults)
        {
            if (result.Blocked)
                continue;
            ++measuredCases;
            colorMae += result.ColorMAE;
            colorRmse += result.ColorRMSE;
            psnr += std::isfinite(result.ColorPSNR) ? result.ColorPSNR : 0.0;
            depthRmse += result.DepthRMSE;
            colorMismatch += result.ColorMismatchPercent;
            depthMismatch += result.DepthMismatchPercent;
            metrics.MaxColorError = std::max(metrics.MaxColorError, result.MaxColorError);
        }
        if (measuredCases > 0)
        {
            const double denom = static_cast<double>(measuredCases);
            metrics.ColorMAE = colorMae / denom;
            metrics.ColorRMSE = colorRmse / denom;
            metrics.ColorPSNR = psnr / denom;
            metrics.ColorMismatchPercent = colorMismatch / denom;
            metrics.DepthRMSE = depthRmse / denom;
            metrics.DepthMismatchPercent = depthMismatch / denom;
        }
    }

    ExportCsv(resolvedConfig, metrics, metrics.CsvPath);
    ExportJson(resolvedConfig, metrics, metrics.JsonPath);
    return metrics;
}

void VoxelVisualValidationRunner::ExportCsv(
    const VoxelVisualValidationConfig& config,
    const VoxelVisualValidationMetrics& metrics,
    const std::filesystem::path& path)
{
    std::ofstream csv(path, std::ios::out | std::ios::trunc);
    if (!csv.is_open())
        throw std::runtime_error("Failed to open visual validation CSV output");

    const auto& snapshot = config.Snapshot;
    csv.imbue(std::locale::classic());
    csv << "validation_run_id,snapshot_hash,case_id,validation_kind,mode_family,config_key,checkpoint_id,"
        << "protocol_hash,reference_config_hash,candidate_config_hash,status,passed,blocked,requested_single_mode,"
        << "actual_single_mode,requested_multi_mode,actual_multi_mode,requested_reference_mode,"
        << "actual_reference_mode,requested_candidate_mode,actual_candidate_mode,scene_preset,profile,total_voxels,"
        << "actual_static_voxels,actual_dynamic_voxels,render_width,render_height,color_format,"
        << "linear_depth_format,fixed_dt,fixed_step_count,warmup_step_count,spatial_lod,lod0_distance,"
        << "lod1_distance,lod_hysteresis,temporal_interval,secondary_share,color_tolerance,"
        << "depth_tolerance,max_color_mismatch_percent,max_depth_mismatch_percent,compared_pixel_count,"
        << "foreground_union_count,foreground_intersection_count,color_mae,color_rmse,psnr,"
        << "max_color_error,alpha_mae,depth_mae,depth_rmse,depth_relative_mae,max_depth_error,"
        << "color_mismatch_count,color_mismatch_percent,depth_mismatch_count,depth_mismatch_percent,"
        << "combined_mismatch_count,combined_mismatch_percent,coverage_mismatch_count,"
        << "coverage_mismatch_percent,config_hash,camera_hash,adapter_pair,single_color_reference,"
        << "single_depth_reference,multi_color_reference,multi_depth_reference,diff_reference,"
        << "build_hash,shader_set_hash,shader_hash,view_matrix,projection_matrix,reason\n";

    for (const auto& result : metrics.CaseResults)
    {
        csv << EscapeCsv(metrics.ValidationRunId) << ','
            << HashHex(result.SnapshotHash) << ','
            << EscapeCsv(result.CaseId) << ','
            << EscapeCsv(result.ValidationKind) << ','
            << EscapeCsv(result.ModeFamily) << ','
            << EscapeCsv(result.ConfigKey) << ','
            << EscapeCsv(result.CheckpointId) << ','
            << EscapeCsv(result.ProtocolHash) << ','
            << EscapeCsv(result.ReferenceConfigHash) << ','
            << EscapeCsv(result.CandidateConfigHash) << ','
            << result.Status << ','
            << BoolText(result.Passed) << ','
            << BoolText(result.Blocked) << ','
            << ModeName(result.RequestedSingleMode) << ','
            << ModeName(result.ActualSingleMode) << ','
            << ModeName(result.RequestedMultiMode) << ','
            << ModeName(result.ActualMultiMode) << ','
            << ModeName(result.RequestedReferenceMode) << ','
            << ModeName(result.ActualReferenceMode) << ','
            << ModeName(result.RequestedCandidateMode) << ','
            << ModeName(result.ActualCandidateMode) << ','
            << EscapeCsv(snapshot.ScenePreset) << ','
            << EscapeCsv(snapshot.WorkloadProfile) << ','
            << result.TotalVoxelCount << ','
            << result.ActualStaticCount << ','
            << result.ActualDynamicCount << ','
            << snapshot.RenderWidth << ','
            << snapshot.RenderHeight << ','
            << snapshot.ColorFormat << ','
            << snapshot.LinearDepthFormat << ','
            << std::fixed << std::setprecision(8)
            << snapshot.FixedDeltaTime << ','
            << snapshot.FixedStepCount << ','
            << snapshot.WarmupStepCount << ','
            << LodName(result.SpatialLodMode) << ','
            << snapshot.SpatialLod.Lod0Distance << ','
            << snapshot.SpatialLod.Lod1Distance << ','
            << snapshot.SpatialLod.Hysteresis << ','
            << result.TemporalInterval << ','
            << snapshot.SecondaryShare << ','
            << config.Tolerances.ColorTolerance << ','
            << config.Tolerances.DepthTolerance << ','
            << config.Tolerances.MaxColorMismatchPercent << ','
            << config.Tolerances.MaxDepthMismatchPercent << ','
            << result.ComparedPixelCount << ','
            << result.ForegroundUnionCount << ','
            << result.ForegroundIntersectionCount << ','
            << result.ColorMAE << ','
            << result.ColorRMSE << ','
            << result.ColorPSNR << ','
            << result.MaxColorError << ','
            << result.AlphaMAE << ','
            << result.DepthMAE << ','
            << result.DepthRMSE << ','
            << result.DepthRelativeMAE << ','
            << result.MaxDepthError << ','
            << result.ColorMismatchCount << ','
            << result.ColorMismatchPercent << ','
            << result.DepthMismatchCount << ','
            << result.DepthMismatchPercent << ','
            << result.CombinedMismatchCount << ','
            << result.CombinedMismatchPercent << ','
            << result.CoverageMismatchCount << ','
            << result.CoverageMismatchPercent << ','
            << EscapeCsv(result.ConfigHash) << ','
            << EscapeCsv(result.CameraHash) << ','
            << EscapeCsv(result.AdapterPairIdentity) << ','
            << EscapeCsv(PathString(result.SingleColorReferencePath)) << ','
            << EscapeCsv(PathString(result.SingleDepthReferencePath)) << ','
            << EscapeCsv(PathString(result.MultiColorReferencePath)) << ','
            << EscapeCsv(PathString(result.MultiDepthReferencePath)) << ','
            << EscapeCsv(PathString(result.DiffReferencePath)) << ','
            << EscapeCsv(snapshot.BuildHash) << ','
            << EscapeCsv(snapshot.ShaderSetHash) << ','
            << EscapeCsv(snapshot.ShaderHash) << ','
            << EscapeCsv(MatrixCsv(snapshot.View)) << ','
            << EscapeCsv(MatrixCsv(snapshot.Projection)) << ','
            << EscapeCsv(result.Reason) << '\n';
    }
}

void VoxelVisualValidationRunner::ExportJson(
    const VoxelVisualValidationConfig& config,
    const VoxelVisualValidationMetrics& metrics,
    const std::filesystem::path& path)
{
    std::ofstream json(path, std::ios::out | std::ios::trunc);
    if (!json.is_open())
        throw std::runtime_error("Failed to open visual validation JSON output");

    const auto& snapshot = config.Snapshot;
    json.imbue(std::locale::classic());
    json << std::fixed << std::setprecision(8);
    json << "{\n";
    json << "  \"validation_run_id\": \"" << EscapeJson(metrics.ValidationRunId) << "\",\n";
    json << "  \"aggregate_status\": \"" << (metrics.Passed ? "PASS" : (metrics.Blocked ? "BLOCKED" : "FAIL")) << "\",\n";
    json << "  \"aggregate_passed\": " << BoolText(metrics.Passed) << ",\n";
    json << "  \"aggregate_reason\": \"" << EscapeJson(metrics.FailReason) << "\",\n";
    json << "  \"provenance\": {\n";
    json << "    \"schema\": \"mgpu_research_provenance.v1\",\n";
    json << "    \"fields\": {\n";
    json << "      \"build.executable_sha256\": \"" << EscapeJson(snapshot.BuildHash) << "\",\n";
    json << "      \"build.shader_bytecode_set_sha256\": \"" << EscapeJson(snapshot.ShaderSetHash) << "\",\n";
    json << "      \"validation.protocol_sha256\": \"" << EscapeJson(snapshot.ProtocolHash) << "\",\n";
    json << "      \"validation.case_config_sha256\": \"" << EscapeJson(snapshot.CaseConfigHash) << "\",\n";
    json << "      \"validation.camera_sha256\": \"" << EscapeJson(snapshot.CameraHash) << "\"\n";
    json << "    }\n";
    json << "  },\n";
    json << "  \"snapshot\": {\n";
    json << "    \"schema_version\": " << snapshot.SchemaVersion << ",\n";
    json << "    \"protocol_version\": \"" << EscapeJson(snapshot.ProtocolVersion) << "\",\n";
    json << "    \"generator_version\": \"" << EscapeJson(snapshot.GeneratorVersion) << "\",\n";
    json << "    \"snapshot_hash\": \"" << HashHex(snapshot.SnapshotHash) << "\",\n";
    json << "    \"profile\": \"" << EscapeJson(snapshot.WorkloadProfile) << "\",\n";
    json << "    \"scene_preset\": \"" << EscapeJson(snapshot.ScenePreset) << "\",\n";
    json << "    \"static_seed\": " << snapshot.StaticSeed << ",\n";
    json << "    \"dynamic_seed\": " << snapshot.DynamicSeed << ",\n";
    json << "    \"requested_static_count\": " << snapshot.RequestedStaticCount << ",\n";
    json << "    \"actual_static_count\": " << snapshot.ActualStaticCount << ",\n";
    json << "    \"requested_dynamic_count\": " << snapshot.RequestedDynamicCount << ",\n";
    json << "    \"actual_dynamic_count\": " << snapshot.ActualDynamicCount << ",\n";
    json << "    \"total_voxels\": " << snapshot.TotalVoxelCount << ",\n";
    json << "    \"render_width\": " << snapshot.RenderWidth << ",\n";
    json << "    \"render_height\": " << snapshot.RenderHeight << ",\n";
    json << "    \"sample_count\": " << snapshot.SampleCount << ",\n";
    json << "    \"color_format\": \"" << EscapeJson(snapshot.ColorFormat) << "\",\n";
    json << "    \"linear_depth_format\": \"" << EscapeJson(snapshot.LinearDepthFormat) << "\",\n";
    json << "    \"fixed_dt\": " << snapshot.FixedDeltaTime << ",\n";
    json << "    \"fixed_step_count\": " << snapshot.FixedStepCount << ",\n";
    json << "    \"warmup_step_count\": " << snapshot.WarmupStepCount << ",\n";
    json << "    \"spatial_lod\": \"" << LodName(snapshot.SpatialLod.Mode) << "\",\n";
    json << "    \"temporal_interval\": " << snapshot.TemporalInterval << ",\n";
    json << "    \"secondary_share\": " << snapshot.SecondaryShare << ",\n";
    json << "    \"build_hash\": \"" << EscapeJson(snapshot.BuildHash) << "\",\n";
    json << "    \"shader_set_hash\": \"" << EscapeJson(snapshot.ShaderSetHash) << "\",\n";
    json << "    \"shader_hash\": \"" << EscapeJson(snapshot.ShaderHash) << "\",\n";
    json << "    \"adapter_pair\": \"" << EscapeJson(snapshot.AdapterPairIdentity) << "\",\n";
    json << "    \"primary_driver_version\": \"" << EscapeJson(snapshot.PrimaryDriverVersion) << "\",\n";
    json << "    \"secondary_driver_version\": \"" << EscapeJson(snapshot.SecondaryDriverVersion) << "\",\n";
    json << "    \"protocol_hash\": \"" << EscapeJson(snapshot.ProtocolHash) << "\",\n";
    json << "    \"case_config_hash\": \"" << EscapeJson(snapshot.CaseConfigHash) << "\",\n";
    json << "    \"camera_hash\": \"" << EscapeJson(snapshot.CameraHash) << "\",\n";
    WriteJsonMatrix(json, "view_matrix", snapshot.View, ",");
    WriteJsonMatrix(json, "projection_matrix", snapshot.Projection, "");
    json << "  },\n";
    json << "  \"tolerances\": {\n";
    json << "    \"color\": " << config.Tolerances.ColorTolerance << ",\n";
    json << "    \"depth\": " << config.Tolerances.DepthTolerance << ",\n";
    json << "    \"max_color_mismatch_percent\": " << config.Tolerances.MaxColorMismatchPercent << ",\n";
    json << "    \"max_depth_mismatch_percent\": " << config.Tolerances.MaxDepthMismatchPercent << ",\n";
    json << "    \"max_combined_mismatch_percent\": " << config.Tolerances.MaxCombinedMismatchPercent << ",\n";
    json << "    \"valid_depth_max\": " << config.Tolerances.ValidDepthMax << "\n";
    json << "  },\n";
    json << "  \"pass_criteria\": {\n";
    json << "    \"decides_pass\": [\"max_color_mismatch_percent\", \"max_depth_mismatch_percent\", \"max_combined_mismatch_percent\", \"max_combined_mismatch_percent_as_coverage_limit\"],\n";
    json << "    \"descriptive_only\": [\"mae\", \"rmse\", \"psnr\", \"max_error\"]\n";
    json << "  },\n";
    json << "  \"canonical_protocol_json\": \"" << EscapeJson(BuildCanonicalProtocolJson(config)) << "\",\n";
    json << "  \"cases\": [\n";
    for (size_t i = 0; i < metrics.CaseResults.size(); ++i)
    {
        const auto& result = metrics.CaseResults[i];
        json << "    {\n";
        json << "      \"case_id\": \"" << EscapeJson(result.CaseId) << "\",\n";
        json << "      \"validation_kind\": \"" << EscapeJson(result.ValidationKind) << "\",\n";
        json << "      \"mode_family\": \"" << EscapeJson(result.ModeFamily) << "\",\n";
        json << "      \"config_key\": \"" << EscapeJson(result.ConfigKey) << "\",\n";
        json << "      \"checkpoint_id\": \"" << EscapeJson(result.CheckpointId) << "\",\n";
        json << "      \"protocol_hash\": \"" << EscapeJson(result.ProtocolHash) << "\",\n";
        json << "      \"reference_config_hash\": \"" << EscapeJson(result.ReferenceConfigHash) << "\",\n";
        json << "      \"candidate_config_hash\": \"" << EscapeJson(result.CandidateConfigHash) << "\",\n";
        json << "      \"status\": \"" << result.Status << "\",\n";
        json << "      \"passed\": " << BoolText(result.Passed) << ",\n";
        json << "      \"blocked\": " << BoolText(result.Blocked) << ",\n";
        json << "      \"requested_single_mode\": \"" << ModeName(result.RequestedSingleMode) << "\",\n";
        json << "      \"actual_single_mode\": \"" << ModeName(result.ActualSingleMode) << "\",\n";
        json << "      \"requested_multi_mode\": \"" << ModeName(result.RequestedMultiMode) << "\",\n";
        json << "      \"actual_multi_mode\": \"" << ModeName(result.ActualMultiMode) << "\",\n";
        json << "      \"requested_reference_mode\": \"" << ModeName(result.RequestedReferenceMode) << "\",\n";
        json << "      \"actual_reference_mode\": \"" << ModeName(result.ActualReferenceMode) << "\",\n";
        json << "      \"requested_candidate_mode\": \"" << ModeName(result.RequestedCandidateMode) << "\",\n";
        json << "      \"actual_candidate_mode\": \"" << ModeName(result.ActualCandidateMode) << "\",\n";
        json << "      \"spatial_lod\": \"" << LodName(result.SpatialLodMode) << "\",\n";
        json << "      \"temporal_interval\": " << result.TemporalInterval << ",\n";
        json << "      \"config_hash\": \"" << EscapeJson(result.ConfigHash) << "\",\n";
        json << "      \"camera_hash\": \"" << EscapeJson(result.CameraHash) << "\",\n";
        json << "      \"adapter_pair\": \"" << EscapeJson(result.AdapterPairIdentity) << "\",\n";
        json << "      \"references\": {"
             << "\"single_color\":\"" << EscapeJson(PathString(result.SingleColorReferencePath)) << "\","
             << "\"single_depth\":\"" << EscapeJson(PathString(result.SingleDepthReferencePath)) << "\","
             << "\"multi_color\":\"" << EscapeJson(PathString(result.MultiColorReferencePath)) << "\","
             << "\"multi_depth\":\"" << EscapeJson(PathString(result.MultiDepthReferencePath)) << "\","
             << "\"diff\":\"" << EscapeJson(PathString(result.DiffReferencePath)) << "\"},\n";
        json << "      \"compared_pixel_count\": " << result.ComparedPixelCount << ",\n";
        json << "      \"foreground_union_count\": " << result.ForegroundUnionCount << ",\n";
        json << "      \"foreground_intersection_count\": " << result.ForegroundIntersectionCount << ",\n";
        json << "      \"color_mae\": " << result.ColorMAE << ",\n";
        json << "      \"color_rmse\": " << result.ColorRMSE << ",\n";
        json << "      \"psnr\": " << result.ColorPSNR << ",\n";
        json << "      \"max_color_error\": " << result.MaxColorError << ",\n";
        json << "      \"depth_rmse\": " << result.DepthRMSE << ",\n";
        json << "      \"depth_relative_mae\": " << result.DepthRelativeMAE << ",\n";
        json << "      \"combined_mismatch_percent\": " << result.CombinedMismatchPercent << ",\n";
        json << "      \"reason\": \"" << EscapeJson(result.Reason) << "\"\n";
        json << "    }" << (i + 1 < metrics.CaseResults.size() ? "," : "") << "\n";
    }
    json << "  ]\n";
    json << "}\n";
}
