#pragma once

#include "DirectXBuffers.h"
#include "GDescriptor.h"
#include "Source/Voxels/VoxelTypes.h"

#include <memory>
#include <string>
#include <vector>

struct VoxelGpuResourceOwner
{
    LUID AdapterLuid{};
    std::wstring DeviceName;
    bool IsValid = false;
};

class VoxelPartitionGpuResources
{
public:
    VoxelGpuResourceOwner Owner;
    std::shared_ptr<PEPEngine::Graphics::ConstantUploadBuffer<ObjectConstants>> ObjectPositionBuffer;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> ParticlesPool;
    std::shared_ptr<PEPEngine::Graphics::CounteredStructBuffer<DWORD>> ParticlesAlive;
    std::shared_ptr<PEPEngine::Graphics::CounteredStructBuffer<DWORD>> ParticlesDead;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> InjectedParticles;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> SimulationStats;
    std::shared_ptr<PEPEngine::Graphics::UploadBuffer> SimulationStatsUpload;
    std::shared_ptr<PEPEngine::Graphics::ReadBackBuffer<DWORD>> SimulationStatsReadback;
    std::shared_ptr<PEPEngine::Graphics::CounteredStructBuffer<VoxelLodRenderItem>> LodRenderItems;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> LodGroupKeys;
    std::shared_ptr<PEPEngine::Graphics::UploadBuffer> LodGroupKeysUpload;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> LodDrawArguments;
    std::shared_ptr<PEPEngine::Graphics::UploadBuffer> LodDrawArgumentsUpload;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> LodStats;
    std::shared_ptr<PEPEngine::Graphics::UploadBuffer> LodStatsUpload;
    std::shared_ptr<PEPEngine::Graphics::ReadBackBuffer<DWORD>> LodStatsReadback;
    DWORD LodGroupTableCapacity = 0;

    std::vector<VoxelParticleData> NewParticles;
    PEPEngine::Graphics::GDescriptor ComputeDescriptors;
    PEPEngine::Graphics::GDescriptor RenderDescriptors;
    PEPEngine::Graphics::GDescriptor LodBuildDescriptors;
    DWORD InjectionCapacity = 1;

    void SetOwnerDevice(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device);
    void AllocateDescriptors(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device);
    void ResetResources();
    void EnsureObjectPositionBuffer(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device);
    void CreateParticleBuffers(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, DWORD particleCount);
    void InitializeDeadParticleList(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, DWORD particleCount) const;
    void InitializeLodState(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, DWORD particleCount) const;
    void CreateParticleViews();
    void ResizeInjectionScratch();
};
