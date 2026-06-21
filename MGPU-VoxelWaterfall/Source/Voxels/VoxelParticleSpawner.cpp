#include "Source/Voxels/VoxelParticleSpawner.h"

#include <algorithm>
#include <cmath>

namespace
{
    DWORD HashVoxel(const DWORD value)
    {
        DWORD x = value;
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    float HashUnitFloat(const DWORD value)
    {
        return static_cast<float>(HashVoxel(value) & 0x00ffffffu) / static_cast<float>(0x01000000u);
    }

    constexpr float TwoPi = 6.28318530717958647692f;

    DWORD StableSpawnXOffset(const DWORD baseZ, const DWORD cycleIndex, const DWORD seed)
    {
        return HashVoxel(baseZ * 73856093u ^ cycleIndex * 19349663u ^ seed);
    }

    DWORD StableSpawnZOffset(const DWORD baseX, const DWORD cycleIndex, const DWORD seed)
    {
        return HashVoxel(baseX * 83492791u ^ cycleIndex * 2654435761u ^ seed ^ 0x68bc21ebu);
    }
}

VoxelParticleSpawner::SpawnGridCell VoxelParticleSpawner::ComputeSpawnGridCell(
    const DWORD globalVoxelId,
    const VoxelSimulationParameters& parameters)
{
    const float voxelSize = std::max(parameters.VoxelSize, 0.05f);
    const DWORD widthCells = std::max<DWORD>(1, static_cast<DWORD>(std::floor(parameters.WaterfallWidth / voxelSize)));
    const DWORD depthCells = std::max<DWORD>(1, static_cast<DWORD>(std::floor(parameters.WaterfallDepth / voxelSize)));
    const DWORD heightCells = std::max<DWORD>(
        1, static_cast<DWORD>(std::floor((parameters.SpawnHeight - parameters.FloorHeight) / voxelSize)));
    const DWORD horizontalCells = std::max<DWORD>(1, widthCells * depthCells);
    const DWORD laneIndex = globalVoxelId % horizontalCells;
    const DWORD cycleIndex = globalVoxelId / horizontalCells;
    const DWORD baseX = laneIndex % widthCells;
    const DWORD baseZ = laneIndex / widthCells;
    const DWORD xIndex = (baseX + StableSpawnXOffset(baseZ, cycleIndex, parameters.Seed) % widthCells) % widthCells;
    const DWORD zIndex = (baseZ + StableSpawnZOffset(baseX, cycleIndex, parameters.Seed) % depthCells) % depthCells;
    const DWORD yPhase = HashVoxel(globalVoxelId + parameters.Seed * 31u) % heightCells;
    const DWORD yIndex = (cycleIndex + yPhase) % heightCells;

    return {xIndex, yIndex, zIndex, widthCells, heightCells, depthCells};
}

VoxelParticleData VoxelParticleSpawner::Generate(const DWORD globalVoxelId, const VoxelSimulationParameters& parameters)
{
    const float voxelSize = std::max(parameters.VoxelSize, 0.05f);
    const auto cell = ComputeSpawnGridCell(globalVoxelId, parameters);

    const float x = (static_cast<float>(cell.X) - 0.5f * static_cast<float>(cell.Width - 1)) * voxelSize;
    const float y = parameters.SpawnHeight - static_cast<float>(cell.Y) * voxelSize;
    const float z = (static_cast<float>(cell.Z) - 0.5f * static_cast<float>(cell.Depth - 1)) * voxelSize;
    const float speedVariation = 0.75f + 0.5f * HashUnitFloat(globalVoxelId ^ parameters.Seed ^ 0x9e3779b9u);
    const float lateralX = (HashUnitFloat(globalVoxelId ^ parameters.Seed ^ 0x85ebca6bu) - 0.5f) * 0.8f;
    const float lateralZ = (HashUnitFloat(globalVoxelId ^ parameters.Seed ^ 0xc2b2ae35u) - 0.5f) * 0.45f;

    VoxelParticleData particle{};
    particle.PreviousContinuousPosition = DirectX::SimpleMath::Vector3(x, y, z);
    particle.CurrentContinuousPosition = particle.PreviousContinuousPosition;
    particle.Velocity = DirectX::SimpleMath::Vector3(lateralX, -parameters.InitialFallSpeed * speedVariation, lateralZ);
    particle.FlowPhase = HashUnitFloat(globalVoxelId ^ parameters.Seed ^ 0x27d4eb2fu) * TwoPi;
    particle.GlobalVoxelId = globalVoxelId;
    return particle;
}
