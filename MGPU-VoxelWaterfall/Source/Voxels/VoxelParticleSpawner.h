#pragma once

#include "Source/Voxels/VoxelTypes.h"

class VoxelParticleSpawner
{
public:
    static VoxelParticleData Generate(DWORD index, const VoxelSimulationParameters& parameters);
};

