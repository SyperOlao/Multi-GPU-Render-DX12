#include "ParticleCommon.hlsl"

ConstantBuffer<EmitterData> EmitterBuffer : register(b0);
RWStructuredBuffer<ParticleData> ParticlesPool : register(u0);

#ifdef INJECTION
ConsumeStructuredBuffer<uint> DeadParticles : register(u1);
AppendStructuredBuffer<uint> AliveParticles : register(u2);
RWStructuredBuffer<ParticleData> InjectionParticles : register(u3);
#endif

#ifdef SIMULATION
AppendStructuredBuffer<uint> DeadParticles : register(u1);
RWStructuredBuffer<uint> AliveParticles : register(u2);
#endif

#define THREAD_GROUP_X 32
#define THREAD_GROUP_Y 32
#define THREAD_GROUP_TOTAL 1024

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

float HashUnitFloat(uint value)
{
    return (float)(HashVoxel(value) & 0x00ffffffu) / (float)0x01000000u;
}

float3 DeterministicSpawnPosition(uint voxelIndex)
{
    const float voxelSize = max(EmitterBuffer.VoxelSize, 0.05f);
    const uint widthCells = max(1u, (uint)floor(EmitterBuffer.WaterfallWidth / voxelSize));
    const uint depthCells = max(1u, (uint)floor(EmitterBuffer.WaterfallDepth / voxelSize));
    const uint horizontalCells = widthCells * depthCells;
    const uint laneCount = max(1u, min(horizontalCells, max(3u, (horizontalCells * 3u) / 4u)));
    const uint laneIndex = voxelIndex % laneCount;
    const uint laneHash = HashVoxel(laneIndex ^ EmitterBuffer.Seed);
    const uint xIndex = laneHash % widthCells;
    const uint zIndex = HashVoxel(laneHash + EmitterBuffer.Seed * 17u) % depthCells;
    const uint topLayer = HashVoxel(voxelIndex + EmitterBuffer.Seed * 13u) % 3u;

    const float x = ((float)xIndex - 0.5f * (float)(widthCells - 1u)) * voxelSize;
    const float y = EmitterBuffer.SpawnHeight + (float)topLayer * voxelSize;
    const float z = ((float)zIndex - 0.5f * (float)(depthCells - 1u)) * voxelSize;
    return float3(x, y, z);
}

float3 DeterministicInitialVelocity(uint voxelIndex)
{
    const float speedVariation = 0.75f + 0.5f * HashUnitFloat(voxelIndex ^ EmitterBuffer.Seed ^ 0x9e3779b9u);
    return float3(0.0f, -EmitterBuffer.InitialFallSpeed * speedVariation, 0.0f);
}

[numthreads(THREAD_GROUP_X, THREAD_GROUP_Y, 1)]
void CS(uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    const uint groupWidth =
#ifdef INJECTION
        EmitterBuffer.InjectGroupCount;
#else
        EmitterBuffer.SimulatedGroupCount;
#endif

    const uint threadParticleIndex = groupID.x * THREAD_GROUP_TOTAL
        + groupID.y * groupWidth * THREAD_GROUP_TOTAL + groupIndex;

#ifdef INJECTION
    if (threadParticleIndex >= EmitterBuffer.ParticleInjectCount)
        return;

    const uint particleIndex = DeadParticles.Consume();
    ParticlesPool[particleIndex] = InjectionParticles.Load(threadParticleIndex);
    AliveParticles.Append(particleIndex);
#endif

#ifdef SIMULATION
    if (threadParticleIndex >= EmitterBuffer.ParticleAliveCount)
        return;

    const uint aliveIndex = AliveParticles.Load(threadParticleIndex);
    ParticleData particle = ParticlesPool.Load(aliveIndex);

    particle.Velocity += EmitterBuffer.Force * EmitterBuffer.DeltaTime;
    particle.ContinuousPosition += particle.Velocity * EmitterBuffer.DeltaTime;

    if (particle.ContinuousPosition.y <= EmitterBuffer.FloorHeight)
    {
        particle.ContinuousPosition = DeterministicSpawnPosition(particle.VoxelIndex);
        particle.Velocity = DeterministicInitialVelocity(particle.VoxelIndex);
    }

    const float voxelSize = max(EmitterBuffer.VoxelSize, 0.05f);
    particle.Position = round(particle.ContinuousPosition / voxelSize) * voxelSize;
    ParticlesPool[aliveIndex] = particle;
#endif
}
