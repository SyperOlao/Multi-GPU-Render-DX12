#pragma once

#include "VoxelBenchmarkProfiler.h"

#include <filesystem>
#include <vector>

class BenchmarkCsvWriter
{
public:
    static bool WriteAutomaticSummary(const std::filesystem::path& outputPath,
                                      const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries);
};

