#pragma once

#include "Source/Voxels/VoxelTypes.h"

class VoxelSceneWorkloadBuilder
{
public:
    static VoxelSceneWorkload Build(const VoxelSceneWorkload& source);
    static bool Validate(const VoxelSceneWorkload& workload);
    static VoxelGlobalId EncodeSequenceGlobalVoxelId(VoxelLayerId layerId, DWORD sequenceId);
    static VoxelChunkId EncodeChunkId(VoxelLayerId layerId, uint32_t chunkX, uint32_t chunkY, uint32_t chunkZ);
    static VoxelAdapterPartitionId ChooseChunkOwner(
        VoxelPartitionStrategy strategy,
        VoxelLoadBalanceScenario scenario,
        VoxelChunkId chunkId,
        uint32_t chunkLinearIndex,
        uint32_t chunkX,
        uint32_t chunkY,
        uint32_t chunkZ,
        uint32_t chunkGridX,
        uint32_t chunkGridY,
        uint32_t chunkGridZ,
        float secondaryShare);

private:
    static void InitializeAdapterPartitions(VoxelSceneWorkload& workload);
    static VoxelSceneLayer BuildDynamicWaterfallLayer(const VoxelSceneWorkload& workload);
    static void AppendLayerToAdapterPartitions(VoxelSceneWorkload& workload, const VoxelSceneLayer& layer);
};
