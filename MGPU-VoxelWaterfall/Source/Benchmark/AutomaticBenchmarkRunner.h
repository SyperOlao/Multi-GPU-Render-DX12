#pragma once

#include "Source/Voxels/VoxelTypes.h"

#include <cstdint>
#include <vector>

struct AutomaticBenchmarkConfig
{
    VoxelExecutionMode Mode = VoxelExecutionMode::PrimaryOnly;
    const char* ModeName = "PrimaryOnly";
    const char* Preset = "Low";
    int NearCount = 0;
    int MediumCount = 0;
    int FarCount = 0;
    uint32_t TotalCount = 0;
};

class AutomaticBenchmarkRunner
{
public:
    static std::vector<AutomaticBenchmarkConfig> BuildDefaultConfigs();
};

