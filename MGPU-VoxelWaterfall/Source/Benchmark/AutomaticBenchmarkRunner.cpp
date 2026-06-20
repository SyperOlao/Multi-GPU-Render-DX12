#include "Source/Benchmark/AutomaticBenchmarkRunner.h"

#include <iterator>
#include <utility>

std::vector<AutomaticBenchmarkConfig> AutomaticBenchmarkRunner::BuildDefaultConfigs()
{
    struct Preset
    {
        const char* Name;
        int Near;
        int Medium;
        int Far;
    };

    constexpr Preset presets[] = {
        {"Low", 76000, 19000, 5000},
        {"Medium", 190000, 47500, 12500},
        {"High", 380000, 95000, 25000},
        {"VeryHigh", 760000, 190000, 50000}
    };

    constexpr std::pair<VoxelExecutionMode, const char*> modes[] = {
        {VoxelExecutionMode::PrimaryOnly, "PrimaryOnly"},
        {VoxelExecutionMode::SplitMultiGpu, "SplitMultiGpu"},
        {VoxelExecutionMode::SplitMultiGpuLod, "SplitMultiGpuLod"}
    };

    std::vector<AutomaticBenchmarkConfig> configs;
    configs.reserve(std::size(presets) * std::size(modes));

    for (const auto& mode : modes)
    {
        for (const auto& preset : presets)
        {
            configs.push_back({
                mode.first,
                mode.second,
                preset.Name,
                preset.Near,
                preset.Medium,
                preset.Far,
                static_cast<uint32_t>(preset.Near + preset.Medium + preset.Far)
            });
        }
    }

    return configs;
}
