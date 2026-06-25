#include "Source/Voxels/VoxelWaterfallDynamicReferenceTests.h"

#if defined(DEBUG) || defined(_DEBUG)

#include "Source/Voxels/VoxelParticleSpawner.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <Windows.h>

namespace
{
    uint64_t Fnv1aAppend32(uint64_t hash, const uint32_t value)
    {
        for (uint32_t shift = 0; shift < 32u; shift += 8u)
        {
            hash ^= (value >> shift) & 0xffu;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t Fnv1aAppendSigned32(const uint64_t hash, const int32_t value)
    {
        return Fnv1aAppend32(hash, static_cast<uint32_t>(value));
    }

    DWORD HashVoxel(const DWORD value)
    {
        DWORD x = value;
        x ^= x >> 16u;
        x *= 0x7feb352du;
        x ^= x >> 15u;
        x *= 0x846ca68bu;
        x ^= x >> 16u;
        return x;
    }

    float HashUnitFloat(const DWORD value)
    {
        return static_cast<float>(HashVoxel(value) & 0x00ffffffu) / static_cast<float>(0x01000000u);
    }

    float Saturate(const float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    DirectX::SimpleMath::Vector2 SafeNormalize2(const DirectX::SimpleMath::Vector2& value)
    {
        const float lengthSquared = value.Dot(value);
        if (lengthSquared <= 1.0e-5f)
            return DirectX::SimpleMath::Vector2(1.0f, 0.0f);
        return value / std::sqrt(lengthSquared);
    }

    DirectX::SimpleMath::Vector3 DeterministicRecyclePosition(
        const DWORD voxelIndex,
        const VoxelSimulationParameters& parameters)
    {
        const float voxelSize = std::max(parameters.VoxelSize, 0.05f);
        const auto cell = VoxelParticleSpawner::ComputeSpawnGridCell(voxelIndex, parameters);
        const DWORD topLayer = HashVoxel(voxelIndex + parameters.Seed * 13u) % 3u;
        const float x = (static_cast<float>(cell.X) - 0.5f * static_cast<float>(cell.Width - 1u)) * voxelSize;
        const float y = parameters.SpawnHeight + static_cast<float>(topLayer) * voxelSize;
        const float z = (static_cast<float>(cell.Z) - 0.5f * static_cast<float>(cell.Depth - 1u)) * voxelSize;
        return DirectX::SimpleMath::Vector3(x, y, z);
    }

    DirectX::SimpleMath::Vector3 DeterministicInitialVelocity(
        const DWORD voxelIndex,
        const VoxelSimulationParameters& parameters)
    {
        const float speedVariation = 0.75f + 0.5f * HashUnitFloat(voxelIndex ^ parameters.Seed ^ 0x9e3779b9u);
        const float lateralX = (HashUnitFloat(voxelIndex ^ parameters.Seed ^ 0x85ebca6bu) - 0.5f) * 0.8f;
        const float lateralZ = (HashUnitFloat(voxelIndex ^ parameters.Seed ^ 0xc2b2ae35u) - 0.5f) * 0.45f;
        return DirectX::SimpleMath::Vector3(lateralX, -parameters.InitialFallSpeed * speedVariation, lateralZ);
    }

    struct SimulationSummary
    {
        uint32_t RecycleCount = 0;
        uint32_t CurtainOccupiedSamples = 0;
        uint64_t StateHash = 1469598103934665603ull;
    };

    VoxelSimulationParameters SmallWaterfallParameters()
    {
        VoxelSimulationParameters parameters{};
        parameters.VoxelSize = 0.35f;
        parameters.SpawnHeight = 41.0f;
        parameters.FloorHeight = 2.0f;
        parameters.WaterfallWidth = 12.0f;
        parameters.WaterfallDepth = 4.0f;
        parameters.InitialFallSpeed = 5.5f;
        parameters.Gravity = 16.0f;
        parameters.Seed = 4242;
        return parameters;
    }

    uint64_t HashFirstSpawnSamples(const VoxelSimulationParameters& parameters, const uint32_t count)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto cell = VoxelParticleSpawner::ComputeSpawnGridCell(i, parameters);
            const auto particle = VoxelParticleSpawner::Generate(i, parameters);
            hash = Fnv1aAppend32(hash, cell.X);
            hash = Fnv1aAppend32(hash, cell.Y);
            hash = Fnv1aAppend32(hash, cell.Z);
            hash = Fnv1aAppendSigned32(hash, static_cast<int32_t>(std::lround(particle.CurrentContinuousPosition.x * 1000.0f)));
            hash = Fnv1aAppendSigned32(hash, static_cast<int32_t>(std::lround(particle.CurrentContinuousPosition.y * 1000.0f)));
            hash = Fnv1aAppendSigned32(hash, static_cast<int32_t>(std::lround(particle.CurrentContinuousPosition.z * 1000.0f)));
            hash = Fnv1aAppendSigned32(hash, static_cast<int32_t>(std::lround(particle.Velocity.x * 1000.0f)));
            hash = Fnv1aAppendSigned32(hash, static_cast<int32_t>(std::lround(particle.Velocity.y * 1000.0f)));
            hash = Fnv1aAppendSigned32(hash, static_cast<int32_t>(std::lround(particle.Velocity.z * 1000.0f)));
        }
        return hash;
    }

    void SimulateParticleStep(
        VoxelParticleData& particle,
        const VoxelSimulationParameters& parameters,
        const float dt,
        const float time,
        uint32_t& recycleCount)
    {
        particle.PreviousContinuousPosition = particle.CurrentContinuousPosition;

        const float voxelSize = std::max(parameters.VoxelSize, 0.05f);
        const float phase = particle.FlowPhase;
        const float flowX = std::sin(phase + time * 0.91f + particle.CurrentContinuousPosition.y * 0.037f) * 0.85f;
        const float flowZ = std::cos(phase * 1.37f + time * 0.67f + particle.CurrentContinuousPosition.x * 0.041f) * 0.45f;
        const float floorSpread = Saturate((parameters.FloorHeight + voxelSize * 6.0f -
            particle.CurrentContinuousPosition.y) / std::max(voxelSize * 8.0f, 0.001f));
        const DirectX::SimpleMath::Vector2 currentXZ(
            particle.CurrentContinuousPosition.x,
            particle.CurrentContinuousPosition.z);
        const DirectX::SimpleMath::Vector2 stableOffset(
            HashUnitFloat(particle.GlobalVoxelId ^ parameters.Seed) - 0.5f,
            HashUnitFloat(particle.GlobalVoxelId ^ parameters.Seed ^ 0x68bc21ebu) - 0.5f);
        const auto spreadDirection = SafeNormalize2(currentXZ + stableOffset);
        const float preStepBasinContact = Saturate((parameters.FloorHeight + voxelSize * 2.5f -
            particle.CurrentContinuousPosition.y) / std::max(voxelSize * 2.5f, 0.001f));
        const DirectX::SimpleMath::Vector2 basinBounds(
            std::max(parameters.WaterfallWidth * 0.75f, voxelSize * 4.0f),
            std::max(parameters.WaterfallDepth * 2.0f, voxelSize * 6.0f));
        const DWORD poolLayer = HashVoxel(particle.GlobalVoxelId ^ parameters.Seed ^ 0x91e10da5u) % 5u;
        const float poolSurfaceY = parameters.FloorHeight + voxelSize * (0.35f + 0.28f * static_cast<float>(poolLayer));
        const float basinResidenceSeconds = 3.25f +
            2.25f * HashUnitFloat(particle.GlobalVoxelId ^ parameters.Seed ^ 0x4cf5ad43u);

        particle.Velocity.y += -parameters.Gravity * dt;
        particle.Velocity.x += (flowX + spreadDirection.x * floorSpread * 4.0f) * dt;
        particle.Velocity.z += (flowZ + spreadDirection.y * floorSpread * 4.0f) * dt;
        particle.Velocity.x += spreadDirection.x * preStepBasinContact * 6.0f * dt;
        particle.Velocity.z += spreadDirection.y * preStepBasinContact * 6.0f * dt;
        particle.Velocity.y = particle.Velocity.y +
            (-voxelSize * 1.25f - particle.Velocity.y) * preStepBasinContact * 0.22f;
        particle.CurrentContinuousPosition += particle.Velocity * dt;
        if (preStepBasinContact > 0.0f)
        {
            particle.CurrentContinuousPosition.x = std::clamp(
                particle.CurrentContinuousPosition.x,
                -basinBounds.x,
                basinBounds.x);
            particle.CurrentContinuousPosition.z = std::clamp(
                particle.CurrentContinuousPosition.z,
                -basinBounds.y,
                basinBounds.y);
            if (particle.CurrentContinuousPosition.y < poolSurfaceY)
            {
                particle.CurrentContinuousPosition.y = poolSurfaceY;
                particle.Velocity.y = std::max(particle.Velocity.y, 0.0f) * 0.15f;
            }
            const float damping = 1.0f + (0.92f - 1.0f) * preStepBasinContact;
            particle.Velocity.x *= damping;
            particle.Velocity.z *= damping;
        }

        const float postStepBasinContact = Saturate((parameters.FloorHeight + voxelSize * 2.5f -
            particle.CurrentContinuousPosition.y) / std::max(voxelSize * 2.5f, 0.001f));
        const bool inPool = postStepBasinContact > 0.0f &&
            std::abs(particle.CurrentContinuousPosition.x) <= basinBounds.x + voxelSize &&
            std::abs(particle.CurrentContinuousPosition.z) <= basinBounds.y + voxelSize;
        particle.AgeSeconds += dt;
        particle.BasinAgeSeconds = inPool ? particle.BasinAgeSeconds + dt : 0.0f;

        if (particle.CurrentContinuousPosition.y <= parameters.FloorHeight - voxelSize * 8.0f ||
            particle.BasinAgeSeconds >= basinResidenceSeconds)
        {
            const auto recyclePosition = DeterministicRecyclePosition(particle.GlobalVoxelId, parameters);
            particle.PreviousContinuousPosition = recyclePosition;
            particle.CurrentContinuousPosition = recyclePosition;
            particle.Velocity = DeterministicInitialVelocity(particle.GlobalVoxelId, parameters);
            particle.AgeSeconds = 0.0f;
            particle.BasinAgeSeconds = 0.0f;
            ++recycleCount;
        }
    }

    SimulationSummary SimulateWaterfallLifecycle(
        const VoxelSimulationParameters& parameters,
        const uint32_t particleCount,
        const uint32_t stepCount)
    {
        std::vector<VoxelParticleData> particles;
        particles.reserve(particleCount);
        for (uint32_t i = 0; i < particleCount; ++i)
            particles.push_back(VoxelParticleSpawner::Generate(i, parameters));

        constexpr float dt = 1.0f / 60.0f;
        SimulationSummary summary{};
        for (uint32_t step = 0; step < stepCount; ++step)
        {
            const float time = static_cast<float>(step) * dt;
            uint32_t curtainCount = 0;
            for (auto& particle : particles)
            {
                SimulateParticleStep(particle, parameters, dt, time, summary.RecycleCount);
                if (particle.CurrentContinuousPosition.y > parameters.FloorHeight + parameters.VoxelSize * 8.0f &&
                    particle.CurrentContinuousPosition.y < parameters.SpawnHeight + parameters.VoxelSize * 4.0f)
                {
                    ++curtainCount;
                }
            }
            if (step + 600u >= stepCount && curtainCount > particleCount / 20u)
                ++summary.CurtainOccupiedSamples;
        }

        for (const auto& particle : particles)
        {
            summary.StateHash = Fnv1aAppend32(summary.StateHash, particle.GlobalVoxelId);
            summary.StateHash = Fnv1aAppendSigned32(
                summary.StateHash,
                static_cast<int32_t>(std::lround(particle.CurrentContinuousPosition.x * 1000.0f)));
            summary.StateHash = Fnv1aAppendSigned32(
                summary.StateHash,
                static_cast<int32_t>(std::lround(particle.CurrentContinuousPosition.y * 1000.0f)));
            summary.StateHash = Fnv1aAppendSigned32(
                summary.StateHash,
                static_cast<int32_t>(std::lround(particle.CurrentContinuousPosition.z * 1000.0f)));
            summary.StateHash = Fnv1aAppendSigned32(
                summary.StateHash,
                static_cast<int32_t>(std::lround(particle.BasinAgeSeconds * 1000.0f)));
        }
        return summary;
    }

    void LogTestResult(const char* name, const uint32_t expected, const uint32_t actual)
    {
        std::ostringstream stream;
        stream << "[VoxelWaterfallDynamicReferenceTests] " << name
            << " expected=" << expected << " actual=" << actual << "\n";
        OutputDebugStringA(stream.str().c_str());
    }

    void LogLifecycleResult(const SimulationSummary& summary)
    {
        std::ostringstream stream;
        stream << "[VoxelWaterfallDynamicReferenceTests] lifecycle recycleCount="
            << summary.RecycleCount
            << " curtainOccupiedSamples=" << summary.CurtainOccupiedSamples
            << " stateHash=0x" << std::hex << summary.StateHash << std::dec << "\n";
        OutputDebugStringA(stream.str().c_str());
    }
}

void RunVoxelWaterfallDynamicReferenceTests()
{
    const auto parameters = SmallWaterfallParameters();
    const auto firstCell = VoxelParticleSpawner::ComputeSpawnGridCell(0, parameters);
    assert(firstCell.Width == 34u);
    assert(firstCell.Depth == 11u);

    constexpr uint32_t particleCount = 25000u;
    std::vector<uint32_t> xBins(firstCell.Width, 0);
    std::vector<uint32_t> zBins(firstCell.Depth, 0);
    std::vector<uint8_t> xzOccupied(static_cast<size_t>(firstCell.Width) * firstCell.Depth, 0);
    for (uint32_t i = 0; i < particleCount; ++i)
    {
        const auto cell = VoxelParticleSpawner::ComputeSpawnGridCell(i, parameters);
        assert(cell.X < firstCell.Width);
        assert(cell.Z < firstCell.Depth);
        ++xBins[cell.X];
        ++zBins[cell.Z];
        xzOccupied[static_cast<size_t>(cell.X) + static_cast<size_t>(firstCell.Width) * cell.Z] = 1;
    }

    const auto occupiedX = static_cast<uint32_t>(std::count_if(xBins.begin(), xBins.end(), [](const uint32_t count) { return count > 0; }));
    const auto occupiedZ = static_cast<uint32_t>(std::count_if(zBins.begin(), zBins.end(), [](const uint32_t count) { return count > 0; }));
    const auto occupiedXZ = static_cast<uint32_t>(std::count(xzOccupied.begin(), xzOccupied.end(), static_cast<uint8_t>(1)));
    LogTestResult("small_x_bins", firstCell.Width, occupiedX);
    LogTestResult("small_z_bins", firstCell.Depth, occupiedZ);
    LogTestResult("small_xz_cells", firstCell.Width * firstCell.Depth, occupiedXZ);
    assert(occupiedX == firstCell.Width);
    assert(occupiedZ == firstCell.Depth);
    assert(occupiedXZ == firstCell.Width * firstCell.Depth);

    const auto [minX, maxX] = std::minmax_element(xBins.begin(), xBins.end());
    const auto [minZ, maxZ] = std::minmax_element(zBins.begin(), zBins.end());
    assert(*minX == 735u);
    assert(*maxX == 736u);
    assert(*minZ == 2270u);
    assert(*maxZ == 2275u);
    assert(100u * occupiedX >= 80u * firstCell.Width);

    constexpr uint64_t expectedFirstSamplesHash = 0xa66920af117821afull;
    const uint64_t firstHash = HashFirstSpawnSamples(parameters, 128u);
    const uint64_t secondHash = HashFirstSpawnSamples(parameters, 128u);
    assert(firstHash == expectedFirstSamplesHash);
    assert(secondHash == expectedFirstSamplesHash);

    for (uint32_t poolLayer = 0; poolLayer < 5u; ++poolLayer)
    {
        const float poolSurfaceY = parameters.FloorHeight +
            parameters.VoxelSize * (0.35f + 0.28f * static_cast<float>(poolLayer));
        const float maximumBasinContactAfterClamp = Saturate(
            (parameters.FloorHeight + parameters.VoxelSize * 2.5f - poolSurfaceY) /
            std::max(parameters.VoxelSize * 2.5f, 0.001f));
        assert(maximumBasinContactAfterClamp < 0.95f);
    }

    constexpr uint32_t lifecycleParticleCount = 4096u;
    constexpr uint32_t lifecycleStepCount = 3600u;
    const auto firstLifecycle = SimulateWaterfallLifecycle(
        parameters,
        lifecycleParticleCount,
        lifecycleStepCount);
    const auto secondLifecycle = SimulateWaterfallLifecycle(
        parameters,
        lifecycleParticleCount,
        lifecycleStepCount);
    LogLifecycleResult(firstLifecycle);
    assert(firstLifecycle.RecycleCount > 0u);
    assert(firstLifecycle.CurtainOccupiedSamples > 0u);
    assert(firstLifecycle.RecycleCount == secondLifecycle.RecycleCount);
    assert(firstLifecycle.StateHash == secondLifecycle.StateHash);
}

#endif
