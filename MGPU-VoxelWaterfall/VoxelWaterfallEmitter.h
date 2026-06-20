#pragma once

#include "Emitter.h"
#include "GDescriptor.h"

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
    Vector3 Position = Vector3::Zero;
    float Reserved = 0.0f;
    Vector3 Velocity = Vector3::Zero;
    float Reserved1 = 0.0f;
    DWORD VoxelIndex = 0;
    Vector3 ContinuousPosition = Vector3::Zero;
};

struct alignas(16) VoxelEmitterData
{
    Vector4 Color = Vector4(0.02f, 0.48f, 0.95f, 1.0f);
    Vector3 Force = Vector3(0.0f, -18.0f, 0.0f);
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

class VoxelWaterfallEmitter : public Emitter
{
    friend class CrossAdapterVoxelEmitter;

    std::shared_ptr<ConstantUploadBuffer<ObjectConstants>> objectPositionBuffer;
    std::shared_ptr<GBuffer> ParticlesPool;
    std::shared_ptr<CounteredStructBuffer<DWORD>> ParticlesAlive;
    std::shared_ptr<CounteredStructBuffer<DWORD>> ParticlesDead;
    std::shared_ptr<GBuffer> InjectedParticles;

    std::vector<VoxelParticleData> newParticles;
    GDescriptor particlesComputeDescriptors;
    GDescriptor particlesRenderDescriptors;

    VoxelEmitterData emitterData{};
    VoxelSimulationParameters parameters{};
    ObjectConstants objectWorldData{};

    DWORD injectionCapacity = 1;
    DWORD nextSpawnIndex = 0;
    bool isWorked = false;
    bool enabled = true;
    DWORD lastDispatchVoxelCount = 0;

    double CalculateGroupCount(DWORD particleCount) const;
    VoxelParticleData GenerateVoxelParticle(DWORD index) const;
    void PSOInitialize();
    void DescriptorInitialize();
    void BufferInitialize();

protected:
    void Update() override;
    void Draw(const std::shared_ptr<GCommandList>& cmdList) override;

public:
    VoxelWaterfallEmitter(const std::shared_ptr<GDevice>& primeDevice, DWORD particleCount,
                          const VoxelSimulationParameters& initialParameters = {});

    void Dispatch(const std::shared_ptr<GCommandList>& cmdList) override;
    void ChangeParticleCount(UINT count);
    void ApplySettings(UINT count, const VoxelSimulationParameters& newParameters);
    const VoxelSimulationParameters& GetParameters() const;
    UINT GetParticleCount() const;
    void SetEnabled(bool value);
    bool IsEnabled() const;
    void SetUpdateInterval(uint32_t value);
    uint32_t GetUpdateInterval() const;
    void SetLastSimulationFrame(uint64_t value);
    uint64_t GetLastSimulationFrame() const;
    void SetSimulationDeltaTime(float value);
    UINT GetLastDispatchVoxelCount() const;

    uint32_t UpdateInterval = 1;
    uint64_t LastSimulationFrame = 0;
};
