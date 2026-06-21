#include "Source/Benchmark/AutomaticBenchmarkRunner.h"

#include <algorithm>
#include <iterator>
#include <random>
#include <utility>

std::vector<AutomaticBenchmarkConfig> AutomaticBenchmarkRunner::BuildDefaultConfigs()
{
    struct Preset
    {
        const char* Name;
        uint32_t Total;
    };

    constexpr Preset presets[] = {
        {"Low", 100000},
        {"Medium", 250000},
        {"High", 500000},
        {"VeryHigh", 1000000}
    };

    struct Mode
    {
        VoxelExecutionMode Value;
        const char* Name;
        uint32_t TemporalInterval;
    };

    constexpr Mode modes[] = {
        {VoxelExecutionMode::SingleGpuFull, "SingleGpuFull", 1},
        {VoxelExecutionMode::MultiGpuFull, "MultiGpuFull", 1},
        {VoxelExecutionMode::SingleGpuTemporalDecimation, "SingleGpuTemporalDecimation", 4},
        {VoxelExecutionMode::MultiGpuTemporalDecimation, "MultiGpuTemporalDecimation", 4}
    };

    constexpr float secondaryShares[] = {0.25f, 0.50f, 0.75f};
    constexpr bool spatialLodModes[] = {false, true};
    constexpr uint32_t repetitions = 3;
    constexpr uint32_t benchmarkSeed = 0x5eed2026u;

    std::vector<AutomaticBenchmarkConfig> configs;
    configs.reserve(std::size(presets) * std::size(modes) * std::size(secondaryShares) *
                    std::size(spatialLodModes) * repetitions);

    for (const auto& mode : modes)
    {
        for (const auto& preset : presets)
        {
            for (const float share : secondaryShares)
            {
                for (const bool spatialLodEnabled : spatialLodModes)
                {
                    for (uint32_t repetition = 0; repetition < repetitions; ++repetition)
                    {
                        configs.push_back({
                            mode.Value,
                            mode.Name,
                            preset.Name,
                            preset.Total,
                            share,
                            spatialLodEnabled,
                            mode.TemporalInterval,
                            repetition
                        });
                    }
                }
            }
        }
    }

    std::mt19937 rng(benchmarkSeed);
    std::shuffle(configs.begin(), configs.end(), rng);
    return configs;
}
