#pragma once

#include "Source/Voxels/VoxelTypes.h"

class VoxelParticleSpawner
{
public:
    struct SpawnGridCell
    {
        DWORD X = 0;
        DWORD Y = 0;
        DWORD Z = 0;
        DWORD Width = 1;
        DWORD Height = 1;
        DWORD Depth = 1;
    };

    static SpawnGridCell ComputeSpawnGridCell(DWORD globalVoxelId, const VoxelSimulationParameters& parameters);
    static VoxelParticleData Generate(DWORD globalVoxelId, const VoxelSimulationParameters& parameters);
};
