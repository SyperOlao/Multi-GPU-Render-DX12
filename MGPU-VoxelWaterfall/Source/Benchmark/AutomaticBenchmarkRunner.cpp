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

    constexpr std::pair<VoxelExecutionMode, const char*> modes[] = {
        {VoxelExecutionMode::SingleGpuFull, "SingleGpuFull"},
        {VoxelExecutionMode::MultiGpuFull, "MultiGpuFull"},
        {VoxelExecutionMode::SingleGpuTemporalDecimation, "SingleGpuTemporalDecimation"},
        {VoxelExecutionMode::MultiGpuTemporalDecimation, "MultiGpuTemporalDecimation"}
    };

    constexpr float secondaryShares[] = {0.25f, 0.50f, 0.75f};
    constexpr uint32_t repetitions = 3;
    constexpr uint32_t benchmarkSeed = 0x5eed2026u;

    std::vector<AutomaticBenchmarkConfig> configs;
    configs.reserve(std::size(presets) * std::size(modes) * std::size(secondaryShares) * repetitions);

    for (const auto& mode : modes)
    {
        for (const auto& preset : presets)
        {
            for (const float share : secondaryShares)
            {
                for (uint32_t repetition = 0; repetition < repetitions; ++repetition)
                {
                    configs.push_back({
                        mode.first,
                        mode.second,
                        preset.Name,
                        preset.Total,
                        share,
                        repetition
                    });
                }
            }
        }
    }

    std::mt19937 rng(benchmarkSeed);
    std::shuffle(configs.begin(), configs.end(), rng);
    return configs;
}
