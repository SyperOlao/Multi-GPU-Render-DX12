#include "Source/Validation/VoxelVisualValidationRunner.h"

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

    uint64_t Fnv1aAppendBytes(uint64_t hash, const void* data, const size_t byteCount)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < byteCount; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t Fnv1aAppendString(uint64_t hash, const std::string& value)
    {
        return Fnv1aAppendBytes(hash, value.data(), value.size());
    }

    template <typename T>
    uint64_t Fnv1aAppendValue(uint64_t hash, const T& value)
    {
        return Fnv1aAppendBytes(hash, &value, sizeof(T));
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
    return {
        {
            "full_lod_off",
            VoxelExecutionMode::SingleGpuFull,
            VoxelExecutionMode::MultiGpuFull,
            false,
            1u
        },
        {
            "full_lod_on",
            VoxelExecutionMode::SingleGpuFull,
            VoxelExecutionMode::MultiGpuFull,
            true,
            1u
        },
        {
            "temporal_lod_off",
            VoxelExecutionMode::SingleGpuTemporalDecimation,
            VoxelExecutionMode::MultiGpuTemporalDecimation,
            false,
            2u
        },
        {
            "temporal_lod_on",
            VoxelExecutionMode::SingleGpuTemporalDecimation,
            VoxelExecutionMode::MultiGpuTemporalDecimation,
            true,
            2u
        }
    };
}

uint64_t VoxelVisualValidationRunner::ComputeSnapshotHash(const VoxelVisualValidationSnapshot& snapshot)
{
    uint64_t hash = 1469598103934665603ull;
    hash = Fnv1aAppendValue(hash, snapshot.SchemaVersion);
    hash = Fnv1aAppendString(hash, snapshot.WorkloadProfile);
    hash = Fnv1aAppendString(hash, snapshot.ScenePreset);
    hash = Fnv1aAppendValue(hash, snapshot.StaticSeed);
    hash = Fnv1aAppendValue(hash, snapshot.DynamicSeed);
    hash = Fnv1aAppendValue(hash, snapshot.RequestedStaticCount);
    hash = Fnv1aAppendValue(hash, snapshot.ActualStaticCount);
    hash = Fnv1aAppendValue(hash, snapshot.RequestedDynamicCount);
    hash = Fnv1aAppendValue(hash, snapshot.ActualDynamicCount);
    hash = Fnv1aAppendValue(hash, snapshot.TotalVoxelCount);
    hash = Fnv1aAppendValue(hash, snapshot.StaticVoxelSize);
    hash = Fnv1aAppendValue(hash, snapshot.DynamicVoxelSize);
    hash = Fnv1aAppendValue(hash, snapshot.RenderWidth);
    hash = Fnv1aAppendValue(hash, snapshot.RenderHeight);
    hash = Fnv1aAppendString(hash, snapshot.ColorFormat);
    hash = Fnv1aAppendString(hash, snapshot.LinearDepthFormat);
    hash = Fnv1aAppendValue(hash, snapshot.FixedDeltaTime);
    hash = Fnv1aAppendValue(hash, snapshot.FixedStepCount);
    hash = Fnv1aAppendValue(hash, snapshot.WarmupStepCount);
    hash = Fnv1aAppendValue(hash, snapshot.SpatialLod.Mode);
    hash = Fnv1aAppendValue(hash, snapshot.SpatialLod.Lod0Distance);
    hash = Fnv1aAppendValue(hash, snapshot.SpatialLod.Lod1Distance);
    hash = Fnv1aAppendValue(hash, snapshot.SpatialLod.Hysteresis);
    hash = Fnv1aAppendValue(hash, snapshot.TemporalPolicy);
    hash = Fnv1aAppendValue(hash, snapshot.TemporalInterval);
    hash = Fnv1aAppendValue(hash, snapshot.SecondaryShare);
    hash = Fnv1aAppendValue(hash, snapshot.PartitionStrategy);
    hash = Fnv1aAppendValue(hash, snapshot.LoadBalanceScenario);
    hash = Fnv1aAppendValue(hash, snapshot.ChunkSize.Width);
    hash = Fnv1aAppendValue(hash, snapshot.ChunkSize.Height);
    hash = Fnv1aAppendValue(hash, snapshot.ChunkSize.Depth);
    hash = Fnv1aAppendValue(hash, snapshot.RequestedExecutionMode);
    hash = Fnv1aAppendBytes(hash, snapshot.View, sizeof(snapshot.View));
    hash = Fnv1aAppendBytes(hash, snapshot.Projection, sizeof(snapshot.Projection));
    hash = Fnv1aAppendValue(hash, snapshot.NearZ);
    hash = Fnv1aAppendValue(hash, snapshot.FarZ);
    hash = Fnv1aAppendString(hash, snapshot.LightingPreset);
    hash = Fnv1aAppendValue(hash, snapshot.DynamicShadowsEnabled);
    hash = Fnv1aAppendString(hash, snapshot.Background);
    hash = Fnv1aAppendString(hash, snapshot.BuildHash);
    hash = Fnv1aAppendString(hash, snapshot.ShaderHash);
    return hash;
}

VoxelVisualValidationMetrics VoxelVisualValidationRunner::RunDeterministicSuite(
    const VoxelVisualValidationConfig& config,
    const std::filesystem::path& outputDirectory) const
{
    std::filesystem::create_directories(outputDirectory);

    VoxelVisualValidationConfig resolvedConfig = config;
    auto& snapshot = resolvedConfig.Snapshot;
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
        result.RequestedSingleMode = validationCase.SingleMode;
        result.ActualSingleMode = validationCase.SingleMode;
        result.RequestedMultiMode = validationCase.MultiMode;
        result.ActualMultiMode = VoxelExecutionMode::SingleGpuFull;
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
            result.ConfigHash = comparison->ConfigHash;
            result.CameraHash = comparison->CameraHash;
            result.AdapterPairIdentity = comparison->AdapterPairIdentity;
            result.SingleColorReferencePath = comparison->SingleColorReferencePath;
            result.SingleDepthReferencePath = comparison->SingleDepthReferencePath;
            result.MultiColorReferencePath = comparison->MultiColorReferencePath;
            result.MultiDepthReferencePath = comparison->MultiDepthReferencePath;
            result.DiffReferencePath = comparison->DiffReferencePath;

            if (!comparison->CaptureAvailable)
                result.Reason = "deterministic Single/Multi color/depth capture was not supplied";
            else if (!comparison->CompareShaderDispatched)
                result.Reason = "VoxelValidationCompare.hlsl dispatch evidence was not supplied";
            else if (!comparison->ReadbackComplete)
                result.Reason = "validation tile statistics readback is not complete";
            else if (comparison->TileStats.empty())
                result.Reason = "validation tile statistics are empty";
            else
                AccumulateComparison(*comparison, resolvedConfig.Tolerances, result);
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
        const auto& first = metrics.CaseResults.front();
        metrics.ColorMAE = first.ColorMAE;
        metrics.ColorRMSE = first.ColorRMSE;
        metrics.ColorPSNR = first.ColorPSNR;
        metrics.MaxColorError = first.MaxColorError;
        metrics.ColorMismatchPercent = first.ColorMismatchPercent;
        metrics.DepthRMSE = first.DepthRMSE;
        metrics.DepthMismatchPercent = first.DepthMismatchPercent;
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
    csv << "validation_run_id,snapshot_hash,case_id,status,passed,blocked,requested_single_mode,"
        << "actual_single_mode,requested_multi_mode,actual_multi_mode,scene_preset,profile,total_voxels,"
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
        << "build_hash,shader_hash,view_matrix,projection_matrix,reason\n";

    for (const auto& result : metrics.CaseResults)
    {
        csv << EscapeCsv(metrics.ValidationRunId) << ','
            << HashHex(result.SnapshotHash) << ','
            << EscapeCsv(result.CaseId) << ','
            << result.Status << ','
            << BoolText(result.Passed) << ','
            << BoolText(result.Blocked) << ','
            << ModeName(result.RequestedSingleMode) << ','
            << ModeName(result.ActualSingleMode) << ','
            << ModeName(result.RequestedMultiMode) << ','
            << ModeName(result.ActualMultiMode) << ','
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
    json << "  \"snapshot\": {\n";
    json << "    \"schema_version\": " << snapshot.SchemaVersion << ",\n";
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
    json << "    \"color_format\": \"" << EscapeJson(snapshot.ColorFormat) << "\",\n";
    json << "    \"linear_depth_format\": \"" << EscapeJson(snapshot.LinearDepthFormat) << "\",\n";
    json << "    \"fixed_dt\": " << snapshot.FixedDeltaTime << ",\n";
    json << "    \"fixed_step_count\": " << snapshot.FixedStepCount << ",\n";
    json << "    \"warmup_step_count\": " << snapshot.WarmupStepCount << ",\n";
    json << "    \"spatial_lod\": \"" << LodName(snapshot.SpatialLod.Mode) << "\",\n";
    json << "    \"temporal_interval\": " << snapshot.TemporalInterval << ",\n";
    json << "    \"secondary_share\": " << snapshot.SecondaryShare << ",\n";
    json << "    \"build_hash\": \"" << EscapeJson(snapshot.BuildHash) << "\",\n";
    json << "    \"shader_hash\": \"" << EscapeJson(snapshot.ShaderHash) << "\",\n";
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
    json << "  \"cases\": [\n";
    for (size_t i = 0; i < metrics.CaseResults.size(); ++i)
    {
        const auto& result = metrics.CaseResults[i];
        json << "    {\n";
        json << "      \"case_id\": \"" << EscapeJson(result.CaseId) << "\",\n";
        json << "      \"status\": \"" << result.Status << "\",\n";
        json << "      \"passed\": " << BoolText(result.Passed) << ",\n";
        json << "      \"blocked\": " << BoolText(result.Blocked) << ",\n";
        json << "      \"requested_single_mode\": \"" << ModeName(result.RequestedSingleMode) << "\",\n";
        json << "      \"actual_single_mode\": \"" << ModeName(result.ActualSingleMode) << "\",\n";
        json << "      \"requested_multi_mode\": \"" << ModeName(result.RequestedMultiMode) << "\",\n";
        json << "      \"actual_multi_mode\": \"" << ModeName(result.ActualMultiMode) << "\",\n";
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
