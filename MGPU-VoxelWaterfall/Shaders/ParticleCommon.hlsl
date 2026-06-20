struct ParticleData
{
    float3 Position;
    float Reserved;
    float3 Velocity;
    float Reserved1;
    uint VoxelIndex;
    float3 ContinuousPosition;
};

struct EmitterData
{
    float4 Color;
    float3 Force;
    float DeltaTime;

    float VoxelSize;
    float SpawnHeight;
    uint ParticlesTotalCount;
    uint SimulatedGroupCount;

    uint ParticleInjectCount;
    uint InjectGroupCount;
    uint ParticleAliveCount;
    float FloorHeight;

    float WaterfallWidth;
    float WaterfallDepth;
    float InitialFallSpeed;
    uint Seed;
};
