struct ParticleData
{
    float3 PreviousContinuousPosition;
    float AgeSeconds;
    float3 Velocity;
    float FlowPhase;
    uint GlobalVoxelId;
    uint PackedGridCoordinate;
    uint MaterialId;
    uint StreamKind;
    float3 CurrentContinuousPosition;
    float Padding0;
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
    uint StreamKind;
    uint Padding;
};

struct VoxelLodRenderItem
{
    float3 PreviousCenter;
    float HalfExtentX;
    float3 CurrentCenter;
    float HalfExtentY;
    float HalfExtentZ;
    uint LodLevel;
    uint MaterialId;
    uint StreamKind;
    uint RepresentativeIndex;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};
