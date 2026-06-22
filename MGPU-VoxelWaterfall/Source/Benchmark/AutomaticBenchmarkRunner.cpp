#include "Source/Benchmark/AutomaticBenchmarkRunner.h"

#include <algorithm>
#include <iterator>
#include <random>
#include <sstream>

namespace
{
    struct ModeSpec
    {
        VoxelExecutionMode Mode;
        const char* Name;
        const char* PairPrefix;
        uint32_t TemporalInterval;
    };

    std::string LodToken(const bool enabled)
    {
        return enabled ? "lod_on" : "lod_off";
    }

    std::string ShareToken(const float share)
    {
        return "share" + std::to_string(static_cast<int>(share * 100.0f + 0.5f));
    }

    std::string BuildPairId(const ModeSpec& mode, const char* preset, const uint32_t total,
                            const float share, const bool lod)
    {
        std::ostringstream stream;
        stream << mode.PairPrefix << ':' << preset << ':' << total << ':' << ShareToken(share)
               << ':' << LodToken(lod) << ":temporal" << mode.TemporalInterval;
        return stream.str();
    }

    std::string BuildConfigId(const AutomaticBenchmarkConfig& config)
    {
        std::ostringstream stream;
        stream << AutomaticBenchmarkRunner::SuiteName(config.Suite) << ':'
               << config.ModeName << ':' << config.Preset << ':' << config.TotalCount << ':'
               << ShareToken(config.SecondaryShare) << ':' << LodToken(config.SpatialLodEnabled)
               << ":temporal" << config.TemporalInterval << ":rep" << config.Repetition;
        return stream.str();
    }

    AutomaticBenchmarkConfig MakeConfig(BenchmarkSuite suite,
                                        const ModeSpec& mode,
                                        const char* preset,
                                        const uint32_t total,
                                        const float share,
                                        const bool lod,
                                        const uint32_t repetition,
                                        const uint32_t repetitionCount,
                                        const uint32_t warmupFrames,
                                        const uint32_t measuredFrames,
                                        const uint32_t seed)
    {
        AutomaticBenchmarkConfig config{};
        config.Suite = suite;
        config.Mode = mode.Mode;
        config.ModeName = mode.Name;
        config.Preset = preset;
        config.TotalCount = total;
        config.SecondaryShare = share;
        config.SpatialLodEnabled = lod;
        config.TemporalInterval = mode.TemporalInterval;
        config.Repetition = repetition;
        config.RepetitionCount = repetitionCount;
        config.WarmupFrameCount = warmupFrames;
        config.MeasuredFrameCount = measuredFrames;
        config.RandomizationSeed = seed;
        config.PairId = BuildPairId(mode, preset, total, share, lod);
        config.ConfigId = BuildConfigId(config);
        return config;
    }
}

const char* AutomaticBenchmarkRunner::SuiteName(const BenchmarkSuite suite)
{
    switch (suite)
    {
    case BenchmarkSuite::Smoke:
        return "Smoke";
    case BenchmarkSuite::Full:
        return "Full";
    default:
        return "Full";
    }
}

std::vector<AutomaticBenchmarkConfig> AutomaticBenchmarkRunner::BuildConfigs(
    const BenchmarkSuite suite,
    const uint32_t seedOverride)
{
    constexpr ModeSpec smokeModes[] = {
        {VoxelExecutionMode::SingleGpuFull, "SingleGpuFull", "Full", 2},
        {VoxelExecutionMode::MultiGpuFull, "MultiGpuFull", "Full", 2},
        {VoxelExecutionMode::SingleGpuTemporalDecimation, "SingleGpuTemporalDecimation", "Temporal", 2},
        {VoxelExecutionMode::MultiGpuTemporalDecimation, "MultiGpuTemporalDecimation", "Temporal", 2}
    };

    constexpr ModeSpec fullModes[] = {
        {VoxelExecutionMode::SingleGpuFull, "SingleGpuFull", "Full", 1},
        {VoxelExecutionMode::MultiGpuFull, "MultiGpuFull", "Full", 1},
        {VoxelExecutionMode::SingleGpuTemporalDecimation, "SingleGpuTemporalDecimation", "Temporal", 4},
        {VoxelExecutionMode::MultiGpuTemporalDecimation, "MultiGpuTemporalDecimation", "Temporal", 4}
    };

    std::vector<AutomaticBenchmarkConfig> configs;
    if (suite == BenchmarkSuite::Smoke)
    {
        const uint32_t seed = seedOverride != 0 ? seedOverride : SmokeSeed;
        constexpr uint32_t total = 100000;
        constexpr float share = 0.50f;
        constexpr bool lodModes[] = {false, true};
        configs.reserve(std::size(smokeModes) * std::size(lodModes));
        for (const auto& mode : smokeModes)
        {
            for (const bool lod : lodModes)
            {
                configs.push_back(MakeConfig(
                    suite, mode, "LowCanonical", total, share, lod, 0, 1, 30, 120, seed));
            }
        }
    }
    else
    {
        const uint32_t seed = seedOverride != 0 ? seedOverride : FullSeed;
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
        constexpr float shares[] = {0.25f, 0.50f, 0.75f};
        constexpr bool lodModes[] = {false, true};
        constexpr uint32_t repetitions = 3;
        configs.reserve(std::size(presets) * std::size(fullModes) * std::size(shares) *
                        std::size(lodModes) * repetitions);
        for (const auto& mode : fullModes)
        {
            for (const auto& preset : presets)
            {
                for (const float share : shares)
                {
                    for (const bool lod : lodModes)
                    {
                        for (uint32_t repetition = 0; repetition < repetitions; ++repetition)
                        {
                            configs.push_back(MakeConfig(
                                suite, mode, preset.Name, preset.Total, share, lod,
                                repetition, repetitions, 100, 500, seed));
                        }
                    }
                }
            }
        }

        std::mt19937 rng(seed);
        std::shuffle(configs.begin(), configs.end(), rng);
    }

    for (uint32_t i = 0; i < configs.size(); ++i)
        configs[i].OrderIndex = i;
    return configs;
}

std::vector<AutomaticBenchmarkConfig> AutomaticBenchmarkRunner::BuildDefaultConfigs()
{
    return BuildConfigs(BenchmarkSuite::Full);
}
