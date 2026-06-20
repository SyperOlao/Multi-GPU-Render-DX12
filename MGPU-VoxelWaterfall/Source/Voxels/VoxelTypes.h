#pragma once

#include "ShaderBuffersData.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>

class CrossAdapterVoxelEmitter;
class VoxelWaterfallEmitter;

enum class VoxelExecutionMode
{
    PrimaryOnly,
    SplitMultiGpu,
    SplitMultiGpuLod
};

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
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    float Reserved = 0.0f;
    DirectX::SimpleMath::Vector3 Velocity = DirectX::SimpleMath::Vector3::Zero;
    float Reserved1 = 0.0f;
    DWORD VoxelIndex = 0;
    DirectX::SimpleMath::Vector3 ContinuousPosition = DirectX::SimpleMath::Vector3::Zero;
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
};

static_assert(sizeof(VoxelParticleData) == sizeof(ParticleData));
static_assert(sizeof(VoxelEmitterData) == sizeof(EmitterData));

struct VoxelLodState
{
    const char* DisplayName = "";
    const char* ObjectName = "";
    bool Enabled = true;
    bool SettingsPending = false;
    int VoxelCount = 1;
    uint32_t UpdateInterval = 1;
    uint64_t LastSimulationFrame = 0;
    bool UpdatedThisFrame = false;
    UINT UpdatedVoxelCount = 0;
    VoxelSimulationParameters Parameters{};
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    std::shared_ptr<VoxelWaterfallEmitter> Emitter;
    std::shared_ptr<CrossAdapterVoxelEmitter> CrossEmitter;
};

static constexpr size_t NearVoxelWaterfall = 0;
static constexpr size_t MediumVoxelWaterfall = 1;
static constexpr size_t FarVoxelWaterfall = 2;
static constexpr size_t VoxelWaterfallLodCount = 3;
using VoxelLodArray = std::array<VoxelLodState, VoxelWaterfallLodCount>;
