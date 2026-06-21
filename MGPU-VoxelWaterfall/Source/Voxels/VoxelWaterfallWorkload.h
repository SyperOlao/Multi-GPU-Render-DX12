#pragma once

#include "Source/Voxels/VoxelTypes.h"

class VoxelWaterfallWorkloadBuilder
{
public:
    static VoxelWaterfallWorkload Build(const VoxelWaterfallWorkload& source);
    static bool Validate(const VoxelWaterfallWorkload& workload);

private:
    static VoxelPartitionId ChooseChunkOwner(
        DWORD globalVoxelId,
        const VoxelSimulationParameters& parameters,
        float secondaryShare);
};
