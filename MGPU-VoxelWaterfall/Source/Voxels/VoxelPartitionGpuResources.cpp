#include "pch.h"
#include "Source/Voxels/VoxelPartitionGpuResources.h"

#include "GCommandList.h"
#include "GCommandQueue.h"
#include "GDevice.h"

#include <algorithm>
#include <numeric>

using PEPEngine::Graphics::CounteredStructBuffer;
using PEPEngine::Graphics::GBuffer;

namespace
{
    DWORD NextPowerOfTwo(DWORD value)
    {
        if (value <= 1u)
            return 1u;
        --value;
        value |= value >> 1u;
        value |= value >> 2u;
        value |= value >> 4u;
        value |= value >> 8u;
        value |= value >> 16u;
        return value + 1u;
    }
}

void VoxelPartitionGpuResources::SetOwnerDevice(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device)
{
    Owner = {};
    if (!device)
        return;

    Owner.AdapterLuid = device->GetDesc().AdapterLuid;
    Owner.DeviceName = device->GetName();
    Owner.IsValid = true;
}

void VoxelPartitionGpuResources::AllocateDescriptors(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device)
{
    ComputeDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5);
    RenderDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
    LodBuildDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6);
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
    LodRenderItems.reset();
    LodGroupKeys.reset();
    LodGroupKeysUpload.reset();
    LodDrawArguments.reset();
    LodDrawArgumentsUpload.reset();
    LodStats.reset();
    LodStatsUpload.reset();
    LodStatsReadback.reset();
    LodGroupTableCapacity = 0;
    NewParticles.clear();
    InjectionCapacity = 0;
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
    LodGroupTableCapacity = NextPowerOfTwo(std::max<DWORD>(64u, std::max<DWORD>(1u, particleCount) * 2u));
    LodGroupKeys = std::make_shared<GBuffer>(device, static_cast<UINT>(sizeof(DWORD)), LodGroupTableCapacity,
                                             L"Voxel LOD Group Keys",
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    LodGroupKeysUpload = std::make_shared<PEPEngine::Graphics::UploadBuffer>(
        device, LodGroupTableCapacity, static_cast<UINT>(sizeof(DWORD)), L"Voxel LOD Group Keys Upload");
    LodDrawArguments = std::make_shared<GBuffer>(device, static_cast<UINT>(sizeof(DWORD)), 4u,
                                                 L"Voxel LOD Indirect Draw Arguments",
                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    LodDrawArgumentsUpload = std::make_shared<PEPEngine::Graphics::UploadBuffer>(
        device, 4u, static_cast<UINT>(sizeof(DWORD)), L"Voxel LOD Indirect Draw Arguments Upload");
    LodStats = std::make_shared<GBuffer>(device, static_cast<UINT>(sizeof(DWORD)), 8u,
                                         L"Voxel LOD Stats",
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    LodStatsUpload = std::make_shared<PEPEngine::Graphics::UploadBuffer>(
        device, 8u, static_cast<UINT>(sizeof(DWORD)), L"Voxel LOD Stats Upload");
    LodStatsReadback = std::make_shared<PEPEngine::Graphics::ReadBackBuffer<DWORD>>(
        device, 8u, L"Voxel LOD Stats Readback");

#pragma warning(push)
#pragma warning(disable : 4267)
    ParticlesAlive = std::make_shared<CounteredStructBuffer<DWORD>>(device, particleCount,
                                                                    L"Voxel Alive Index Buffer");
    ParticlesDead = std::make_shared<CounteredStructBuffer<DWORD>>(device, particleCount,
                                                                   L"Voxel Dead Index Buffer");
    LodRenderItems = std::make_shared<CounteredStructBuffer<VoxelLodRenderItem>>(device, particleCount,
                                                                                 L"Voxel LOD Render Items");
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

void VoxelPartitionGpuResources::InitializeLodState(
    const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, const DWORD particleCount) const
{
    std::vector<DWORD> emptyGroupKeys(LodGroupTableCapacity, 0xffffffffu);
    auto queue = device->GetCommandQueue();
    auto initList = queue->GetCommandList();
    if (particleCount > 0)
    {
        LodGroupKeysUpload->CopyData(0, emptyGroupKeys.data(), sizeof(DWORD) * emptyGroupKeys.size());
        initList->TransitionBarrier(LodGroupKeys->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
        initList->FlushResourceBarriers();
        initList->CopyBufferRegion(*LodGroupKeys, 0, *LodGroupKeysUpload, 0,
                                   static_cast<UINT>(sizeof(DWORD) * emptyGroupKeys.size()), false);
    }
    LodRenderItems->SetCounterValue(initList, 0u);

    const DWORD initialArgs[4] = {0u, 1u, 0u, 0u};
    LodDrawArgumentsUpload->CopyData(0, initialArgs, sizeof(initialArgs));
    initList->TransitionBarrier(LodDrawArguments->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
    initList->FlushResourceBarriers();
    initList->CopyBufferRegion(*LodDrawArguments, 0, *LodDrawArgumentsUpload, 0,
                               static_cast<UINT>(sizeof(initialArgs)), false);

    const DWORD zeroStats[8] = {};
    LodStatsUpload->CopyData(0, zeroStats, sizeof(zeroStats));
    initList->TransitionBarrier(LodStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
    initList->FlushResourceBarriers();
    initList->CopyBufferRegion(*LodStats, 0, *LodStatsUpload, 0,
                               static_cast<UINT>(sizeof(zeroStats)), false);

    initList->TransitionBarrier(LodGroupKeys->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->TransitionBarrier(LodRenderItems->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->TransitionBarrier(LodDrawArguments->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->TransitionBarrier(LodStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->FlushResourceBarriers();
    queue->WaitForFenceValue(queue->ExecuteCommandList(initList));
}

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

    srvDesc.Buffer.NumElements = LodRenderItems->GetElementCount();
    srvDesc.Buffer.StructureByteStride = LodRenderItems->GetStride();
    LodRenderItems->CreateShaderResourceView(&srvDesc, &RenderDescriptors, 1);

    srvDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    srvDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    ParticlesPool->CreateShaderResourceView(&srvDesc, &LodBuildDescriptors, 0);

    srvDesc.Buffer.NumElements = ParticlesAlive->GetElementCount();
    srvDesc.Buffer.StructureByteStride = ParticlesAlive->GetStride();
    ParticlesAlive->CreateShaderResourceView(&srvDesc, &LodBuildDescriptors, 1);

    uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = LodRenderItems->GetElementCount();
    uavDesc.Buffer.StructureByteStride = LodRenderItems->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = LodRenderItems->GetBufferSize() - sizeof(DWORD);
    LodRenderItems->CreateUnorderedAccessView(&uavDesc, &LodBuildDescriptors, 2, LodRenderItems->GetD3D12Resource());

    D3D12_UNORDERED_ACCESS_VIEW_DESC rawUavDesc{};
    rawUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    rawUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rawUavDesc.Buffer.NumElements = LodDrawArguments->GetBufferSize() / sizeof(DWORD);
    rawUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    LodDrawArguments->CreateUnorderedAccessView(&rawUavDesc, &LodBuildDescriptors, 3);

    uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = LodStats->GetElementCount();
    uavDesc.Buffer.StructureByteStride = LodStats->GetStride();
    LodStats->CreateUnorderedAccessView(&uavDesc, &LodBuildDescriptors, 4);

    uavDesc.Buffer.NumElements = LodGroupKeys->GetElementCount();
    uavDesc.Buffer.StructureByteStride = LodGroupKeys->GetStride();
    LodGroupKeys->CreateUnorderedAccessView(&uavDesc, &LodBuildDescriptors, 5);
}

void VoxelPartitionGpuResources::ResizeInjectionScratch()
{
    NewParticles.resize(InjectionCapacity);
}
