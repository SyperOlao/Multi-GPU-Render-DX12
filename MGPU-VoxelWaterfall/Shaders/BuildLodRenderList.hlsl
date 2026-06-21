#include "ParticleCommon.hlsl"

struct LodBuildData
{
    float3 CameraPosition;
    float Lod0Distance;

    float3 ObjectPosition;
    float Lod1Distance;

    float Hysteresis;
    float VoxelSize;
    float SpawnHeight;
    float FloorHeight;

    float WaterfallWidth;
    float WaterfallDepth;
    uint Seed;
    uint AliveCount;

    uint SpatialLodMode;
    uint AdapterOwner;
    uint Padding0;
    uint Padding1;
};

ConstantBuffer<LodBuildData> LodData : register(b0);
StructuredBuffer<ParticleData> ParticlesPool : register(t0);
StructuredBuffer<uint> AliveParticles : register(t1);
AppendStructuredBuffer<VoxelLodRenderItem> RenderParticles : register(u0);
RWByteAddressBuffer DrawArguments : register(u1);
RWStructuredBuffer<uint> LodStats : register(u2);
RWStructuredBuffer<uint> PreviousLodLevels : register(u3);

#define THREAD_GROUP_SIZE 256
#define CHUNK_WIDTH_CELLS 8u
#define CHUNK_HEIGHT_CELLS 8u
#define CHUNK_DEPTH_CELLS 4u

uint HashVoxel(uint value)
{
    uint x = value;
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

void ComputeSpawnGridCell(uint globalVoxelId, out uint3 cell, out uint3 gridSize)
{
    const float voxelSize = max(LodData.VoxelSize, 0.05f);
    const uint widthCells = max(1u, (uint)floor(LodData.WaterfallWidth / voxelSize));
    const uint depthCells = max(1u, (uint)floor(LodData.WaterfallDepth / voxelSize));
    const uint heightCells = max(1u, (uint)floor((LodData.SpawnHeight - LodData.FloorHeight) / voxelSize));
    const uint horizontalCells = widthCells * depthCells;
    const uint laneCount = max(1u, min(horizontalCells, max(3u, (horizontalCells * 3u) / 4u)));
    const uint laneIndex = globalVoxelId % laneCount;
    const uint laneHash = HashVoxel(laneIndex ^ LodData.Seed);

    cell.x = laneHash % widthCells;
    cell.z = HashVoxel(laneHash + LodData.Seed * 17u) % depthCells;
    const uint yPhase = HashVoxel(globalVoxelId + LodData.Seed * 31u) % heightCells;
    cell.y = ((globalVoxelId / laneCount) + yPhase) % heightCells;
    gridSize = uint3(widthCells, heightCells, depthCells);
}

uint SelectLodLevel(float distanceToCamera, uint previousLevel)
{
    uint selectedLevel = 0u;
    const float h = max(LodData.Hysteresis, 0.0f);
    previousLevel = min(previousLevel, 2u);

    if (LodData.SpatialLodMode != 0u)
    {
        if (previousLevel == 0u && distanceToCamera <= LodData.Lod0Distance + h)
            selectedLevel = 0u;
        else if (previousLevel == 1u &&
                 distanceToCamera > LodData.Lod0Distance - h &&
                 distanceToCamera <= LodData.Lod1Distance + h)
            selectedLevel = 1u;
        else if (previousLevel == 2u && distanceToCamera > LodData.Lod1Distance - h)
            selectedLevel = 2u;
        else if (distanceToCamera <= LodData.Lod0Distance)
            selectedLevel = 0u;
        else if (distanceToCamera <= LodData.Lod1Distance)
            selectedLevel = 1u;
        else
            selectedLevel = 2u;
    }

    return selectedLevel;
}

bool IsRepresentative(uint3 cell, uint lodLevel)
{
    const uint localX = cell.x % CHUNK_WIDTH_CELLS;
    const uint localY = cell.y % CHUNK_HEIGHT_CELLS;
    const uint localZ = cell.z % CHUNK_DEPTH_CELLS;
    const uint chunkLocalOrdinal = localX + CHUNK_WIDTH_CELLS * (localY + CHUNK_HEIGHT_CELLS * localZ);
    const uint groupSize = lodLevel == 1u ? 8u : 64u;
    bool representative = true;
    if (lodLevel != 0u)
        representative = (chunkLocalOrdinal % groupSize) == 0u;
    return representative;
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint aliveOrdinal = dispatchThreadId.x;
    if (aliveOrdinal >= LodData.AliveCount)
        return;

    const uint particleIndex = AliveParticles[aliveOrdinal];
    const ParticleData particle = ParticlesPool[particleIndex];

    uint3 cell;
    uint3 gridSize;
    ComputeSpawnGridCell(particle.GlobalVoxelId, cell, gridSize);

    const uint3 chunk = uint3(
        cell.x / CHUNK_WIDTH_CELLS,
        cell.y / CHUNK_HEIGHT_CELLS,
        cell.z / CHUNK_DEPTH_CELLS);
    const float voxelSize = max(LodData.VoxelSize, 0.05f);
    const float3 chunkCenterLocal = float3(
        ((float)(chunk.x * CHUNK_WIDTH_CELLS) + 0.5f * (float)min(CHUNK_WIDTH_CELLS, max(1u, gridSize.x - chunk.x * CHUNK_WIDTH_CELLS)) - 0.5f * (float)(gridSize.x - 1u)) * voxelSize,
        LodData.SpawnHeight - ((float)(chunk.y * CHUNK_HEIGHT_CELLS) + 0.5f * (float)min(CHUNK_HEIGHT_CELLS, max(1u, gridSize.y - chunk.y * CHUNK_HEIGHT_CELLS))) * voxelSize,
        ((float)(chunk.z * CHUNK_DEPTH_CELLS) + 0.5f * (float)min(CHUNK_DEPTH_CELLS, max(1u, gridSize.z - chunk.z * CHUNK_DEPTH_CELLS)) - 0.5f * (float)(gridSize.z - 1u)) * voxelSize);
    const float distanceToCamera = length(chunkCenterLocal + LodData.ObjectPosition - LodData.CameraPosition);
    const uint lodLevel = SelectLodLevel(distanceToCamera, PreviousLodLevels[particleIndex]);
    PreviousLodLevels[particleIndex] = lodLevel;

    if (!IsRepresentative(cell, lodLevel))
    {
        InterlockedAdd(LodStats[3], 1u);
        return;
    }

    VoxelLodRenderItem item;
    item.ParticleIndex = particleIndex;
    item.LodLevel = lodLevel;
    item.Padding0 = 0u;
    item.Padding1 = 0u;
    RenderParticles.Append(item);
    DrawArguments.InterlockedAdd(0, 1u);
    InterlockedAdd(LodStats[lodLevel], 1u);
}
