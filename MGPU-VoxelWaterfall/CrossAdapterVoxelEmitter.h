#pragma once
#include "Emitter.h"
#include "GCrossAdapterResource.h"
#include "GDescriptor.h"
#include "VoxelWaterfallEmitter.h"

class CrossAdapterVoxelEmitter : public Emitter
{
    std::shared_ptr<VoxelWaterfallEmitter> primeVoxelWaterfallEmitter;

    std::shared_ptr<GBuffer> ParticlesPool = nullptr;
    std::shared_ptr<CounteredStructBuffer<DWORD>> ParticlesAlive = nullptr;
    std::shared_ptr<CounteredStructBuffer<DWORD>> ParticlesDead = nullptr;
    std::shared_ptr<GBuffer> InjectedParticles = nullptr;

    std::shared_ptr<GCrossAdapterResource> CrossAdapterAliveIndexes;
    std::shared_ptr<GCrossAdapterResource> CrossAdapterDeadIndexes;
    std::shared_ptr<GCrossAdapterResource> CrossAdapterParticles;

    GDescriptor updateDescriptors;

    std::shared_ptr<ComputePSO> injectPSO;
    std::shared_ptr<ComputePSO> updatePSO;
    std::shared_ptr<GRootSignature> computeRS;

    std::vector<VoxelParticleData> newParticles;

    bool UseSharedCompute = false;
    bool enabled = true;


    std::shared_ptr<GDevice> secondDevice;

    enum Status : short
    {
        None = -1,
        Enable = 0,
        Disable = 1
    };

    Status DirtyActivated = None;

public:
    void InitPSO(const std::shared_ptr<GDevice>& otherDevice);
    void CreateBuffers();
    CrossAdapterVoxelEmitter(std::shared_ptr<GDevice> primeDevice, const std::shared_ptr<GDevice>& otherDevice,
                             DWORD particleCount, const VoxelSimulationParameters& initialParameters = {});
    void Update() override;;
    void Draw(const std::shared_ptr<GCommandList>& cmdList) override;
    void Dispatch(const std::shared_ptr<GCommandList>& cmdList) override;

    void ChangeParticleCount(UINT count);
    void ApplySettings(UINT count, const VoxelSimulationParameters& newParameters);
    UINT GetParticleCount() const;
    const VoxelSimulationParameters& GetParameters() const;
    void SetEnabled(bool value);
    bool IsEnabled() const;
    void SetUpdateInterval(uint32_t value);
    uint32_t GetUpdateInterval() const;
    void SetLastSimulationFrame(uint64_t value);
    uint64_t GetLastSimulationFrame() const;
    void SetSimulationDeltaTime(float value);
    UINT GetLastDispatchVoxelCount() const;
    bool IsSharedComputeEnabled() const;
    void CopySharedToPrimary(const std::shared_ptr<GCommandList>& cmdList);

    uint32_t UpdateInterval = 1;
    uint64_t LastSimulationFrame = 0;

    void EnableShared();;

    void DisableShared();;
};
