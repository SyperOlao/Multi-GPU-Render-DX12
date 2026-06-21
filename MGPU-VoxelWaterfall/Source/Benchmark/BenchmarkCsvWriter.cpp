#include "Source/Benchmark/BenchmarkCsvWriter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <string>
#include <windows.h>

namespace
{
    std::string ToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                                 static_cast<int>(value.size()),
                                                 nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(std::max(0, required)), '\0');
        if (required > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                result.data(), required, nullptr, nullptr);
        }
        return result;
    }

    std::string EscapeCsv(const std::string& value)
    {
        if (value.find_first_of(",\"\n\r") == std::string::npos)
            return value;

        std::string escaped = "\"";
        for (const char ch : value)
            escaped += ch == '"' ? "\"\"" : std::string(1, ch);
        escaped += '"';
        return escaped;
    }

    std::string EscapeCsv(const std::wstring& value)
    {
        return EscapeCsv(ToUtf8(value));
    }

    bool SameBaselineKey(const VoxelBenchmarkProfiler::BenchmarkSummary& baseline,
                         const VoxelBenchmarkProfiler::BenchmarkSummary& candidate,
                         const std::string& baselineMode)
    {
        return baseline.RequestedMode == baselineMode &&
            baseline.TotalVoxelCount == candidate.TotalVoxelCount &&
            std::abs(baseline.SecondaryShare - candidate.SecondaryShare) < 0.0001f &&
            baseline.TemporalPolicy == candidate.TemporalPolicy &&
            baseline.RenderWidth == candidate.RenderWidth &&
            baseline.RenderHeight == candidate.RenderHeight &&
            baseline.PrimaryAdapterName == candidate.PrimaryAdapterName &&
            baseline.SecondaryAdapterName == candidate.SecondaryAdapterName &&
            baseline.SkipReason.empty() &&
            candidate.SkipReason.empty();
    }

    double MatchingSpeedup(const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries,
                           const VoxelBenchmarkProfiler::BenchmarkSummary& row)
    {
        std::string baselineMode;
        if (row.RequestedMode == "MultiGpuFull")
            baselineMode = "SingleGpuFull";
        else if (row.RequestedMode == "MultiGpuTemporalDecimation")
            baselineMode = "SingleGpuTemporalDecimation";
        else
            return 1.0;

        for (const auto& baseline : summaries)
        {
            if (SameBaselineKey(baseline, row, baselineMode) && row.AverageCpuFrameMs > 0.0)
                return baseline.AverageCpuFrameMs / row.AverageCpuFrameMs;
        }
        return 0.0;
    }
}

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
    summary << "requested_mode,actual_mode,primary_adapter,secondary_adapter,total_voxels,"
        << "secondary_share,temporal_policy,render_width,render_height,repetition,preset,"
        << "average_cpu_frame_ms,median_cpu_frame_ms,p95_cpu_frame_ms,p99_cpu_frame_ms,"
        << "stddev_cpu_frame_ms,cpu_frame_ci95_half_width_ms,critical_path_gpu_ms,"
        << "gpu_work_sum_ms,primary_compute_ms,secondary_compute_ms,primary_graphics_ms,"
        << "secondary_graphics_ms,transfer_ms,composite_ms,transfer_bytes,"
        << "particle_transfer_bytes,secondary_draw_calls,reused_secondary_image_rate,"
        << "speedup_vs_matching_single_gpu,efficiency,visual_validation_passed,skip_reason,csv_path\n";

    summary << std::fixed << std::setprecision(6);
    for (const auto& row : summaries)
    {
        const double speedup = MatchingSpeedup(summaries, row);
        const double efficiency = speedup > 0.0 ? speedup / 2.0 : 0.0;

        summary << EscapeCsv(row.RequestedMode) << ','
            << EscapeCsv(row.ActualMode) << ','
            << EscapeCsv(row.PrimaryAdapterName) << ','
            << EscapeCsv(row.SecondaryAdapterName) << ','
            << row.TotalVoxelCount << ','
            << row.SecondaryShare << ','
            << EscapeCsv(row.TemporalPolicy) << ','
            << row.RenderWidth << ','
            << row.RenderHeight << ','
            << row.Repetition << ','
            << EscapeCsv(row.Preset) << ','
            << row.AverageCpuFrameMs << ','
            << row.MedianCpuFrameMs << ','
            << row.P95CpuFrameMs << ','
            << row.P99CpuFrameMs << ','
            << row.StdDevCpuFrameMs << ','
            << row.CpuFrameCi95HalfWidthMs << ','
            << row.CriticalPathGpuMs << ','
            << row.GpuWorkSumMs << ','
            << row.PrimaryComputeMs << ','
            << row.SecondaryComputeMs << ','
            << row.PrimaryGraphicsMs << ','
            << row.SecondaryGraphicsMs << ','
            << row.TransferMs << ','
            << row.CompositeMs << ','
            << row.AverageTransferBytes << ','
            << row.AverageParticleTransferBytes << ','
            << row.AverageSecondaryDrawCalls << ','
            << row.ReusedSecondaryImageRate << ','
            << speedup << ','
            << efficiency << ','
            << (row.VisualValidationPassed ? "true" : "false") << ','
            << EscapeCsv(row.SkipReason) << ','
            << EscapeCsv(row.CsvPath.wstring()) << '\n';
    }

    return true;
}
