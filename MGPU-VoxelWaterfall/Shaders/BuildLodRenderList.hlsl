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
    uint StreamKind;
    uint GroupTableCapacity;

    int GridOriginX;
    int GridOriginY;
    int GridOriginZ;
    uint MaxProbeCount;
};

ConstantBuffer<LodBuildData> LodData : register(b0);
StructuredBuffer<ParticleData> ParticlesPool : register(t0);
StructuredBuffer<uint> AliveParticles : register(t1);
RWStructuredBuffer<VoxelLodRenderItem> RenderParticles : register(u0);
RWByteAddressBuffer DrawArguments : register(u1);
RWStructuredBuffer<uint> LodStats : register(u2);
RWStructuredBuffer<uint> LodGroupKeys : register(u3);

#define THREAD_GROUP_SIZE 256
#define EMPTY_GROUP_KEY 0xffffffffu
#define STAT_LOD0 0u
#define STAT_LOD1 1u
#define STAT_LOD2 2u
#define STAT_DUPLICATES 3u
#define STAT_OVERFLOW 4u
#define STAT_MAX_PROBE 5u
#define STAT_TOTAL_PROBE 6u
#define STAT_EMITTED 7u

uint HashGroupKey(uint key)
{
    uint x = key;
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

uint3 DecodePackedGridCoordinate(uint packed)
{
    return uint3(packed & 0x3ffu, (packed >> 10u) & 0x3ffu, (packed >> 20u) & 0x3ffu);
}

int FloorDiv(const int value, const int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

int3 FloorDiv3(const int3 value, const int divisor)
{
    return int3(FloorDiv(value.x, divisor), FloorDiv(value.y, divisor), FloorDiv(value.z, divisor));
}

uint EncodeSigned10(const int value)
{
    return (uint)(value & 1023);
}

uint PackGroupKey(const int3 groupCoord, const uint lodLevel)
{
    return (min(lodLevel, 2u) << 30u) |
        (EncodeSigned10(groupCoord.z) << 20u) |
        (EncodeSigned10(groupCoord.y) << 10u) |
        EncodeSigned10(groupCoord.x);
}

uint SelectLodLevel(float distanceToCamera)
{
    if (LodData.SpatialLodMode == 0u)
        return 0u;

    if (distanceToCamera <= LodData.Lod0Distance)
        return 0u;
    if (distanceToCamera <= LodData.Lod1Distance)
        return 1u;
    return 2u;
}

uint BlockSizeForLod(const uint lodLevel)
{
    return lodLevel == 0u ? 1u : (lodLevel == 1u ? 2u : 4u);
}

float3 CellCenter(const int3 signedCellCoord, const float voxelSize)
{
    return (float3(signedCellCoord) + 0.5f) * voxelSize;
}

float3 GroupCenter(const int3 groupCoord, const uint blockSize, const float voxelSize)
{
    return (float3(groupCoord * (int)blockSize) + 0.5f * (float)blockSize) * voxelSize;
}

bool ClaimGroup(const uint groupKey, out uint probeCount, out bool duplicate)
{
    probeCount = 0u;
    duplicate = false;
    const uint capacity = max(1u, LodData.GroupTableCapacity);
    const uint mask = capacity - 1u;
    uint slot = HashGroupKey(groupKey) & mask;
    const uint maxProbeCount = clamp(LodData.MaxProbeCount, 1u, capacity);

    [loop]
    for (uint probe = 0u; probe < maxProbeCount; ++probe)
    {
        probeCount = probe + 1u;
        uint original;
        InterlockedCompareExchange(LodGroupKeys[slot], EMPTY_GROUP_KEY, groupKey, original);
        if (original == EMPTY_GROUP_KEY)
            return true;
        if (original == groupKey)
        {
            duplicate = true;
            return false;
        }
        slot = (slot + 1u) & mask;
    }

    return false;
}

VoxelLodRenderItem MakeRenderItem(
    const ParticleData particle,
    const uint particleIndex,
    const uint lodLevel,
    const float3 previousCenter,
    const float3 currentCenter,
    const float3 halfExtent)
{
    VoxelLodRenderItem item;
    item.PreviousCenter = previousCenter;
    item.HalfExtentX = halfExtent.x;
    item.CurrentCenter = currentCenter;
    item.HalfExtentY = halfExtent.y;
    item.HalfExtentZ = halfExtent.z;
    item.LodLevel = lodLevel;
    item.MaterialId = particle.MaterialId;
    item.StreamKind = particle.StreamKind;
    item.RepresentativeIndex = particleIndex;
    item.Padding0 = 0u;
    item.Padding1 = 0u;
    item.Padding2 = 0u;
    return item;
}

void BuildStaticSpatialData(
    const ParticleData particle,
    const uint lodLevel,
    out uint groupKey,
    out float3 previousCenter,
    out float3 currentCenter,
    out float3 halfExtent)
{
    const float voxelSize = max(LodData.VoxelSize, 0.05f);
    const uint3 localCell = DecodePackedGridCoordinate(particle.PackedGridCoordinate);
    const int3 signedCell = int3(localCell) + int3(LodData.GridOriginX, LodData.GridOriginY, LodData.GridOriginZ);
    const uint blockSize = BlockSizeForLod(lodLevel);
    const int3 groupCoord = FloorDiv3(signedCell, (int)blockSize);

    groupKey = PackGroupKey(groupCoord, lodLevel);
    previousCenter = GroupCenter(groupCoord, blockSize, voxelSize);
    currentCenter = previousCenter;
    halfExtent = 0.5f * (float)blockSize * voxelSize;
}

void BuildDynamicSpatialData(
    const ParticleData particle,
    const uint lodLevel,
    out uint groupKey,
    out float3 previousCenter,
    out float3 currentCenter,
    out float3 halfExtent)
{
    const float voxelSize = max(LodData.VoxelSize, 0.05f);
    const uint blockSize = BlockSizeForLod(lodLevel);
    const int3 currentCell = int3(
        (int)floor(particle.CurrentContinuousPosition.x / voxelSize),
        (int)floor(particle.CurrentContinuousPosition.y / voxelSize),
        (int)floor(particle.CurrentContinuousPosition.z / voxelSize));
    const int3 previousCell = int3(
        (int)floor(particle.PreviousContinuousPosition.x / voxelSize),
        (int)floor(particle.PreviousContinuousPosition.y / voxelSize),
        (int)floor(particle.PreviousContinuousPosition.z / voxelSize));
    const int3 currentGroup = FloorDiv3(currentCell, (int)blockSize);
    const int3 previousGroup = FloorDiv3(previousCell, (int)blockSize);

    groupKey = PackGroupKey(currentGroup, lodLevel);
    previousCenter = GroupCenter(previousGroup, blockSize, voxelSize);
    currentCenter = GroupCenter(currentGroup, blockSize, voxelSize);
    halfExtent = 0.56f * (float)blockSize * voxelSize;
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint aliveOrdinal = dispatchThreadId.x;
    if (aliveOrdinal >= LodData.AliveCount)
        return;

    const uint particleIndex = AliveParticles[aliveOrdinal];
    const ParticleData particle = ParticlesPool[particleIndex];
    const bool staticStream = LodData.StreamKind == 1u || particle.StreamKind == 1u;

    if (LodData.SpatialLodMode == 0u)
    {
        const float voxelSize = max(LodData.VoxelSize, 0.05f);
        const float halfExtent = staticStream ? 0.5f * voxelSize : 0.56f * voxelSize;
        RenderParticles[aliveOrdinal] = MakeRenderItem(
            particle,
            particleIndex,
            0u,
            particle.PreviousContinuousPosition,
            particle.CurrentContinuousPosition,
            float3(halfExtent, halfExtent, halfExtent));
        return;
    }

    float3 distanceCenter;
    if (staticStream)
    {
        const float voxelSize = max(LodData.VoxelSize, 0.05f);
        const uint3 localCell = DecodePackedGridCoordinate(particle.PackedGridCoordinate);
        const int3 signedCell = int3(localCell) + int3(LodData.GridOriginX, LodData.GridOriginY, LodData.GridOriginZ);
        distanceCenter = CellCenter(signedCell, voxelSize);
    }
    else
    {
        distanceCenter = particle.CurrentContinuousPosition;
    }

    const float distanceToCamera = length(distanceCenter + LodData.ObjectPosition - LodData.CameraPosition);
    const uint lodLevel = SelectLodLevel(distanceToCamera);

    uint groupKey;
    float3 previousCenter;
    float3 currentCenter;
    float3 halfExtent;
    if (staticStream)
    {
        BuildStaticSpatialData(particle, lodLevel, groupKey, previousCenter, currentCenter, halfExtent);
    }
    else
    {
        BuildDynamicSpatialData(particle, lodLevel, groupKey, previousCenter, currentCenter, halfExtent);
    }

    uint probeCount = 0u;
    bool duplicate = false;
    if (!ClaimGroup(groupKey, probeCount, duplicate))
    {
        if (duplicate)
            InterlockedAdd(LodStats[STAT_DUPLICATES], 1u);
        else
            InterlockedAdd(LodStats[STAT_OVERFLOW], 1u);
        InterlockedAdd(LodStats[STAT_TOTAL_PROBE], probeCount);
        uint previousMax;
        InterlockedMax(LodStats[STAT_MAX_PROBE], probeCount, previousMax);
        return;
    }

    uint outputIndex;
    DrawArguments.InterlockedAdd(0, 1u, outputIndex);
    RenderParticles[outputIndex] = MakeRenderItem(
        particle,
        particleIndex,
        lodLevel,
        previousCenter,
        currentCenter,
        halfExtent);
    InterlockedAdd(LodStats[lodLevel], 1u);
    InterlockedAdd(LodStats[STAT_EMITTED], 1u);
    InterlockedAdd(LodStats[STAT_TOTAL_PROBE], probeCount);
    uint previousMax;
    InterlockedMax(LodStats[STAT_MAX_PROBE], probeCount, previousMax);
}
