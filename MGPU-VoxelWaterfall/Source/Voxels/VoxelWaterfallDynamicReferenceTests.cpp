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

    void LogTestResult(const char* name, const uint32_t expected, const uint32_t actual)
    {
        std::ostringstream stream;
        stream << "[VoxelWaterfallDynamicReferenceTests] " << name
            << " expected=" << expected << " actual=" << actual << "\n";
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
}

#endif
