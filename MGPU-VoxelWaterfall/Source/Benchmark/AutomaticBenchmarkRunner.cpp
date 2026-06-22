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

    uint32_t MixedDynamicBudgetForStaticBudget(const uint32_t staticBudget)
    {
        if (staticBudget <= 100000)
            return 25000;
        if (staticBudget <= 250000)
            return 100000;
        if (staticBudget <= 500000)
            return 250000;
        return 500000;
    }

    std::string BuildSessionId(const BenchmarkSuite suite, const uint32_t seed)
    {
        std::ostringstream stream;
        stream << AutomaticBenchmarkRunner::SuiteName(suite) << ":seed" << std::hex << seed;
        return stream.str();
    }

    std::string CountToken(const uint32_t labelCount,
                           const uint32_t staticBudget,
                           const uint32_t dynamicBudget)
    {
        std::ostringstream stream;
        stream << "label" << labelCount << ":static" << staticBudget << ":dynamic" << dynamicBudget;
        return stream.str();
    }

    std::string BuildPairId(const ModeSpec& mode, const char* preset, const uint32_t labelCount,
                            const uint32_t staticBudget, const uint32_t dynamicBudget,
                            const float share, const bool lod)
    {
        std::ostringstream stream;
        stream << mode.PairPrefix << ':' << preset << ':' << CountToken(labelCount, staticBudget, dynamicBudget)
               << ':' << ShareToken(share)
               << ':' << LodToken(lod) << ":temporal" << mode.TemporalInterval;
        return stream.str();
    }

    std::string BuildBlockId(const ModeSpec& mode, const char* preset, const uint32_t labelCount,
                             const uint32_t staticBudget, const uint32_t dynamicBudget,
                             const float share, const bool lod, const uint32_t repetition)
    {
        std::ostringstream stream;
        stream << BuildPairId(mode, preset, labelCount, staticBudget, dynamicBudget, share, lod)
               << ":rep" << repetition;
        return stream.str();
    }

    std::string BuildConfigId(const AutomaticBenchmarkConfig& config)
    {
        std::ostringstream stream;
        stream << AutomaticBenchmarkRunner::SuiteName(config.Suite) << ':'
               << config.ModeName << ':' << config.Preset << ':'
               << CountToken(config.RequestedLabelCount,
                             config.RequestedStaticBudget,
                             config.RequestedDynamicBudget) << ':'
               << ShareToken(config.SecondaryShare) << ':' << LodToken(config.SpatialLodEnabled)
               << ":temporal" << config.TemporalInterval << ":rep" << config.Repetition;
        return stream.str();
    }

    AutomaticBenchmarkConfig MakeConfig(BenchmarkSuite suite,
                                        const ModeSpec& mode,
                                        const char* preset,
                                        const uint32_t labelCount,
                                        const float share,
                                        const bool lod,
                                        const uint32_t repetition,
                                        const uint32_t repetitionCount,
                                        const uint32_t warmupFrames,
                                        const uint32_t measuredFrames,
                                        const uint32_t seed,
                                        const std::string& sessionId,
                                        const std::string& blockId)
    {
        AutomaticBenchmarkConfig config{};
        config.Suite = suite;
        config.Mode = mode.Mode;
        config.ModeName = mode.Name;
        config.Preset = preset;
        config.RequestedLabelCount = labelCount;
        config.RequestedStaticBudget = labelCount;
        config.RequestedDynamicBudget = MixedDynamicBudgetForStaticBudget(labelCount);
        config.TotalCount = labelCount;
        config.SecondaryShare = share;
        config.SpatialLodEnabled = lod;
        config.TemporalInterval = mode.TemporalInterval;
        config.Repetition = repetition;
        config.RepetitionCount = repetitionCount;
        config.WarmupFrameCount = warmupFrames;
        config.MeasuredFrameCount = measuredFrames;
        config.RandomizationSeed = seed;
        config.SessionId = sessionId;
        config.BlockId = blockId;
        config.PairId = BuildPairId(mode, preset,
                                    config.RequestedLabelCount,
                                    config.RequestedStaticBudget,
                                    config.RequestedDynamicBudget,
                                    share, lod);
        config.ConfigId = BuildConfigId(config);
        return config;
    }

    struct BenchmarkBlock
    {
        std::vector<AutomaticBenchmarkConfig> Members;
    };

    void AppendPairedBlock(std::vector<BenchmarkBlock>& blocks,
                           const BenchmarkSuite suite,
                           const char* preset,
                           const uint32_t labelCount,
                           const float share,
                           const bool lod,
                           const uint32_t repetition,
                           const uint32_t repetitionCount,
                           const uint32_t warmupFrames,
                           const uint32_t measuredFrames,
                           const uint32_t seed,
                           const std::string& sessionId,
                           const ModeSpec& single,
                           const ModeSpec& multi)
    {
        BenchmarkBlock block;
        const uint32_t staticBudget = labelCount;
        const uint32_t dynamicBudget = MixedDynamicBudgetForStaticBudget(labelCount);
        const auto blockId = BuildBlockId(single, preset, labelCount, staticBudget, dynamicBudget, share, lod, repetition);
        block.Members.push_back(MakeConfig(suite, single, preset, labelCount, share, lod,
                                           repetition, repetitionCount, warmupFrames,
                                           measuredFrames, seed, sessionId, blockId));
        block.Members.push_back(MakeConfig(suite, multi, preset, labelCount, share, lod,
                                           repetition, repetitionCount, warmupFrames,
                                           measuredFrames, seed, sessionId, blockId));
        blocks.push_back(std::move(block));
    }

    std::vector<AutomaticBenchmarkConfig> FlattenRandomizedBlocks(std::vector<BenchmarkBlock> blocks,
                                                                  const uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::shuffle(blocks.begin(), blocks.end(), rng);

        std::vector<AutomaticBenchmarkConfig> configs;
        size_t order = 0;
        for (uint32_t blockOrder = 0; blockOrder < blocks.size(); ++blockOrder)
        {
            auto& block = blocks[blockOrder];
            const bool multiFirst = (rng() & 1u) != 0u;
            if (multiFirst && block.Members.size() == 2)
                std::swap(block.Members[0], block.Members[1]);

            for (uint32_t member = 0; member < block.Members.size(); ++member)
            {
                auto config = block.Members[member];
                config.OrderIndex = static_cast<uint32_t>(order++);
                config.BlockOrderIndex = blockOrder;
                config.PairMemberOrder = member;
                configs.push_back(std::move(config));
            }
        }
        return configs;
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

    std::vector<BenchmarkBlock> blocks;
    if (suite == BenchmarkSuite::Smoke)
    {
        const uint32_t seed = seedOverride != 0 ? seedOverride : SmokeSeed;
        const auto sessionId = BuildSessionId(suite, seed);
        constexpr uint32_t total = 100000;
        constexpr float share = 0.50f;
        constexpr bool lodModes[] = {false, true};
        blocks.reserve(4);
        for (const bool lod : lodModes)
        {
            AppendPairedBlock(blocks, suite, "LowCanonical", total, share, lod, 0, 1,
                              30, 120, seed, sessionId, smokeModes[0], smokeModes[1]);
            AppendPairedBlock(blocks, suite, "LowCanonical", total, share, lod, 0, 1,
                              30, 120, seed, sessionId, smokeModes[2], smokeModes[3]);
        }
    }
    else
    {
        const uint32_t seed = seedOverride != 0 ? seedOverride : FullSeed;
        const auto sessionId = BuildSessionId(suite, seed);
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
        blocks.reserve(std::size(presets) * 2 * std::size(shares) *
                       std::size(lodModes) * repetitions);
        for (uint32_t family = 0; family < std::size(fullModes); family += 2)
        {
            for (const auto& preset : presets)
            {
                for (const float share : shares)
                {
                    for (const bool lod : lodModes)
                    {
                        for (uint32_t repetition = 0; repetition < repetitions; ++repetition)
                        {
                            AppendPairedBlock(blocks, suite, preset.Name, preset.Total, share, lod,
                                              repetition, repetitions, 100, 500, seed, sessionId,
                                              fullModes[family], fullModes[family + 1]);
                        }
                    }
                }
            }
        }
    }

    return FlattenRandomizedBlocks(std::move(blocks), seedOverride != 0
                                                        ? seedOverride
                                                        : (suite == BenchmarkSuite::Smoke ? SmokeSeed : FullSeed));
}

std::vector<AutomaticBenchmarkConfig> AutomaticBenchmarkRunner::BuildDefaultConfigs()
{
    return BuildConfigs(BenchmarkSuite::Full);
}
