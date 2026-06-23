#pragma once

#include "ShaderBuffersData.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VoxelGpuPartition;

enum class VoxelExecutionMode
{
    SingleGpuFull,
    MultiGpuFull,
    SingleGpuTemporalDecimation,
    MultiGpuTemporalDecimation
};

enum class VoxelAdapterPartitionId : uint8_t
{
    PrimaryPartition = 0,
    SecondaryPartition,
    Count
};

enum class VoxelAdapterOwner : uint8_t
{
    Primary,
    Secondary
};

enum class VoxelCompositeDebugView : uint32_t
{
    FinalComposite,
    PrimaryOnly,
    SecondaryColorOnly,
    SecondaryLinearDepth,
    PrimaryLinearDepth,
    PartitionOwnershipColors,
    SpatialLodColors,
    DepthDifference
};

enum class VoxelSpatialLodMode : uint32_t
{
    Off,
    ThreeLevel
};

enum class VoxelSpatialLodDebugMode : uint32_t
{
    None,
    LodLevel,
    AdapterOwnership
};

enum class VoxelSceneLayerType : uint8_t
{
    Static,
    Dynamic
};

enum class StaticVoxelStorageMode : uint32_t
{
    SurfaceOnly,
    DenseSolidStress
};

enum class StaticVoxelBudgetPreset : uint32_t
{
    Small,
    Medium,
    Large,
    VeryLarge
};

enum class DynamicVoxelBudgetPreset : uint32_t
{
    Small,
    Medium,
    Large,
    VeryLarge
};

enum class VoxelResearchWorkloadProfile : uint32_t
{
    StaticRenderOnly,
    DynamicSimulationAndRender,
    MixedStaticAndDynamic,
    OcclusionValidation,
    SpatialLodDemonstration,
    DemoMixed
};

enum class VoxelPartitionStrategy : uint32_t
{
    HashedChunks,
    SpatialPlane
};

enum class VoxelLoadBalanceScenario : uint32_t
{
    Balanced,
    PrimaryHeavy,
    SecondaryHeavy
};

enum class VoxelTemporalPolicy : uint32_t
{
    Full,
    Decimated
};

enum class VoxelBenchmarkConfigClass : uint32_t
{
    ValidMatchingBenchmark,
    Diagnostic,
    InvalidMixedQuality
};

enum class VoxelResearchCameraMode : uint32_t
{
    Interactive,
    FixedOverview,
    FixedOcclusion,
    WaterfallCloseup,
    LodSweepRoute,
    BenchmarkRoute,
    DemoMixedOverview
};

enum class VoxelResearchLightingPreset : uint32_t
{
    BenchmarkNeutral,
    DemoStaticSky,
    OcclusionValidationLighting
};

enum class VoxelRenderResolutionPreset : uint32_t
{
    R1280x720,
    R1920x1080,
    R2560x1440,
    R3840x2160
};

using VoxelLayerId = uint32_t;
using VoxelGlobalId = uint64_t;
using VoxelChunkId = uint64_t;

static constexpr size_t VoxelAdapterPartitionCount = static_cast<size_t>(VoxelAdapterPartitionId::Count);

struct VoxelSimulationParameters
{
    float VoxelSize = 0.75f;
    float SpawnHeight = 35.0f;
    float FloorHeight = 0.0f;
    float WaterfallWidth = 18.0f;
    float WaterfallDepth = 6.0f;
    float InitialFallSpeed = 3.0f;
    float Gravity = 18.0f;
    DWORD Seed = 1337;
};

struct alignas(16) VoxelParticleData
{
    DirectX::SimpleMath::Vector3 PreviousContinuousPosition = DirectX::SimpleMath::Vector3::Zero;
    float AgeSeconds = 0.0f;
    DirectX::SimpleMath::Vector3 Velocity = DirectX::SimpleMath::Vector3::Zero;
    float FlowPhase = 0.0f;
    DWORD GlobalVoxelId = 0;
    DWORD PackedGridCoordinate = 0;
    DWORD MaterialId = 0;
    DWORD StreamKind = 0;
    DirectX::SimpleMath::Vector3 CurrentContinuousPosition = DirectX::SimpleMath::Vector3::Zero;
    float BasinAgeSeconds = 0.0f;
};

struct alignas(16) VoxelEmitterData
{
    DirectX::SimpleMath::Vector4 Color = DirectX::SimpleMath::Vector4(0.02f, 0.48f, 0.95f, 1.0f);
    DirectX::SimpleMath::Vector3 Force = DirectX::SimpleMath::Vector3(0.0f, -18.0f, 0.0f);
    float DeltaTime = 1.0f / 60.0f;

    float VoxelSize = 0.75f;
    float SpawnHeight = 35.0f;
    DWORD ParticlesTotalCount = 0;
    DWORD SimulatedGroupCount = 0;

    DWORD ParticleInjectCount = 0;
    DWORD InjectedGroupCount = 0;
    DWORD ParticlesAliveCount = 0;
    float FloorHeight = 0.0f;

    float WaterfallWidth = 18.0f;
    float WaterfallDepth = 6.0f;
    float InitialFallSpeed = 3.0f;
    DWORD Seed = 1337;

    float SimulationTime = 0.0f;
    float InterpolationAlpha = 0.0f;
    float RecycleMargin = 4.0f;
    float GridSnapEnabled = 0.0f;

    DWORD SpatialLodDebugMode = 0;
    DWORD AdapterOwner = 0;
    DWORD StreamKind = 0;
    DWORD Padding = 0;
};

static_assert(sizeof(VoxelParticleData) == 64);
static_assert(offsetof(VoxelParticleData, CurrentContinuousPosition) == 48);
static_assert(offsetof(VoxelParticleData, BasinAgeSeconds) == 60);
static_assert(sizeof(VoxelEmitterData) == 112);
static_assert(offsetof(VoxelEmitterData, SimulationTime) == 80);

struct alignas(16) VoxelLodBuildData
{
    DirectX::SimpleMath::Vector3 CameraPosition = DirectX::SimpleMath::Vector3::Zero;
    float Lod0Distance = 45.0f;

    DirectX::SimpleMath::Vector3 ObjectPosition = DirectX::SimpleMath::Vector3::Zero;
    float Lod1Distance = 120.0f;

    float Hysteresis = 8.0f;
    float VoxelSize = 0.75f;
    float SpawnHeight = 35.0f;
    float FloorHeight = 0.0f;

    float WaterfallWidth = 18.0f;
    float WaterfallDepth = 6.0f;
    DWORD Seed = 1337;
    DWORD AliveCount = 0;

    DWORD SpatialLodMode = 0;
    DWORD AdapterOwner = 0;
    DWORD StreamKind = 0;
    DWORD GroupTableCapacity = 0;

    int32_t GridOriginX = 0;
    int32_t GridOriginY = 0;
    int32_t GridOriginZ = 0;
    DWORD MaxProbeCount = 64;
};

static_assert(sizeof(VoxelLodBuildData) == 96);

struct alignas(16) VoxelLodRenderItem
{
    DirectX::SimpleMath::Vector3 PreviousCenter = DirectX::SimpleMath::Vector3::Zero;
    float HalfExtentX = 0.0f;
    DirectX::SimpleMath::Vector3 CurrentCenter = DirectX::SimpleMath::Vector3::Zero;
    float HalfExtentY = 0.0f;
    float HalfExtentZ = 0.0f;
    DWORD LodLevel = 0;
    DWORD MaterialId = 0;
    DWORD StreamKind = 0;
    DWORD RepresentativeIndex = 0;
    DWORD Padding0 = 0;
    DWORD Padding1 = 0;
    DWORD Padding2 = 0;
};

static_assert(sizeof(VoxelLodRenderItem) == 64);

struct VoxelSpatialLodSettings
{
    VoxelSpatialLodMode Mode = VoxelSpatialLodMode::Off;
    float Lod0Distance = 45.0f;
    float Lod1Distance = 120.0f;
    float Hysteresis = 8.0f;
    bool FreezeCamera = false;
    VoxelSpatialLodDebugMode DebugMode = VoxelSpatialLodDebugMode::None;
};

struct VoxelSpatialLodStats
{
    uint32_t Lod0Rendered = 0;
    uint32_t Lod1Rendered = 0;
    uint32_t Lod2Rendered = 0;
    uint32_t Aggregated = 0;
    uint32_t ProbeOverflow = 0;
    uint32_t MaxProbeCount = 0;
    uint32_t TotalProbeCount = 0;
    uint32_t EmittedGroups = 0;
    uint32_t HashCapacity = 0;

    uint32_t TotalRendered() const
    {
        return Lod0Rendered + Lod1Rendered + Lod2Rendered;
    }

    float HashLoadFactor(uint32_t capacity) const
    {
        return capacity > 0 ? static_cast<float>(EmittedGroups) / static_cast<float>(capacity) : 0.0f;
    }

    float HashLoadFactor() const
    {
        return HashLoadFactor(HashCapacity);
    }
};

struct VoxelGridCoordinate
{
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Z = 0;
};

struct VoxelStaticEnvironmentTelemetry
{
    uint32_t OccupiedCells = 0;
    uint32_t SurfaceVoxels = 0;
    uint32_t HiddenInteriorVoxels = 0;
    uint32_t ChunkCount = 0;
    uint32_t PrimaryStaticVoxels = 0;
    uint32_t SecondaryStaticVoxels = 0;
    uint32_t ActualRenderedStaticVoxels = 0;
    uint32_t RequestedVoxelBudget = 0;
    uint32_t GenerationSeed = 0;
    StaticVoxelBudgetPreset BudgetPreset = StaticVoxelBudgetPreset::Small;
    StaticVoxelStorageMode StorageMode = StaticVoxelStorageMode::SurfaceOnly;
    float VoxelSize = 0.65f;
    VoxelGridCoordinate GridOrigin{};
    DirectX::SimpleMath::Vector3 BoundsMin = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 BoundsMax = DirectX::SimpleMath::Vector3::Zero;
};

struct VoxelChunkSize
{
    uint32_t Width = 8;
    uint32_t Height = 8;
    uint32_t Depth = 4;
};

struct VoxelLayerRenderSettings
{
    DirectX::SimpleMath::Vector4 Color = DirectX::SimpleMath::Vector4(0.02f, 0.48f, 0.95f, 1.0f);
    float VoxelSize = 0.75f;
};

struct VoxelLayerSimulationPolicy
{
    bool Enabled = false;
    uint32_t UpdateInterval = 1;
};

struct VoxelLayerSpatialLodPolicy
{
    VoxelSpatialLodSettings Settings{};
    VoxelChunkSize ChunkSize{};
};

struct VoxelPartitionDrawStream
{
    VoxelLayerId LayerId = 0;
    VoxelSceneLayerType LayerType = VoxelSceneLayerType::Dynamic;
    std::vector<VoxelGlobalId> GlobalVoxelIds;
    std::vector<DWORD> SimulationVoxelIds;
    std::vector<VoxelChunkId> ChunkIds;
    std::vector<VoxelGridCoordinate> GridCoordinates;
    std::vector<uint32_t> MaterialIds;
    VoxelGridCoordinate GridOrigin{};
    VoxelSimulationParameters SimulationParameters{};
    VoxelLayerRenderSettings RenderSettings{};
    VoxelLayerSimulationPolicy SimulationPolicy{};
    VoxelLayerSpatialLodPolicy SpatialLodPolicy{};

    uint32_t VoxelCount() const
    {
        return static_cast<uint32_t>(GlobalVoxelIds.size());
    }
};

struct VoxelAdapterPartition
{
    const char* DisplayName = "";
    const char* ObjectName = "";
    VoxelAdapterPartitionId PartitionId = VoxelAdapterPartitionId::PrimaryPartition;
    VoxelAdapterOwner AdapterOwner = VoxelAdapterOwner::Primary;
    VoxelLayerId LayerId = 0;
    VoxelSceneLayerType LayerType = VoxelSceneLayerType::Dynamic;
    VoxelLayerSimulationPolicy SimulationPolicy{true, 1};
    uint32_t UpdateInterval = 1;
    uint32_t EffectiveUpdateInterval = 1;
    uint32_t StepsSinceLastUpdate = 0;
    float InterpolationPhase = 0.0f;
    float CoarseDeltaTime = 1.0f / 60.0f;
    uint64_t LastSimulationFrame = 0;
    bool UpdatedThisFrame = false;
    bool SimulationDispatchedThisFrame = false;
    UINT UpdatedVoxelCount = 0;
    std::vector<VoxelGlobalId> GlobalVoxelIds;
    std::vector<DWORD> SimulationVoxelIds;
    std::vector<VoxelChunkId> ChunkIds;
    std::vector<VoxelGridCoordinate> GridCoordinates;
    std::vector<uint32_t> MaterialIds;
    std::vector<VoxelPartitionDrawStream> DrawStreams;
    VoxelSimulationParameters Parameters{};
    VoxelSpatialLodSettings SpatialLod{};
    std::shared_ptr<VoxelGpuPartition> GpuPartition;

    uint32_t VoxelCount() const
    {
        return static_cast<uint32_t>(GlobalVoxelIds.size());
    }

    bool HasDynamicVoxels() const
    {
        for (const auto& stream : DrawStreams)
        {
            if (stream.LayerType == VoxelSceneLayerType::Dynamic && stream.VoxelCount() > 0)
                return true;
        }
        return false;
    }
};

struct VoxelPartitionRenderResult
{
    VoxelAdapterPartitionId PartitionId = VoxelAdapterPartitionId::PrimaryPartition;
    bool DrawIssued = false;
    uint32_t DrawCallCount = 0;
    uint32_t SubmittedVoxelCount = 0;
    uint32_t LogicalVoxelCount = 0;
    bool UsedIndirectDraw = false;
    VoxelSpatialLodStats LodStats{};
    VoxelAdapterOwner OwnerAdapter = VoxelAdapterOwner::Primary;
    std::wstring OwnerAdapterName;
    std::wstring CommandListAdapterName;
};

struct VoxelRenderWorkload
{
    std::vector<const VoxelAdapterPartition*> PrimaryOwnedPartitions;
    std::vector<const VoxelAdapterPartition*> SecondaryOwnedPartitions;
    uint32_t LogicalVoxelCount = 0;
};

struct VoxelSceneLayer
{
    VoxelLayerId LayerId = 0;
    VoxelSceneLayerType LayerType = VoxelSceneLayerType::Dynamic;
    std::string DisplayName;
    std::vector<VoxelGlobalId> GlobalVoxelIds;
    std::vector<DWORD> SimulationVoxelIds;
    std::vector<VoxelChunkId> ChunkIds;
    std::vector<VoxelGridCoordinate> GridCoordinates;
    std::vector<uint32_t> MaterialIds;
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Rotation = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Matrix WorldTransform = DirectX::SimpleMath::Matrix::Identity;
    DirectX::SimpleMath::Vector3 BoundsMin = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 BoundsMax = DirectX::SimpleMath::Vector3::Zero;
    VoxelGridCoordinate GridOrigin{};
    VoxelLayerRenderSettings RenderSettings{};
    std::array<VoxelPartitionDrawStream, VoxelAdapterPartitionCount> AdapterPartitions{};
    VoxelLayerSimulationPolicy SimulationPolicy{};
    VoxelLayerSpatialLodPolicy SpatialLodPolicy{};
    VoxelSimulationParameters SimulationParameters{};

    uint32_t LogicalVoxelCount() const
    {
        return static_cast<uint32_t>(GlobalVoxelIds.size());
    }
};

struct VoxelSceneWorkload
{
    VoxelResearchWorkloadProfile Profile = VoxelResearchWorkloadProfile::MixedStaticAndDynamic;
    std::string ScenePreset = "MixedVoxelEnvironment";
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Rotation = DirectX::SimpleMath::Vector3::Zero;
    VoxelSimulationParameters Parameters{};
    uint32_t TotalVoxelCount = 0;
    std::vector<VoxelSceneLayer> Layers;
    uint32_t StaticVoxelBudget = 100000;
    StaticVoxelBudgetPreset StaticBudgetPreset = StaticVoxelBudgetPreset::Small;
    StaticVoxelStorageMode StaticStorageMode = StaticVoxelStorageMode::SurfaceOnly;
    uint32_t StaticGenerationSeed = 1337;
    float StaticVoxelSize = 0.65f;
    VoxelStaticEnvironmentTelemetry StaticTelemetry{};
    uint32_t DynamicVoxelBudget = 25000;
    DynamicVoxelBudgetPreset DynamicBudgetPreset = DynamicVoxelBudgetPreset::Small;
    uint32_t ActualStaticVoxelCount = 0;
    uint32_t ActualDynamicVoxelCount = 0;
    VoxelPartitionStrategy PartitionStrategy = VoxelPartitionStrategy::HashedChunks;
    VoxelLoadBalanceScenario LoadBalanceScenario = VoxelLoadBalanceScenario::Balanced;
    float SecondaryShare = 0.35f;
    VoxelTemporalPolicy TemporalPolicy = VoxelTemporalPolicy::Full;
    uint32_t TemporalDecimationInterval = 2;
    VoxelSpatialLodSettings SpatialLod{};
    VoxelChunkSize ChunkSize{};
    std::string CameraPath = "FixedOverview";
    std::string LightingPreset = "FixedNeutralDirectional";
    VoxelResearchCameraMode CameraMode = VoxelResearchCameraMode::FixedOverview;
    VoxelResearchLightingPreset LightingMode = VoxelResearchLightingPreset::BenchmarkNeutral;
    VoxelRenderResolutionPreset ResolutionPreset = VoxelRenderResolutionPreset::R1920x1080;
    uint32_t RenderResolutionWidth = 1920;
    uint32_t RenderResolutionHeight = 1080;
    bool DynamicShadowsEnabled = false;
    VoxelBenchmarkConfigClass BenchmarkConfigClass = VoxelBenchmarkConfigClass::ValidMatchingBenchmark;
    std::string BenchmarkConfigReason;
    std::vector<VoxelAdapterPartition> Partitions;
};
