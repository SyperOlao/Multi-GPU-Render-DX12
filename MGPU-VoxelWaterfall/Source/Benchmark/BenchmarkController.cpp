#include "Source/Benchmark/BenchmarkController.h"

#include "Source/Benchmark/BenchmarkCsvWriter.h"

#include <cstring>

namespace
{
    std::wstring ToWide(const char* value)
    {
        return std::wstring(value, value + std::strlen(value));
    }
}

void BenchmarkController::StartManual(const BenchmarkControllerContext& context)
{
    benchmarkVSyncWasEnabled = context.IsVSyncEnabled();
    if (benchmarkVSyncWasEnabled)
        context.SetVSync(false);

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

void BenchmarkController::StartAutomatic(const BenchmarkControllerContext& context)
{
    StopAutomatic(context);

    automaticBenchmarkConfigs.clear();
    automaticBenchmarkSummaries.clear();
    automaticBenchmarkIndex = 0;
    automaticBenchmarkStopRequested = false;
    automaticBenchmarkConfigs = AutomaticBenchmarkRunner::BuildDefaultConfigs();

    benchmarkVSyncWasEnabled = context.IsVSyncEnabled();
    if (benchmarkVSyncWasEnabled)
        context.SetVSync(false);

    automaticBenchmarkSummaryPath = benchmarkDirectory / "VoxelBenchmark_Summary.csv";
    automaticBenchmarkActive = true;
    context.Log(L"\nAutomatic voxel benchmark started");
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
            summary.SpatialLodPolicy = config.SpatialLodEnabled ? "SpatialDensityThreeLevel" : "Off";
            summary.TemporalPolicy =
                config.TemporalInterval <= 1 ? "Full" : "TemporalDecimation";
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
    const bool requestsMultiGpu = config.Mode == VoxelExecutionMode::MultiGpuFull ||
        config.Mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
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
        skipped.SpatialLodPolicy = config.SpatialLodEnabled ? "SpatialDensityThreeLevel" : "Off";
        skipped.TemporalPolicy = config.TemporalInterval <= 1 ? "Full" : "TemporalDecimation";
        skipped.Repetition = config.Repetition;
        skipped.Valid = false;
        skipped.SkipReason = "secondary hardware adapter unavailable";
        automaticBenchmarkSummaries.push_back(skipped);
        ++automaticBenchmarkIndex;
        return;
    }

    context.Flush();
    context.ApplyExecutionMode(config.Mode);
    ApplyBenchmarkVoxelCount(context, static_cast<int>(config.TotalCount));
    if (context.ApplySecondaryShare)
        context.ApplySecondaryShare(config.SecondaryShare);
    if (context.ApplySpatialLodEnabled)
        context.ApplySpatialLodEnabled(config.SpatialLodEnabled);
    if (context.ApplyTemporalInterval)
        context.ApplyTemporalInterval(config.TemporalInterval);
    context.Flush();

    const std::string fileName = "VoxelBenchmark_" + std::string(config.ModeName) + "_" +
        config.Preset + "_" + std::to_string(config.TotalCount) +
        "_share" + std::to_string(static_cast<int>(config.SecondaryShare * 100.0f)) +
        "_" + (config.SpatialLodEnabled ? "SpatialLOD" : "NoSpatialLOD") +
        "_temporal" + std::to_string(config.TemporalInterval) +
        "_rep" + std::to_string(config.Repetition) + ".csv";
    if (!context.Profiler.Start(benchmarkDirectory, context.BuildMetadata(), fileName,
                                config.Preset, config.Repetition))
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
    if (automaticBenchmarkSummaries.empty())
        return;

    if (BenchmarkCsvWriter::WriteAutomaticSummary(automaticBenchmarkSummaryPath, automaticBenchmarkSummaries))
        context.Log(L"\nAutomatic benchmark summary written: " + automaticBenchmarkSummaryPath.wstring());
}
