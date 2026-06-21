#pragma once

#include "Source/Voxels/VoxelTypes.h"

#include <cstdint>
#include <vector>

struct AutomaticBenchmarkConfig
{
    VoxelExecutionMode Mode = VoxelExecutionMode::SingleGpuFull;
    const char* ModeName = "SingleGpuFull";
    const char* Preset = "Low";
    uint32_t TotalCount = 0;
    float SecondaryShare = 0.25f;
    bool SpatialLodEnabled = false;
    uint32_t TemporalInterval = 1;
    uint32_t Repetition = 0;
};

class AutomaticBenchmarkRunner
{
public:
    static std::vector<AutomaticBenchmarkConfig> BuildDefaultConfigs();
};
