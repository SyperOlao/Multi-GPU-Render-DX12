#pragma once

#include <cstdint>
#include <filesystem>

enum class ResearchRunnerSuite : uint32_t
{
    Validation,
    TwoGpuVerify,
    Smoke,
    Full,
    ProfileSweep,
    MemorySoak,
    RebuildStress
};

struct ResearchRunnerRequest
{
    ResearchRunnerSuite Suite = ResearchRunnerSuite::Smoke;
    uint32_t Seed = 0;
    uint32_t WarmupFrames = 30;
    uint32_t MeasuredFrames = 120;
    uint32_t Repetitions = 1;
    std::filesystem::path OutputDirectory;
    bool RunPrerequisites = false;
};
