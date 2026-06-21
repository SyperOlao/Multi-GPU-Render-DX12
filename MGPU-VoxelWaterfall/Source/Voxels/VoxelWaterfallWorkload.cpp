#include "Source/Voxels/VoxelWaterfallWorkload.h"

#include "Source/Voxels/VoxelParticleSpawner.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_set>

namespace
{
    constexpr DWORD ChunkWidthCells = 8;
    constexpr DWORD ChunkHeightCells = 8;
    constexpr DWORD ChunkDepthCells = 4;

    DWORD HashChunk(const DWORD value)
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
        return static_cast<float>(HashChunk(value) & 0x00ffffffu) / static_cast<float>(0x01000000u);
    }
}

VoxelPartitionId VoxelWaterfallWorkloadBuilder::ChooseChunkOwner(
    const DWORD globalVoxelId,
    const VoxelSimulationParameters& parameters,
    const float secondaryShare)
{
    const auto cell = VoxelParticleSpawner::ComputeSpawnGridCell(globalVoxelId, parameters);
    const DWORD chunkX = cell.X / ChunkWidthCells;
    const DWORD chunkY = cell.Y / ChunkHeightCells;
    const DWORD chunkZ = cell.Z / ChunkDepthCells;
    const DWORD chunkGridX = (cell.Width + ChunkWidthCells - 1) / ChunkWidthCells;
    const DWORD chunkGridY = (cell.Height + ChunkHeightCells - 1) / ChunkHeightCells;
    const DWORD chunkId = chunkX + chunkGridX * (chunkY + chunkGridY * chunkZ);

    const float clampedShare = std::clamp(secondaryShare, 0.0f, 1.0f);
    const float ownerSample = HashUnitFloat(chunkId ^ parameters.Seed);
    return ownerSample < clampedShare
               ? VoxelPartitionId::SecondaryPartition
               : VoxelPartitionId::PrimaryPartition;
}

VoxelWaterfallWorkload VoxelWaterfallWorkloadBuilder::Build(const VoxelWaterfallWorkload& source)
{
    VoxelWaterfallWorkload workload = source;
    workload.TotalVoxelCount = std::max<uint32_t>(1, workload.TotalVoxelCount);
    workload.SecondaryShare = std::clamp(workload.SecondaryShare, 0.0f, 1.0f);
    workload.TemporalDecimationInterval = std::max<uint32_t>(1, workload.TemporalDecimationInterval);

    auto& primary = workload.Partitions[static_cast<size_t>(VoxelPartitionId::PrimaryPartition)];
    primary = {};
    primary.DisplayName = "PrimaryPartition";
    primary.ObjectName = "VoxelWaterfall.PrimaryPartition";
    primary.PartitionId = VoxelPartitionId::PrimaryPartition;
    primary.AdapterOwner = VoxelAdapterOwner::Primary;
    primary.UpdateInterval = 1;
    primary.EffectiveUpdateInterval = 1;
    primary.CoarseDeltaTime = 1.0f / 60.0f;

    auto& secondary = workload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)];
    secondary = {};
    secondary.DisplayName = "SecondaryPartition";
    secondary.ObjectName = "VoxelWaterfall.SecondaryPartition";
    secondary.PartitionId = VoxelPartitionId::SecondaryPartition;
    secondary.AdapterOwner = VoxelAdapterOwner::Secondary;
    secondary.UpdateInterval = workload.TemporalDecimationInterval;
    secondary.EffectiveUpdateInterval = workload.TemporalDecimationInterval;
    secondary.CoarseDeltaTime =
        static_cast<float>(static_cast<double>(secondary.EffectiveUpdateInterval) / 60.0);

    for (DWORD globalVoxelId = 0; globalVoxelId < workload.TotalVoxelCount; ++globalVoxelId)
    {
        const auto owner = ChooseChunkOwner(globalVoxelId, workload.Parameters, workload.SecondaryShare);
        workload.Partitions[static_cast<size_t>(owner)].GlobalVoxelIds.push_back(globalVoxelId);
    }

    if (!Validate(workload))
        throw std::runtime_error("Invalid voxel workload partitioning");

    return workload;
}

bool VoxelWaterfallWorkloadBuilder::Validate(const VoxelWaterfallWorkload& workload)
{
    size_t totalPartitionVoxels = 0;
    std::unordered_set<DWORD> globalIds;
    globalIds.reserve(workload.TotalVoxelCount);

    for (const auto& partition : workload.Partitions)
    {
        totalPartitionVoxels += partition.GlobalVoxelIds.size();
        for (const DWORD globalId : partition.GlobalVoxelIds)
        {
            if (globalId >= workload.TotalVoxelCount)
            {
                assert(false && "Global voxel id is outside the workload range");
                return false;
            }

            if (!globalIds.insert(globalId).second)
            {
                assert(false && "Duplicate global voxel id found across partitions");
                return false;
            }
        }
    }

    if (totalPartitionVoxels != workload.TotalVoxelCount || globalIds.size() != workload.TotalVoxelCount)
    {
        assert(false && "Partition voxel counts do not match TotalVoxelCount");
        return false;
    }

    return true;
}
