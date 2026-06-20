#include "Source/Benchmark/BenchmarkCsvWriter.h"

#include <fstream>
#include <iomanip>
#include <locale>

bool BenchmarkCsvWriter::WriteAutomaticSummary(
    const std::filesystem::path& outputPath,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    if (summaries.empty())
        return false;

    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream summary(outputPath, std::ios::out | std::ios::trunc);
    if (!summary.is_open())
        return false;

    summary.imbue(std::locale::classic());
    summary << "mode,preset,total_voxel_count,average_frame_ms,median_frame_ms,p95_frame_ms,"
        << "average_primary_compute_ms,average_secondary_compute_ms,average_transfer_ms,"
        << "average_sync_ms,average_graphics_ms,target_60_fps_reached\n";

    summary << std::fixed << std::setprecision(6);
    for (const auto& row : summaries)
    {
        summary << row.Mode << ','
            << row.Preset << ','
            << row.TotalVoxelCount << ','
            << row.AverageFrameMs << ','
            << row.MedianFrameMs << ','
            << row.P95FrameMs << ','
            << row.AveragePrimaryComputeMs << ','
            << row.AverageSecondaryComputeMs << ','
            << row.AverageTransferMs << ','
            << row.AverageSyncMs << ','
            << row.AverageGraphicsMs << ','
            << (row.Target60FpsReached ? "true" : "false") << '\n';
    }

    return true;
}

