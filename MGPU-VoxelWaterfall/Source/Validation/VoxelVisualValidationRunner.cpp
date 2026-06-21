#include "Source/Validation/VoxelVisualValidationRunner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace
{
    std::string BoolText(const bool value)
    {
        return value ? "true" : "false";
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
}

std::vector<VoxelVisualValidationCase> VoxelVisualValidationConfig::DefaultCases()
{
    std::vector<VoxelVisualValidationCase> cases;
    constexpr float shares[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    constexpr uint32_t intervals[] = {1u, 4u};
    for (const float share : shares)
    {
        for (const bool spatialLodEnabled : {false, true})
        {
            for (const uint32_t interval : intervals)
            {
                std::ostringstream name;
                name << "share_" << share
                    << "_lod_" << (spatialLodEnabled ? "on" : "off")
                    << "_temporal_" << interval;
                cases.push_back({name.str(), share, spatialLodEnabled, interval});
            }
        }
    }

    cases.push_back({"resize", 0.5f, false, 1});
    cases.push_back({"voxels_before_primary_geometry", 0.5f, false, 1});
    cases.push_back({"voxels_behind_primary_geometry", 0.5f, false, 1});
    cases.push_back({"intersecting_partitions", 0.5f, true, 4});
    return cases;
}

VoxelVisualValidationMetrics VoxelVisualValidationRunner::RunDeterministicSuite(
    const VoxelVisualValidationConfig& config,
    const std::filesystem::path& outputDirectory) const
{
    std::filesystem::create_directories(outputDirectory);

    VoxelVisualValidationMetrics metrics{};
    metrics.HasResult = true;
    metrics.Passed = false;
    metrics.FailReason =
        "Deterministic SingleGpuFull/MultiGpuFull validation textures were not captured; numerical comparison was not executed.";
    metrics.ColorMAE = 0.0;
    metrics.ColorRMSE = 0.0;
    metrics.ColorPSNR = 0.0;
    metrics.MaxColorError = 0.0;
    metrics.ColorMismatchPercent = 100.0;
    metrics.DepthRMSE = 0.0;
    metrics.DepthMismatchPercent = 100.0;
    metrics.CsvPath = outputDirectory / "voxel_visual_validation.csv";
    metrics.JsonPath = outputDirectory / "voxel_visual_validation.json";

    ExportCsv(config, metrics, metrics.CsvPath);
    ExportJson(config, metrics, metrics.JsonPath);
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

    csv.imbue(std::locale::classic());
    csv << "case,secondary_share,spatial_lod,temporal_interval,seed,total_voxels,render_width,render_height,"
        << "fixed_dt,fixed_step_count,color_tolerance,depth_tolerance,color_mae,color_rmse,psnr,max_error,"
        << "mismatched_pixel_percent,depth_rmse,depth_mismatched_pixel_percent,secondary_draw_count,"
        << "pipeline_primitive_count,passed,fail_reason\n";

    const auto& cases = config.Cases.empty() ? VoxelVisualValidationConfig::DefaultCases() : config.Cases;
    for (const auto& validationCase : cases)
    {
        csv << EscapeCsv(validationCase.Name) << ','
            << validationCase.SecondaryShare << ','
            << BoolText(validationCase.SpatialLodEnabled) << ','
            << validationCase.TemporalInterval << ','
            << config.Snapshot.Seed << ','
            << config.Snapshot.TotalVoxelCount << ','
            << config.Snapshot.RenderWidth << ','
            << config.Snapshot.RenderHeight << ','
            << std::fixed << std::setprecision(8)
            << config.Snapshot.FixedDeltaTime << ','
            << config.Snapshot.FixedStepCount << ','
            << config.Tolerances.ColorTolerance << ','
            << config.Tolerances.DepthTolerance << ','
            << metrics.ColorMAE << ','
            << metrics.ColorRMSE << ','
            << metrics.ColorPSNR << ','
            << metrics.MaxColorError << ','
            << metrics.ColorMismatchPercent << ','
            << metrics.DepthRMSE << ','
            << metrics.DepthMismatchPercent << ','
            << metrics.ActualSecondaryDrawCount << ','
            << metrics.PipelinePrimitiveCount << ','
            << BoolText(metrics.Passed) << ','
            << EscapeCsv(metrics.FailReason) << '\n';
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

    json.imbue(std::locale::classic());
    json << "{\n";
    json << "  \"snapshot\": {\n";
    json << "    \"seed\": " << config.Snapshot.Seed << ",\n";
    json << "    \"total_voxels\": " << config.Snapshot.TotalVoxelCount << ",\n";
    json << "    \"render_width\": " << config.Snapshot.RenderWidth << ",\n";
    json << "    \"render_height\": " << config.Snapshot.RenderHeight << ",\n";
    json << "    \"fixed_dt\": " << std::fixed << std::setprecision(8)
        << config.Snapshot.FixedDeltaTime << ",\n";
    json << "    \"fixed_step_count\": " << config.Snapshot.FixedStepCount << "\n";
    json << "  },\n";
    json << "  \"tolerances\": {\n";
    json << "    \"color\": " << config.Tolerances.ColorTolerance << ",\n";
    json << "    \"depth\": " << config.Tolerances.DepthTolerance << ",\n";
    json << "    \"max_color_mismatch_percent\": " << config.Tolerances.MaxColorMismatchPercent << ",\n";
    json << "    \"max_depth_mismatch_percent\": " << config.Tolerances.MaxDepthMismatchPercent << "\n";
    json << "  },\n";
    json << "  \"metrics\": {\n";
    json << "    \"has_result\": " << BoolText(metrics.HasResult) << ",\n";
    json << "    \"passed\": " << BoolText(metrics.Passed) << ",\n";
    json << "    \"color_mae\": " << metrics.ColorMAE << ",\n";
    json << "    \"color_rmse\": " << metrics.ColorRMSE << ",\n";
    json << "    \"psnr\": " << metrics.ColorPSNR << ",\n";
    json << "    \"max_error\": " << metrics.MaxColorError << ",\n";
    json << "    \"mismatched_pixel_percent\": " << metrics.ColorMismatchPercent << ",\n";
    json << "    \"depth_rmse\": " << metrics.DepthRMSE << ",\n";
    json << "    \"depth_mismatched_pixel_percent\": " << metrics.DepthMismatchPercent << ",\n";
    json << "    \"secondary_draw_count\": " << metrics.ActualSecondaryDrawCount << ",\n";
    json << "    \"pipeline_primitive_count\": " << metrics.PipelinePrimitiveCount << ",\n";
    json << "    \"fail_reason\": \"" << EscapeJson(metrics.FailReason) << "\"\n";
    json << "  },\n";
    json << "  \"cases\": [\n";
    const auto& cases = config.Cases.empty() ? VoxelVisualValidationConfig::DefaultCases() : config.Cases;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const auto& validationCase = cases[i];
        json << "    {\"name\": \"" << EscapeJson(validationCase.Name)
            << "\", \"secondary_share\": " << validationCase.SecondaryShare
            << ", \"spatial_lod\": " << BoolText(validationCase.SpatialLodEnabled)
            << ", \"temporal_interval\": " << validationCase.TemporalInterval << "}";
        json << (i + 1 < cases.size() ? ",\n" : "\n");
    }
    json << "  ]\n";
    json << "}\n";
}
