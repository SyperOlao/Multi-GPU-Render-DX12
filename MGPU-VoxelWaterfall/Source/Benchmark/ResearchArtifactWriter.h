#pragma once

#include "Source/Benchmark/AutomaticBenchmarkRunner.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"

#include <filesystem>
#include <string>
#include <vector>

struct BenchmarkResearchArtifactContext
{
    BenchmarkSuite Suite = BenchmarkSuite::Smoke;
    std::string RunId;
    uint32_t RandomizationSeed = 0;
    uint32_t WarmupFrames = 0;
    uint32_t MeasuredFrames = 0;
    size_t ExecutionCount = 0;
    std::string CurrentBuildHash;
    std::string CurrentShaderHash;
    std::string ValidationRunId;
    std::string TwoGpuVerificationRunId;
    std::string ValidationAdapterPairIdentity;
    std::string TwoGpuAdapterPairIdentity;
    std::string GateStatus;
    std::string GateReason;
    std::filesystem::path OutputDirectory;
    std::filesystem::path StatusPath;
    std::filesystem::path SummaryPath;
};

class ResearchArtifactWriter
{
public:
    static void WriteSuiteManifest(const BenchmarkResearchArtifactContext& context,
                                   const std::vector<AutomaticBenchmarkConfig>& configs);

    static void WriteEnvironment(const BenchmarkResearchArtifactContext& context,
                                 const VoxelBenchmarkProfiler::FrameMetadata& metadata);

    static void WriteInvalidRecords(const BenchmarkResearchArtifactContext& context,
                                    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries);

    static void WriteRunsCsv(const BenchmarkResearchArtifactContext& context,
                             const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries);

    static void WriteRawFramesCsv(const BenchmarkResearchArtifactContext& context,
                                  const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries);

    static void WritePairedRunsCsv(const BenchmarkResearchArtifactContext& context,
                                   const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries);

    static void WriteTelemetryCsv(const BenchmarkResearchArtifactContext& context);

    static void WriteMemoryTimelineCsv(const BenchmarkResearchArtifactContext& context);

    static void WriteReproductionReadme(const BenchmarkResearchArtifactContext& context);
};
