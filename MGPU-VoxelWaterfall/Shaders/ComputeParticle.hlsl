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
RWStructuredBuffer<uint> SimulationStats : register(u4);
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
    const uint horizontalCells = max(1u, widthCells * depthCells);
    const uint laneIndex = voxelIndex % horizontalCells;
    const uint cycleIndex = voxelIndex / horizontalCells;
    const uint baseX = laneIndex % widthCells;
    const uint baseZ = laneIndex / widthCells;
    const uint xIndex = (baseX + HashVoxel(baseZ * 73856093u ^ cycleIndex * 19349663u ^ EmitterBuffer.Seed) % widthCells) % widthCells;
    const uint zIndex = (baseZ + HashVoxel(baseX * 83492791u ^ cycleIndex * 2654435761u ^ EmitterBuffer.Seed ^ 0x68bc21ebu) % depthCells) % depthCells;
    const uint topLayer = HashVoxel(voxelIndex + EmitterBuffer.Seed * 13u) % 3u;

    const float x = ((float)xIndex - 0.5f * (float)(widthCells - 1u)) * voxelSize;
    const float y = EmitterBuffer.SpawnHeight + (float)topLayer * voxelSize;
    const float z = ((float)zIndex - 0.5f * (float)(depthCells - 1u)) * voxelSize;
    return float3(x, y, z);
}

float3 DeterministicRecyclePosition(uint voxelIndex)
{
    const float voxelSize = max(EmitterBuffer.VoxelSize, 0.05f);
    const float3 basePosition = DeterministicSpawnPosition(voxelIndex);
    const uint topLayer = HashVoxel(voxelIndex + EmitterBuffer.Seed * 13u) % 3u;
    return float3(basePosition.x, EmitterBuffer.SpawnHeight + (float)topLayer * voxelSize, basePosition.z);
}

float3 DeterministicInitialVelocity(uint voxelIndex)
{
    const float speedVariation = 0.75f + 0.5f * HashUnitFloat(voxelIndex ^ EmitterBuffer.Seed ^ 0x9e3779b9u);
    const float lateralX = (HashUnitFloat(voxelIndex ^ EmitterBuffer.Seed ^ 0x85ebca6bu) - 0.5f) * 0.8f;
    const float lateralZ = (HashUnitFloat(voxelIndex ^ EmitterBuffer.Seed ^ 0xc2b2ae35u) - 0.5f) * 0.45f;
    return float3(lateralX, -EmitterBuffer.InitialFallSpeed * speedVariation, lateralZ);
}

float2 SafeNormalize2(float2 value)
{
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-5f)
        return float2(1.0f, 0.0f);
    return value * rsqrt(lengthSquared);
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
    particle.PreviousContinuousPosition = particle.CurrentContinuousPosition;

    const float voxelSize = max(EmitterBuffer.VoxelSize, 0.05f);
    const float dt = EmitterBuffer.DeltaTime;
    const float time = EmitterBuffer.SimulationTime;
    const float phase = particle.FlowPhase;
    const float flowX = sin(phase + time * 0.91f + particle.CurrentContinuousPosition.y * 0.037f) * 0.85f;
    const float flowZ = cos(phase * 1.37f + time * 0.67f + particle.CurrentContinuousPosition.x * 0.041f) * 0.45f;
    const float floorSpread = saturate((EmitterBuffer.FloorHeight + voxelSize * 6.0f -
        particle.CurrentContinuousPosition.y) / max(voxelSize * 8.0f, 0.001f));
    const float2 spreadDirection = SafeNormalize2(particle.CurrentContinuousPosition.xz +
        float2(HashUnitFloat(particle.GlobalVoxelId ^ EmitterBuffer.Seed) - 0.5f,
               HashUnitFloat(particle.GlobalVoxelId ^ EmitterBuffer.Seed ^ 0x68bc21ebu) - 0.5f));
    const float preStepBasinContact = saturate((EmitterBuffer.FloorHeight + voxelSize * 2.5f -
        particle.CurrentContinuousPosition.y) / max(voxelSize * 2.5f, 0.001f));
    const float2 basinBounds = float2(
        max(EmitterBuffer.WaterfallWidth * 0.75f, voxelSize * 4.0f),
        max(EmitterBuffer.WaterfallDepth * 2.0f, voxelSize * 6.0f));
    const uint poolLayer = HashVoxel(particle.GlobalVoxelId ^ EmitterBuffer.Seed ^ 0x91e10da5u) % 5u;
    const float poolSurfaceY = EmitterBuffer.FloorHeight + voxelSize * (0.35f + 0.28f * (float)poolLayer);
    const float basinResidenceSeconds = 3.25f +
        2.25f * HashUnitFloat(particle.GlobalVoxelId ^ EmitterBuffer.Seed ^ 0x4cf5ad43u);

    particle.Velocity += EmitterBuffer.Force * dt;
    particle.Velocity.xz += (float2(flowX, flowZ) + spreadDirection * floorSpread * 4.0f) * dt;
    particle.Velocity.xz += spreadDirection * preStepBasinContact * 6.0f * dt;
    particle.Velocity.y = lerp(particle.Velocity.y, -voxelSize * 1.25f, preStepBasinContact * 0.22f);
    particle.CurrentContinuousPosition += particle.Velocity * dt;
    if (preStepBasinContact > 0.0f)
    {
        particle.CurrentContinuousPosition.xz = clamp(
            particle.CurrentContinuousPosition.xz,
            -basinBounds,
            basinBounds);
        if (particle.CurrentContinuousPosition.y < poolSurfaceY)
        {
            particle.CurrentContinuousPosition.y = poolSurfaceY;
            particle.Velocity.y = max(particle.Velocity.y, 0.0f) * 0.15f;
        }
        particle.Velocity.xz *= lerp(1.0f, 0.92f, preStepBasinContact);
    }

    const float postStepBasinContact = saturate((EmitterBuffer.FloorHeight + voxelSize * 2.5f -
        particle.CurrentContinuousPosition.y) / max(voxelSize * 2.5f, 0.001f));
    const bool inPool = postStepBasinContact > 0.0f &&
        all(abs(particle.CurrentContinuousPosition.xz) <= basinBounds + voxelSize);
    particle.AgeSeconds += dt;
    particle.BasinAgeSeconds = inPool ? particle.BasinAgeSeconds + dt : 0.0f;

    if (particle.CurrentContinuousPosition.y <= EmitterBuffer.FloorHeight - EmitterBuffer.RecycleMargin ||
        particle.BasinAgeSeconds >= basinResidenceSeconds)
    {
        const float3 recyclePosition = DeterministicRecyclePosition(particle.GlobalVoxelId);
        particle.PreviousContinuousPosition = recyclePosition;
        particle.CurrentContinuousPosition = recyclePosition;
        particle.Velocity = DeterministicInitialVelocity(particle.GlobalVoxelId);
        particle.AgeSeconds = 0.0f;
        particle.BasinAgeSeconds = 0.0f;
        InterlockedAdd(SimulationStats[0], 1u);
    }

    ParticlesPool[aliveIndex] = particle;
#endif
}
