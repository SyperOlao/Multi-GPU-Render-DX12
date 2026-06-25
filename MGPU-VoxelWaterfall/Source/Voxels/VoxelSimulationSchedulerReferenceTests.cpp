#include "Source/Voxels/VoxelSimulationSchedulerReferenceTests.h"

#if defined(DEBUG) || defined(_DEBUG)

#include "Source/Voxels/VoxelSimulationScheduler.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace
{
    constexpr double FixedStep = 1.0 / 60.0;

    bool NearlyEqual(const double left, const double right, const double epsilon = 1.0e-6)
    {
        return std::abs(left - right) <= epsilon;
    }

    uint32_t CountTemporalDispatches(const uint32_t frameCount, const uint32_t interval)
    {
        uint32_t dispatches = 0;
        bool started = false;
        for (uint64_t step = 0; step < frameCount; ++step)
        {
            if (VoxelSimulationScheduler::ShouldRunTemporalUpdate(step, interval, started))
            {
                ++dispatches;
                started = true;
            }
        }
        return dispatches;
    }

    uint32_t LogicalWorkForPair(const uint32_t frameCount,
                                const uint32_t primaryVoxels,
                                const uint32_t secondaryVoxels)
    {
        uint32_t logicalWork = 0;
        for (uint64_t step = 0; step < frameCount; ++step)
            logicalWork += primaryVoxels + secondaryVoxels;
        return logicalWork;
    }
}

void RunVoxelSimulationSchedulerReferenceTests()
{
    {
        const auto plan = VoxelSimulationScheduler::BuildStepPlan(
            VoxelSimulationSchedulerMode::Interactive, FixedStep, 0.0, 3, 1);
        assert(plan.RequestedFixedSteps == 1);
        assert(plan.ExecutedFixedSteps == 1);
        assert(plan.DroppedStepCount == 0);
        assert(NearlyEqual(plan.OutputAccumulator, 0.0));
    }

    {
        const auto plan = VoxelSimulationScheduler::BuildStepPlan(
            VoxelSimulationSchedulerMode::Interactive, 0.100, 0.0, 3, 1);
        assert(plan.RequestedFixedSteps == 6);
        assert(plan.ExecutedFixedSteps == 3);
        assert(plan.DroppedStepCount == 3);
        assert(plan.DroppedSimulationTime > 0.049 && plan.DroppedSimulationTime < 0.051);
        assert(NearlyEqual(plan.OutputAccumulator, 0.0));
    }

    {
        const auto plan = VoxelSimulationScheduler::BuildStepPlan(
            VoxelSimulationSchedulerMode::Interactive, 1.0, 0.0, 3, 1);
        assert(plan.RequestedFixedSteps == 60);
        assert(plan.ExecutedFixedSteps == 3);
        assert(plan.DroppedStepCount == 57);
        assert(plan.DroppedSimulationTime > 0.94 && plan.DroppedSimulationTime < 0.96);
    }

    {
        const auto plan = VoxelSimulationScheduler::BuildStepPlan(
            VoxelSimulationSchedulerMode::Benchmark, 1.0, 0.5, 3, 1);
        assert(plan.RequestedFixedSteps == 1);
        assert(plan.ExecutedFixedSteps == 1);
        assert(plan.DroppedStepCount == 0);
        assert(NearlyEqual(plan.OutputAccumulator, 0.0));
        assert(NearlyEqual(plan.InterpolationAlpha, 0.0));
    }

    {
        double wallTimeBetweenSuccessfulFrames = 0.0;
        for (uint32_t poll = 0; poll < 30; ++poll)
            wallTimeBetweenSuccessfulFrames += 0.001;
        wallTimeBetweenSuccessfulFrames += 6.0 * FixedStep;

        const auto plan = VoxelSimulationScheduler::BuildStepPlan(
            VoxelSimulationSchedulerMode::Interactive,
            wallTimeBetweenSuccessfulFrames,
            0.0,
            16,
            1);
        assert(plan.RequestedFixedSteps == 7);
        assert(plan.ExecutedFixedSteps == 7);
        assert(plan.DroppedStepCount == 0);
        assert(plan.OutputAccumulator > 0.013 && plan.OutputAccumulator < 0.014);
    }

    assert(CountTemporalDispatches(8, 1) == 8);
    assert(CountTemporalDispatches(8, 2) == 4);
    assert(CountTemporalDispatches(8, 4) == 2);

    constexpr uint32_t frameCount = 16;
    constexpr uint32_t primaryVoxels = 700;
    constexpr uint32_t secondaryVoxels = 300;
    const uint32_t fullLogicalWork = LogicalWorkForPair(frameCount, primaryVoxels, secondaryVoxels);
    const uint32_t temporalLogicalWork = LogicalWorkForPair(frameCount, primaryVoxels, secondaryVoxels);
    assert(fullLogicalWork == temporalLogicalWork);
    assert(CountTemporalDispatches(frameCount, 4) < CountTemporalDispatches(frameCount, 1));
    assert(CountTemporalDispatches(frameCount, 4) * secondaryVoxels <
           CountTemporalDispatches(frameCount, 1) * secondaryVoxels);
}

#endif
