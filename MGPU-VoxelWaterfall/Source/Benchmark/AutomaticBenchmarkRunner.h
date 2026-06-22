#pragma once

#include "Source/Voxels/VoxelTypes.h"

#include <cstdint>
#include <string>
#include <vector>

enum class BenchmarkSuite : uint32_t
{
    Smoke,
    Full
};

struct AutomaticBenchmarkConfig
{
    BenchmarkSuite Suite = BenchmarkSuite::Full;
    VoxelExecutionMode Mode = VoxelExecutionMode::SingleGpuFull;
    const char* ModeName = "SingleGpuFull";
    const char* Preset = "Low";
    uint32_t RequestedLabelCount = 0;
    uint32_t RequestedStaticBudget = 0;
    uint32_t RequestedDynamicBudget = 0;
    uint32_t TotalCount = 0;
    float SecondaryShare = 0.25f;
    bool SpatialLodEnabled = false;
    uint32_t TemporalInterval = 1;
    uint32_t Repetition = 0;
    uint32_t RepetitionCount = 1;
    uint32_t WarmupFrameCount = 100;
    uint32_t MeasuredFrameCount = 500;
    uint32_t RandomizationSeed = 0;
    uint32_t OrderIndex = 0;
    uint32_t BlockOrderIndex = 0;
    uint32_t PairMemberOrder = 0;
    std::string SessionId;
    std::string BlockId;
    std::string PairId;
    std::string ConfigId;
};

struct BenchmarkConfigurationApplyResult
{
    bool Passed = false;
    std::string Reason;
    std::string ResolvedConfigHash;
    uint32_t RequestedLabelCount = 0;
    uint32_t RequestedStaticBudget = 0;
    uint32_t RequestedDynamicBudget = 0;
    uint32_t ActualStaticCount = 0;
    uint32_t ActualDynamicCount = 0;
    uint32_t ActualTotalCount = 0;
    std::string ActualMode;
};

class AutomaticBenchmarkRunner
{
public:
    static constexpr uint32_t SmokeSeed = 0x51a0c001u;
    static constexpr uint32_t FullSeed = 0x5eed2026u;

    static const char* SuiteName(BenchmarkSuite suite);
    static std::vector<AutomaticBenchmarkConfig> BuildConfigs(BenchmarkSuite suite,
                                                              uint32_t seedOverride = 0,
                                                              uint32_t repetitionOverride = 0);
    static std::vector<AutomaticBenchmarkConfig> BuildDefaultConfigs();
};
