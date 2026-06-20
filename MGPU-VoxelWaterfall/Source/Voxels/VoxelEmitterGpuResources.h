#pragma once

#include "DirectXBuffers.h"
#include "GDescriptor.h"
#include "Source/Voxels/VoxelTypes.h"

#include <memory>
#include <vector>

class VoxelEmitterGpuResources
{
public:
    std::shared_ptr<PEPEngine::Graphics::ConstantUploadBuffer<ObjectConstants>> ObjectPositionBuffer;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> ParticlesPool;
    std::shared_ptr<PEPEngine::Graphics::CounteredStructBuffer<DWORD>> ParticlesAlive;
    std::shared_ptr<PEPEngine::Graphics::CounteredStructBuffer<DWORD>> ParticlesDead;
    std::shared_ptr<PEPEngine::Graphics::GBuffer> InjectedParticles;

    std::vector<VoxelParticleData> NewParticles;
    PEPEngine::Graphics::GDescriptor ComputeDescriptors;
    PEPEngine::Graphics::GDescriptor RenderDescriptors;
    DWORD InjectionCapacity = 1;

    void AllocateDescriptors(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device);
    void ResetParticleBuffers();
    void EnsureObjectPositionBuffer(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device);
    void CreateParticleBuffers(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, DWORD particleCount);
    void InitializeDeadParticleList(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, DWORD particleCount) const;
    void CreateParticleViews();
    void ResizeInjectionScratch();
};
