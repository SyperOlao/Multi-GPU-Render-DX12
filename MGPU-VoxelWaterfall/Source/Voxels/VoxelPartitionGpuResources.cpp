#include "pch.h"
#include "Source/Voxels/VoxelPartitionGpuResources.h"

#include "GCommandList.h"
#include "GCommandQueue.h"
#include "GDevice.h"

#include <numeric>

using PEPEngine::Graphics::CounteredStructBuffer;
using PEPEngine::Graphics::GBuffer;

void VoxelPartitionGpuResources::AllocateDescriptors(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device)
{
    ComputeDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5);
    RenderDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
}

void VoxelPartitionGpuResources::ResetResources()
{
    ParticlesPool.reset();
    InjectedParticles.reset();
    ParticlesAlive.reset();
    ParticlesDead.reset();
    SimulationStats.reset();
    SimulationStatsUpload.reset();
    SimulationStatsReadback.reset();
}

void VoxelPartitionGpuResources::EnsureObjectPositionBuffer(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device)
{
    if (!ObjectPositionBuffer)
        ObjectPositionBuffer = std::make_shared<PEPEngine::Graphics::ConstantUploadBuffer<ObjectConstants>>(
            device, 1, L"Voxel Emitter Position");
}

void VoxelPartitionGpuResources::CreateParticleBuffers(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, const DWORD particleCount)
{
    const auto particleStride = static_cast<UINT>(sizeof(VoxelParticleData));

    ParticlesPool = std::make_shared<GBuffer>(device, particleStride, particleCount,
                                              L"Voxel Pool Buffer", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    InjectedParticles = std::make_shared<GBuffer>(device, particleStride, InjectionCapacity,
                                                  L"Injected Voxel Buffer", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    SimulationStats = std::make_shared<GBuffer>(device, static_cast<UINT>(sizeof(DWORD)), 1u,
                                                L"Voxel Simulation Stats",
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    SimulationStatsUpload = std::make_shared<PEPEngine::Graphics::UploadBuffer>(
        device, 1u, static_cast<UINT>(sizeof(DWORD)), L"Voxel Simulation Stats Upload");
    SimulationStatsReadback = std::make_shared<PEPEngine::Graphics::ReadBackBuffer<DWORD>>(
        device, 1, L"Voxel Simulation Stats Readback");

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
void VoxelPartitionGpuResources::InitializeDeadParticleList(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, const DWORD particleCount) const
{
    std::vector<UINT> deadIndices(particleCount);
    std::iota(deadIndices.begin(), deadIndices.end(), 0u);
    auto queue = device->GetCommandQueue();
    auto initList = queue->GetCommandList();
    ParticlesDead->LoadData(deadIndices.data(), initList);
    ParticlesDead->SetCounterValue(initList, particleCount);
    const DWORD zero = 0;
    SimulationStatsUpload->CopyData(0, &zero, sizeof(DWORD));
    initList->TransitionBarrier(SimulationStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
    initList->FlushResourceBarriers();
    initList->CopyBufferRegion(*SimulationStats, 0, *SimulationStatsUpload, 0, sizeof(DWORD), false);
    initList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->TransitionBarrier(SimulationStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->FlushResourceBarriers();
    queue->WaitForFenceValue(queue->ExecuteCommandList(initList));
}
#pragma warning(pop)

void VoxelPartitionGpuResources::CreateParticleViews()
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

    uavDesc.Buffer.NumElements = SimulationStats->GetElementCount();
    uavDesc.Buffer.StructureByteStride = SimulationStats->GetStride();
    SimulationStats->CreateUnorderedAccessView(&uavDesc, &ComputeDescriptors, 4);

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

void VoxelPartitionGpuResources::ResizeInjectionScratch()
{
    NewParticles.resize(InjectionCapacity);
}
