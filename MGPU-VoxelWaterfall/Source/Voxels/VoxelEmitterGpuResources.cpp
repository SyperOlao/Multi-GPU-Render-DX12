#include "pch.h"
#include "Source/Voxels/VoxelEmitterGpuResources.h"

#include "GCommandList.h"
#include "GCommandQueue.h"
#include "GDevice.h"

#include <numeric>

using PEPEngine::Graphics::CounteredStructBuffer;
using PEPEngine::Graphics::GBuffer;

void VoxelEmitterGpuResources::AllocateDescriptors(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device)
{
    ComputeDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
    RenderDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
}

void VoxelEmitterGpuResources::ResetParticleBuffers()
{
    ParticlesPool.reset();
    InjectedParticles.reset();
    ParticlesAlive.reset();
    ParticlesDead.reset();
}

void VoxelEmitterGpuResources::EnsureObjectPositionBuffer(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device)
{
    if (!ObjectPositionBuffer)
        ObjectPositionBuffer = std::make_shared<PEPEngine::Graphics::ConstantUploadBuffer<ObjectConstants>>(
            device, 1, L"Voxel Emitter Position");
}

void VoxelEmitterGpuResources::CreateParticleBuffers(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, const DWORD particleCount)
{
    const auto particleStride = static_cast<UINT>(sizeof(VoxelParticleData));

    ParticlesPool = std::make_shared<GBuffer>(device, particleStride, particleCount,
                                              L"Voxel Pool Buffer", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    InjectedParticles = std::make_shared<GBuffer>(device, particleStride, InjectionCapacity,
                                                  L"Injected Voxel Buffer", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

#pragma warning(push)
#pragma warning(disable : 4267)
    ParticlesAlive = std::make_shared<CounteredStructBuffer<DWORD>>(device, particleCount,
                                                                    L"Voxel Alive Index Buffer");
    ParticlesDead = std::make_shared<CounteredStructBuffer<DWORD>>(device, particleCount,
                                                                   L"Voxel Dead Index Buffer");
#pragma warning(pop)
}

#pragma warning(push)
#pragma warning(disable : 4267)
void VoxelEmitterGpuResources::InitializeDeadParticleList(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, const DWORD particleCount) const
{
    std::vector<UINT> deadIndices(particleCount);
    std::iota(deadIndices.begin(), deadIndices.end(), 0u);
    auto queue = device->GetCommandQueue();
    auto initList = queue->GetCommandList();
    ParticlesDead->LoadData(deadIndices.data(), initList);
    ParticlesDead->SetCounterValue(initList, particleCount);
    initList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->FlushResourceBarriers();
    queue->WaitForFenceValue(queue->ExecuteCommandList(initList));
}
#pragma warning(pop)

void VoxelEmitterGpuResources::CreateParticleViews()
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    ParticlesPool->CreateUnorderedAccessView(&uavDesc, &ComputeDescriptors, 0);

    uavDesc.Buffer.NumElements = ParticlesDead->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesDead->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = ParticlesDead->GetBufferSize() - sizeof(DWORD);
    ParticlesDead->CreateUnorderedAccessView(&uavDesc, &ComputeDescriptors, 1, ParticlesDead->GetD3D12Resource());
    ParticlesAlive->CreateUnorderedAccessView(&uavDesc, &ComputeDescriptors, 2, ParticlesAlive->GetD3D12Resource());

    uavDesc.Buffer.NumElements = InjectedParticles->GetElementCount();
    uavDesc.Buffer.StructureByteStride = InjectedParticles->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    InjectedParticles->CreateUnorderedAccessView(&uavDesc, &ComputeDescriptors, 3);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    srvDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    ParticlesPool->CreateShaderResourceView(&srvDesc, &RenderDescriptors, 0);

    srvDesc.Buffer.NumElements = ParticlesAlive->GetElementCount();
    srvDesc.Buffer.StructureByteStride = ParticlesAlive->GetStride();
    ParticlesAlive->CreateShaderResourceView(&srvDesc, &RenderDescriptors, 1);
}

void VoxelEmitterGpuResources::ResizeInjectionScratch()
{
    NewParticles.resize(InjectionCapacity);
}
