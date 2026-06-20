#pragma once

#include "Emitter.h"
#include "GDescriptor.h"
#include "Source/Voxels/VoxelEmitterGpuResources.h"
#include "Source/Voxels/VoxelTypes.h"

class VoxelWaterfallEmitter : public Emitter
{
    VoxelEmitterGpuResources gpuResources;
    VoxelEmitterData emitterData{};
    VoxelSimulationParameters parameters{};
    ObjectConstants objectWorldData{};

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
    void UpdateFromCrossAdapterBridge();
    void DrawFromCrossAdapterBridge(const std::shared_ptr<GCommandList>& cmdList);
    void ChangeParticleCount(UINT count);
    void ApplySettings(UINT count, const VoxelSimulationParameters& newParameters);
    const VoxelSimulationParameters& GetParameters() const;
    UINT GetParticleCount() const;
    VoxelEmitterData& GetEmitterData();
    const VoxelEmitterData& GetEmitterData() const;
    GBuffer& GetParticlesPool() const;
    CounteredStructBuffer<DWORD>& GetParticlesAlive() const;
    CounteredStructBuffer<DWORD>& GetParticlesDead() const;
    bool HasStartedSimulation() const;
    VoxelParticleData GenerateParticleForIndex(DWORD index) const;
    DWORD ConsumeNextSpawnIndex(DWORD count);
    double CalculateDispatchGroupCount(DWORD particleCount) const;
    void SetLastDispatchVoxelCount(UINT count);
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
