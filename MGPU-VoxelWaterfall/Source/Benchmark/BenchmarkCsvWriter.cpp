#include "Source/Benchmark/BenchmarkCsvWriter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <numeric>
#include <string>
#include <tuple>
#include <windows.h>

namespace
{
    using Summary = VoxelBenchmarkProfiler::BenchmarkSummary;

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

    struct AggregateKey
    {
        std::string RequestedMode;
        std::string ActualMode;
        std::string Preset;
        std::string ProfileName;
        std::string PartitionStrategy;
        std::string LoadBalanceScenario;
        std::string BenchmarkConfigClass;
        std::string TemporalPolicy;
        std::string SpatialLodPolicy;
        std::string PairId;
        uint32_t TotalVoxelCount = 0;
        uint32_t ActualStaticVoxelCount = 0;
        uint32_t ActualDynamicVoxelCount = 0;
        int SecondarySharePermille = 0;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        std::wstring PrimaryAdapterName;
        std::wstring SecondaryAdapterName;

        bool operator<(const AggregateKey& other) const
        {
            return std::tie(RequestedMode, ActualMode, Preset, ProfileName, PartitionStrategy,
                            LoadBalanceScenario, BenchmarkConfigClass, TemporalPolicy, SpatialLodPolicy,
                            PairId, TotalVoxelCount, ActualStaticVoxelCount, ActualDynamicVoxelCount,
                            SecondarySharePermille, RenderWidth, RenderHeight, PrimaryAdapterName,
                            SecondaryAdapterName) <
                std::tie(other.RequestedMode, other.ActualMode, other.Preset, other.ProfileName,
                         other.PartitionStrategy, other.LoadBalanceScenario, other.BenchmarkConfigClass,
                         other.TemporalPolicy, other.SpatialLodPolicy, other.PairId, other.TotalVoxelCount,
                         other.ActualStaticVoxelCount, other.ActualDynamicVoxelCount,
                         other.SecondarySharePermille, other.RenderWidth, other.RenderHeight,
                         other.PrimaryAdapterName, other.SecondaryAdapterName);
        }
    };

    AggregateKey KeyFor(const Summary& summary)
    {
        return {
            summary.RequestedMode,
            summary.ActualMode,
            summary.Preset,
            summary.ProfileName,
            summary.PartitionStrategy,
            summary.LoadBalanceScenario,
            summary.BenchmarkConfigClass,
            summary.TemporalPolicy,
            summary.SpatialLodPolicy,
            summary.PairId,
            summary.TotalVoxelCount,
            summary.ActualStaticVoxelCount,
            summary.ActualDynamicVoxelCount,
            static_cast<int>(std::round(summary.SecondaryShare * 1000.0f)),
            summary.RenderWidth,
            summary.RenderHeight,
            summary.PrimaryAdapterName,
            summary.SecondaryAdapterName
        };
    }

    AggregateKey MatchingSingleKey(const Summary& row)
    {
        AggregateKey key = KeyFor(row);
        if (row.RequestedMode == "MultiGpuFull")
        {
            key.RequestedMode = "SingleGpuFull";
            key.ActualMode = "SingleGpuFull";
        }
        else if (row.RequestedMode == "MultiGpuTemporalDecimation")
        {
            key.RequestedMode = "SingleGpuTemporalDecimation";
            key.ActualMode = "SingleGpuTemporalDecimation";
        }
        return key;
    }

    bool IsSingleMode(const std::string& mode)
    {
        return mode == "SingleGpuFull" || mode == "SingleGpuTemporalDecimation";
    }

    bool IsMultiMode(const std::string& mode)
    {
        return mode == "MultiGpuFull" || mode == "MultiGpuTemporalDecimation";
    }

    std::string ModeFamily(const std::string& mode)
    {
        return mode.find("Temporal") != std::string::npos ? "Temporal" : "Full";
    }

    double Average(const std::vector<double>& values)
    {
        if (values.empty())
            return 0.0;
        return std::accumulate(values.begin(), values.end(), 0.0) /
            static_cast<double>(values.size());
    }

    double StdDev(const std::vector<double>& values)
    {
        if (values.size() < 2)
            return 0.0;
        const double mean = Average(values);
        double sum = 0.0;
        for (const double value : values)
        {
            const double delta = value - mean;
            sum += delta * delta;
        }
        return std::sqrt(sum / static_cast<double>(values.size() - 1));
    }

    double Median(std::vector<double> values)
    {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        const size_t mid = values.size() / 2;
        if ((values.size() % 2) == 0)
            return (values[mid - 1] + values[mid]) * 0.5;
        return values[mid];
    }

    uint64_t AverageUint64(const std::vector<uint64_t>& values)
    {
        if (values.empty())
            return 0;
        long double sum = 0.0;
        for (const auto value : values)
            sum += static_cast<long double>(value);
        return static_cast<uint64_t>(sum / static_cast<long double>(values.size()));
    }

    double StudentTCritical95TwoSided(const size_t sampleCount)
    {
        if (sampleCount < 2)
            return 0.0;
        switch (sampleCount - 1)
        {
        case 1: return 12.706204736;
        case 2: return 4.302652730;
        case 3: return 3.182446305;
        case 4: return 2.776445105;
        case 5: return 2.570581836;
        case 6: return 2.446911851;
        case 7: return 2.364624252;
        case 8: return 2.306004135;
        case 9: return 2.262157163;
        case 10: return 2.228138852;
        default: return 1.959963985;
        }
    }

    Summary Aggregate(const std::vector<Summary>& rows)
    {
        Summary result = rows.front();
        result.Repetition = 0;
        result.RepetitionCount = static_cast<uint32_t>(rows.size());
        result.MeasuredFrameCount = 0;
        result.ValidFrameCount = 0;
        result.InvalidFrameCount = 0;
        result.Valid = true;
        result.ValidityReason.clear();
        result.CsvPath.clear();

        std::vector<double> cpuMean;
        std::vector<double> cpuMedian;
        std::vector<double> criticalPath;
        std::vector<double> gpuWorkSum;
        std::vector<double> primaryCompute;
        std::vector<double> primaryLod;
        std::vector<double> primaryGraphics;
        std::vector<double> secondaryCompute;
        std::vector<double> secondaryLod;
        std::vector<double> secondaryGraphics;
        std::vector<double> transfer;
        std::vector<double> composite;
        std::vector<double> secondaryDraws;
        std::vector<double> primarySubmitted;
        std::vector<double> secondarySubmitted;
        std::vector<double> primaryLod0;
        std::vector<double> primaryLod1;
        std::vector<double> primaryLod2;
        std::vector<double> secondaryLod0;
        std::vector<double> secondaryLod1;
        std::vector<double> secondaryLod2;
        std::vector<uint64_t> totalTransferBytes;
        std::vector<uint64_t> colorTransferBytes;
        std::vector<uint64_t> depthTransferBytes;
        std::vector<uint64_t> particleTransferBytes;
        std::vector<uint64_t> renderOutputTransferBytes;

        for (const auto& row : rows)
        {
            result.MeasuredFrameCount += row.MeasuredFrameCount;
            result.ValidFrameCount += row.ValidFrameCount;
            result.InvalidFrameCount += row.InvalidFrameCount;
            result.VisualValidationPassed = result.VisualValidationPassed && row.VisualValidationPassed;
            result.Valid = result.Valid && row.Valid && row.SkipReason.empty();
            if ((!row.Valid || !row.SkipReason.empty()) && result.ValidityReason.empty())
                result.ValidityReason = !row.ValidityReason.empty() ? row.ValidityReason : row.SkipReason;

            cpuMean.push_back(row.AverageCpuFrameMs);
            cpuMedian.push_back(row.MedianCpuFrameMs);
            criticalPath.push_back(row.CriticalPathGpuMs);
            gpuWorkSum.push_back(row.GpuWorkSumMs);
            primaryCompute.push_back(row.PrimaryComputeMs);
            primaryLod.push_back(row.PrimaryLodCompactionMs);
            primaryGraphics.push_back(row.PrimaryGraphicsMs);
            secondaryCompute.push_back(row.SecondaryComputeMs);
            secondaryLod.push_back(row.SecondaryLodCompactionMs);
            secondaryGraphics.push_back(row.SecondaryGraphicsMs);
            transfer.push_back(row.TransferMs);
            composite.push_back(row.CompositeMs);
            totalTransferBytes.push_back(row.AverageTransferBytes);
            colorTransferBytes.push_back(row.AverageColorTransferBytes);
            depthTransferBytes.push_back(row.AverageDepthTransferBytes);
            particleTransferBytes.push_back(row.AverageParticleTransferBytes);
            renderOutputTransferBytes.push_back(row.AverageRenderOutputTransferBytes);
            secondaryDraws.push_back(row.AverageSecondaryDrawCalls);
            primarySubmitted.push_back(row.AveragePrimarySubmittedVoxelCount);
            secondarySubmitted.push_back(row.AverageSecondarySubmittedVoxelCount);
            primaryLod0.push_back(row.AveragePrimaryLod0Count);
            primaryLod1.push_back(row.AveragePrimaryLod1Count);
            primaryLod2.push_back(row.AveragePrimaryLod2Count);
            secondaryLod0.push_back(row.AverageSecondaryLod0Count);
            secondaryLod1.push_back(row.AverageSecondaryLod1Count);
            secondaryLod2.push_back(row.AverageSecondaryLod2Count);
        }

        result.AverageCpuFrameMs = Average(cpuMean);
        result.MedianCpuFrameMs = Median(cpuMedian);
        result.P95CpuFrameMs = Median([&]
        {
            std::vector<double> values;
            values.reserve(rows.size());
            for (const auto& row : rows)
                values.push_back(row.P95CpuFrameMs);
            return values;
        }());
        result.P99CpuFrameMs = Median([&]
        {
            std::vector<double> values;
            values.reserve(rows.size());
            for (const auto& row : rows)
                values.push_back(row.P99CpuFrameMs);
            return values;
        }());
        result.StdDevCpuFrameMs = StdDev(cpuMean);
        result.CpuFrameCi95HalfWidthMs =
            cpuMean.size() > 1 ? StudentTCritical95TwoSided(cpuMean.size()) * result.StdDevCpuFrameMs /
            std::sqrt(static_cast<double>(cpuMean.size())) : 0.0;
        result.CriticalPathGpuMs = Average(criticalPath);
        result.GpuWorkSumMs = Average(gpuWorkSum);
        result.PrimaryComputeMs = Average(primaryCompute);
        result.PrimaryLodCompactionMs = Average(primaryLod);
        result.PrimaryGraphicsMs = Average(primaryGraphics);
        result.SecondaryComputeMs = Average(secondaryCompute);
        result.SecondaryLodCompactionMs = Average(secondaryLod);
        result.SecondaryGraphicsMs = Average(secondaryGraphics);
        result.TransferMs = Average(transfer);
        result.CompositeMs = Average(composite);
        result.AverageTransferBytes = AverageUint64(totalTransferBytes);
        result.AverageColorTransferBytes = AverageUint64(colorTransferBytes);
        result.AverageDepthTransferBytes = AverageUint64(depthTransferBytes);
        result.AverageParticleTransferBytes = AverageUint64(particleTransferBytes);
        result.AverageRenderOutputTransferBytes = AverageUint64(renderOutputTransferBytes);
        result.AverageSecondaryDrawCalls = Average(secondaryDraws);
        result.AveragePrimarySubmittedVoxelCount = Average(primarySubmitted);
        result.AverageSecondarySubmittedVoxelCount = Average(secondarySubmitted);
        result.AveragePrimaryLod0Count = Average(primaryLod0);
        result.AveragePrimaryLod1Count = Average(primaryLod1);
        result.AveragePrimaryLod2Count = Average(primaryLod2);
        result.AverageSecondaryLod0Count = Average(secondaryLod0);
        result.AverageSecondaryLod1Count = Average(secondaryLod1);
        result.AverageSecondaryLod2Count = Average(secondaryLod2);
        for (const auto& row : rows)
        {
            result.TotalExecutedFixedSteps += row.TotalExecutedFixedSteps;
            result.TotalLogicalUpdatedVoxelCount += row.TotalLogicalUpdatedVoxelCount;
        }
        result.SkipReason = result.Valid ? "" : result.ValidityReason;
        result.SpeedupStatistic = "run_mean_cpu_frame_ms_student_t_95_ci";
        return result;
    }
}

bool BenchmarkCsvWriter::WriteAutomaticSummary(
    const std::filesystem::path& outputPath,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    if (summaries.empty())
    {
        std::filesystem::create_directories(outputPath.parent_path());
        std::ofstream summary(outputPath, std::ios::out | std::ios::trunc);
        if (summary.is_open())
        {
            summary << "schema,status,reason\n"
                    << "mgpu_voxel_paired_summary.v1,NOT_MEASURED,no valid benchmark summaries\n";
        }
        return false;
    }

    std::map<AggregateKey, std::vector<Summary>> groupedRows;
    for (const auto& summary : summaries)
        groupedRows[KeyFor(summary)].push_back(summary);

    std::map<std::tuple<std::string, std::string, std::string, uint32_t, std::string,
                        uint32_t, uint32_t, uint32_t>, Summary> singleRuns;
    for (const auto& summary : summaries)
    {
        if (!summary.Valid || !summary.SkipReason.empty() || !IsSingleMode(summary.RequestedMode))
            continue;
        singleRuns[{summary.SessionId, summary.PairId, summary.BlockId,
                    summary.Repetition, ModeFamily(summary.RequestedMode),
                    summary.TotalVoxelCount, summary.ActualStaticVoxelCount,
                    summary.ActualDynamicVoxelCount}] = summary;
    }

    std::map<AggregateKey, std::vector<double>> pairedLogSpeedups;
    for (const auto& summary : summaries)
    {
        if (!summary.Valid || !summary.SkipReason.empty() || !IsMultiMode(summary.RequestedMode) ||
            summary.AverageCpuFrameMs <= 0.0)
        {
            continue;
        }
        const auto singleIt = singleRuns.find({
            summary.SessionId, summary.PairId, summary.BlockId, summary.Repetition,
            ModeFamily(summary.RequestedMode), summary.TotalVoxelCount,
            summary.ActualStaticVoxelCount, summary.ActualDynamicVoxelCount
        });
        if (singleIt == singleRuns.end() || singleIt->second.AverageCpuFrameMs <= 0.0)
            continue;
        pairedLogSpeedups[KeyFor(summary)].push_back(
            std::log(singleIt->second.AverageCpuFrameMs / summary.AverageCpuFrameMs));
    }

    std::map<AggregateKey, Summary> aggregates;
    for (const auto& [key, rows] : groupedRows)
        aggregates.emplace(key, Aggregate(rows));

    for (auto& [key, row] : aggregates)
    {
        const bool isMulti = IsMultiMode(row.RequestedMode);
        if (!isMulti)
        {
            row.SpeedupVsMatchingSingleGpu = 1.0;
            row.Efficiency = 1.0;
            continue;
        }

        const auto baselineIt = aggregates.find(MatchingSingleKey(row));
        const auto pairedIt = pairedLogSpeedups.find(key);
        if (baselineIt == aggregates.end() || !baselineIt->second.Valid || !row.Valid ||
            row.AverageCpuFrameMs <= 0.0 || pairedIt == pairedLogSpeedups.end() ||
            pairedIt->second.empty())
        {
            row.SpeedupVsMatchingSingleGpu = 0.0;
            row.Efficiency = 0.0;
            if (row.ValidityReason.empty())
                row.ValidityReason = "matching valid Single/Multi run-level block not found";
            row.Valid = false;
            row.SkipReason = row.ValidityReason;
            continue;
        }
        if (baselineIt->second.TotalExecutedFixedSteps != row.TotalExecutedFixedSteps ||
            baselineIt->second.TotalLogicalUpdatedVoxelCount != row.TotalLogicalUpdatedVoxelCount)
        {
            row.SpeedupVsMatchingSingleGpu = 0.0;
            row.Efficiency = 0.0;
            row.Valid = false;
            row.ValidityReason = "matching Single/Multi pair executed different logical simulation work";
            row.SkipReason = row.ValidityReason;
            continue;
        }

        row.SpeedupVsMatchingSingleGpu = std::exp(Average(pairedIt->second));
        row.Efficiency = row.SpeedupVsMatchingSingleGpu / 2.0;
        row.SpeedupStatistic = "paired_run_log_speedup_student_t_95_ci";
    }

    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream summary(outputPath, std::ios::out | std::ios::trunc);
    if (!summary.is_open())
        return false;

    summary.imbue(std::locale::classic());
    summary << "requested_mode,actual_mode,primary_adapter,secondary_adapter,total_voxels,"
        << "requested_label_count,requested_static_budget,requested_dynamic_budget,"
        << "actual_total_count,"
        << "secondary_share,profile,partition_strategy,load_balance_scenario,benchmark_config_class,"
        << "temporal_policy,spatial_lod_policy,pair_id,"
        << "actual_static_count,actual_dynamic_count,actual_static_voxels,actual_dynamic_voxels,"
        << "resolved_config_hash,render_width,render_height,"
        << "repetition_count,measured_frame_count,valid_frame_count,invalid_frame_count,"
        << "preset,run_valid,validity_reason,speedup_statistic,"
        << "average_cpu_frame_ms,median_cpu_frame_ms,p95_cpu_frame_ms,p99_cpu_frame_ms,"
        << "stddev_cpu_frame_ms,cpu_frame_ci95_half_width_ms,"
        << "critical_path_gpu_ms,gpu_work_sum_ms,primary_compute_ms,primary_lod_compaction_ms,"
        << "secondary_compute_ms,secondary_lod_compaction_ms,primary_graphics_ms,"
        << "secondary_graphics_ms,transfer_ms,composite_ms,total_transfer_bytes,"
        << "color_transfer_bytes,depth_transfer_bytes,particle_transfer_bytes,render_output_transfer_bytes,"
        << "secondary_draw_calls,primary_submitted_voxels,secondary_submitted_voxels,"
        << "primary_lod0,primary_lod1,primary_lod2,secondary_lod0,secondary_lod1,secondary_lod2,"
        << "total_executed_fixed_steps,total_logical_updated_voxels,"
        << "speedup_vs_matching_single_gpu,two_device_nominal_efficiency,"
        << "visual_validation_passed,skip_reason\n";

    summary << std::fixed << std::setprecision(6);
    for (const auto& [key, row] : aggregates)
    {
        summary << EscapeCsv(row.RequestedMode) << ','
            << EscapeCsv(row.ActualMode) << ','
            << EscapeCsv(row.PrimaryAdapterName) << ','
            << EscapeCsv(row.SecondaryAdapterName) << ','
            << row.TotalVoxelCount << ','
            << row.RequestedLabelCount << ','
            << row.RequestedStaticBudget << ','
            << row.RequestedDynamicBudget << ','
            << row.TotalVoxelCount << ','
            << row.SecondaryShare << ','
            << EscapeCsv(row.ProfileName) << ','
            << EscapeCsv(row.PartitionStrategy) << ','
            << EscapeCsv(row.LoadBalanceScenario) << ','
            << EscapeCsv(row.BenchmarkConfigClass) << ','
            << EscapeCsv(row.TemporalPolicy) << ','
            << EscapeCsv(row.SpatialLodPolicy) << ','
            << EscapeCsv(row.PairId) << ','
            << row.ActualStaticVoxelCount << ','
            << row.ActualDynamicVoxelCount << ','
            << row.ActualStaticVoxelCount << ','
            << row.ActualDynamicVoxelCount << ','
            << EscapeCsv(row.ResolvedConfigHash) << ','
            << row.RenderWidth << ','
            << row.RenderHeight << ','
            << row.RepetitionCount << ','
            << row.MeasuredFrameCount << ','
            << row.ValidFrameCount << ','
            << row.InvalidFrameCount << ','
            << EscapeCsv(row.Preset) << ','
            << (row.Valid ? "true" : "false") << ','
            << EscapeCsv(row.ValidityReason) << ','
            << EscapeCsv(row.SpeedupStatistic) << ','
            << row.AverageCpuFrameMs << ','
            << row.MedianCpuFrameMs << ','
            << row.P95CpuFrameMs << ','
            << row.P99CpuFrameMs << ','
            << row.StdDevCpuFrameMs << ','
            << row.CpuFrameCi95HalfWidthMs << ','
            << row.CriticalPathGpuMs << ','
            << row.GpuWorkSumMs << ','
            << row.PrimaryComputeMs << ','
            << row.PrimaryLodCompactionMs << ','
            << row.SecondaryComputeMs << ','
            << row.SecondaryLodCompactionMs << ','
            << row.PrimaryGraphicsMs << ','
            << row.SecondaryGraphicsMs << ','
            << row.TransferMs << ','
            << row.CompositeMs << ','
            << row.AverageTransferBytes << ','
            << row.AverageColorTransferBytes << ','
            << row.AverageDepthTransferBytes << ','
            << row.AverageParticleTransferBytes << ','
            << row.AverageRenderOutputTransferBytes << ','
            << row.AverageSecondaryDrawCalls << ','
            << row.AveragePrimarySubmittedVoxelCount << ','
            << row.AverageSecondarySubmittedVoxelCount << ','
            << row.AveragePrimaryLod0Count << ','
            << row.AveragePrimaryLod1Count << ','
            << row.AveragePrimaryLod2Count << ','
            << row.AverageSecondaryLod0Count << ','
            << row.AverageSecondaryLod1Count << ','
            << row.AverageSecondaryLod2Count << ','
            << row.TotalExecutedFixedSteps << ','
            << row.TotalLogicalUpdatedVoxelCount << ','
            << row.SpeedupVsMatchingSingleGpu << ','
            << row.Efficiency << ','
            << (row.VisualValidationPassed ? "true" : "false") << ','
            << EscapeCsv(row.SkipReason) << '\n';
    }

    return true;
}
