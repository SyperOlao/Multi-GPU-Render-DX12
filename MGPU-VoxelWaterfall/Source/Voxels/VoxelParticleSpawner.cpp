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
}

VoxelParticleData VoxelParticleSpawner::Generate(const DWORD index, const VoxelSimulationParameters& parameters)
{
    const float voxelSize = std::max(parameters.VoxelSize, 0.05f);
    const DWORD widthCells = std::max<DWORD>(1, static_cast<DWORD>(std::floor(parameters.WaterfallWidth / voxelSize)));
    const DWORD depthCells = std::max<DWORD>(1, static_cast<DWORD>(std::floor(parameters.WaterfallDepth / voxelSize)));
    const DWORD horizontalCells = widthCells * depthCells;
    const DWORD verticalCells = std::max<DWORD>(
        1, static_cast<DWORD>(std::floor((parameters.SpawnHeight - parameters.FloorHeight) / voxelSize)));

    const DWORD laneCount = std::max<DWORD>(
        1, std::min<DWORD>(horizontalCells, std::max<DWORD>(3, (horizontalCells * 3) / 4)));
    const DWORD laneIndex = index % laneCount;
    const DWORD laneHash = HashVoxel(laneIndex ^ parameters.Seed);
    const DWORD xIndex = laneHash % widthCells;
    const DWORD zIndex = HashVoxel(laneHash + parameters.Seed * 17u) % depthCells;
    const DWORD yPhase = HashVoxel(index + parameters.Seed * 31u) % verticalCells;
    const DWORD yIndex = ((index / laneCount) + yPhase) % verticalCells;

    const float x = (static_cast<float>(xIndex) - 0.5f * static_cast<float>(widthCells - 1)) * voxelSize;
    const float y = parameters.SpawnHeight - static_cast<float>(yIndex) * voxelSize;
    const float z = (static_cast<float>(zIndex) - 0.5f * static_cast<float>(depthCells - 1)) * voxelSize;
    const float speedVariation = 0.75f + 0.5f * HashUnitFloat(index ^ parameters.Seed ^ 0x9e3779b9u);

    VoxelParticleData particle{};
    particle.Position = DirectX::SimpleMath::Vector3(x, y, z);
    particle.ContinuousPosition = particle.Position;
    particle.Velocity = DirectX::SimpleMath::Vector3(0.0f, -parameters.InitialFallSpeed * speedVariation, 0.0f);
    particle.VoxelIndex = index;
    return particle;
}

