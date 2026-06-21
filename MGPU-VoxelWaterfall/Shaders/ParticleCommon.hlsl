struct ParticleData
{
    float3 PreviousContinuousPosition;
    float AgeSeconds;
    float3 Velocity;
    float FlowPhase;
    uint GlobalVoxelId;
    float3 CurrentContinuousPosition;
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

    float SimulationTime;
    float InterpolationAlpha;
    float RecycleMargin;
    float GridSnapEnabled;

    uint SpatialLodDebugMode;
    uint AdapterOwner;
    float2 Padding;
};

struct VoxelLodRenderItem
{
    uint ParticleIndex;
    uint LodLevel;
    uint Padding0;
    uint Padding1;
};
