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

enum class VoxelPartitionId : uint8_t
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

static constexpr size_t VoxelPartitionCount = static_cast<size_t>(VoxelPartitionId::Count);

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
    DirectX::SimpleMath::Vector3 CurrentContinuousPosition = DirectX::SimpleMath::Vector3::Zero;
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
    DirectX::SimpleMath::Vector2 Padding = DirectX::SimpleMath::Vector2::Zero;
};

static_assert(sizeof(VoxelParticleData) == sizeof(ParticleData));
static_assert(sizeof(VoxelParticleData) == 48);
static_assert(offsetof(VoxelParticleData, CurrentContinuousPosition) == 36);
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
    DWORD Padding0 = 0;
    DWORD Padding1 = 0;
};

static_assert(sizeof(VoxelLodBuildData) == 80);

struct alignas(16) VoxelLodRenderItem
{
    DWORD ParticleIndex = 0;
    DWORD LodLevel = 0;
    DWORD Padding0 = 0;
    DWORD Padding1 = 0;
};

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

    uint32_t TotalRendered() const
    {
        return Lod0Rendered + Lod1Rendered + Lod2Rendered;
    }
};

struct VoxelPartitionState
{
    const char* DisplayName = "";
    const char* ObjectName = "";
    VoxelPartitionId PartitionId = VoxelPartitionId::PrimaryPartition;
    VoxelAdapterOwner AdapterOwner = VoxelAdapterOwner::Primary;
    uint32_t UpdateInterval = 1;
    uint32_t EffectiveUpdateInterval = 1;
    uint32_t StepsSinceLastUpdate = 0;
    float InterpolationPhase = 0.0f;
    float CoarseDeltaTime = 1.0f / 60.0f;
    uint64_t LastSimulationFrame = 0;
    bool UpdatedThisFrame = false;
    bool SimulationDispatchedThisFrame = false;
    UINT UpdatedVoxelCount = 0;
    std::vector<DWORD> GlobalVoxelIds;
    std::shared_ptr<VoxelGpuPartition> GpuPartition;

    uint32_t VoxelCount() const
    {
        return static_cast<uint32_t>(GlobalVoxelIds.size());
    }
};

struct VoxelPartitionRenderResult
{
    VoxelPartitionId PartitionId = VoxelPartitionId::PrimaryPartition;
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
    std::vector<const VoxelPartitionState*> PrimaryOwnedPartitions;
    std::vector<const VoxelPartitionState*> SecondaryOwnedPartitions;
};

struct VoxelWaterfallWorkload
{
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Rotation = DirectX::SimpleMath::Vector3::Zero;
    VoxelSimulationParameters Parameters{};
    uint32_t TotalVoxelCount = 1;
    float SecondaryShare = 0.35f;
    uint32_t TemporalDecimationInterval = 2;
    VoxelSpatialLodSettings SpatialLod{};
    std::array<VoxelPartitionState, VoxelPartitionCount> Partitions{};
};
