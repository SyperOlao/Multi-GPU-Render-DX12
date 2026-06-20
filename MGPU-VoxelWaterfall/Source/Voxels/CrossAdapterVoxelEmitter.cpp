#include "pch.h"
#include "Source/Voxels/CrossAdapterVoxelEmitter.h"


#include "MathHelper.h"
#include "Source/Voxels/VoxelWaterfallEmitter.h"

void CrossAdapterVoxelEmitter::InitPSO(const std::shared_ptr<GDevice>& otherDevice)
{
    CD3DX12_DESCRIPTOR_RANGE rng[4];
    rng[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    rng[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
    rng[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);
    rng[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);

    computeRS = std::make_shared<GRootSignature>();
    computeRS->AddConstantParameter(sizeof(VoxelEmitterData) / sizeof(DWORD), 0); // VoxelEmitterData		
    computeRS->AddDescriptorParameter(&rng[0], 1);
    computeRS->AddDescriptorParameter(&rng[1], 1);
    computeRS->AddDescriptorParameter(&rng[2], 1);
    computeRS->AddDescriptorParameter(&rng[3], 1);
    computeRS->Initialize(otherDevice, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    injectPSO = std::make_shared<ComputePSO>();
    injectPSO->SetShader(injectedShader.get());
    injectPSO->SetRootSignature(*computeRS);
    injectPSO->Initialize(secondDevice);


    updatePSO = std::make_shared<ComputePSO>();
    updatePSO->SetShader(simulatedShader.get());
    updatePSO->SetRootSignature(*computeRS);
    updatePSO->Initialize(secondDevice);
}

#pragma warning(push)
#pragma warning(disable : 4267)
void CrossAdapterVoxelEmitter::CreateBuffers()
{
    if (particlesPool)
    {
        particlesPool->Reset();
        particlesPool.reset();
    }

    if (injectedParticles)
    {
        injectedParticles->Reset();
        injectedParticles.reset();
    }

    if (particlesAlive)
    {
        particlesAlive->Reset();
        particlesAlive.reset();
    }

    if (particlesDead)
    {
        particlesDead->Reset();
        particlesDead.reset();
    }

    if (CrossAdapterAliveIndexes)
    {
        CrossAdapterAliveIndexes->Reset();
        CrossAdapterAliveIndexes.reset();
    }

    if (CrossAdapterDeadIndexes)
    {
        CrossAdapterDeadIndexes->Reset();
        CrossAdapterDeadIndexes.reset();
    }

    if (CrossAdapterParticles)
    {
        CrossAdapterParticles->Reset();
        CrossAdapterParticles.reset();
    }

    auto& emitterData = primeVoxelWaterfallEmitter->GetEmitterData();

    const auto particleStride = static_cast<UINT>(sizeof(VoxelParticleData));
    particlesPool = std::make_shared<GBuffer>(secondDevice, particleStride,
                                              emitterData.ParticlesTotalCount,
                                              L"Second Particles Pool Buffer",
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    injectedParticles = std::make_shared<GBuffer>(secondDevice, particleStride,
                                                  emitterData.ParticleInjectCount,
                                                  L"Second Injected Particle Buffer",
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    particlesAlive = std::make_shared<CounteredStructBuffer<DWORD>>(secondDevice,
                                                                    emitterData.ParticlesTotalCount,
                                                                    L"Second Particles Alive Index Buffer");
    particlesDead = std::make_shared<CounteredStructBuffer<DWORD>>(secondDevice,
                                                                   emitterData.ParticlesTotalCount,
                                                                   L"Second Particles Dead Index Buffer");

    auto desc = particlesAlive->GetD3D12ResourceDesc();
    CrossAdapterAliveIndexes = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                       L"Cross Adapter Particle Alive Index Buffer");
    CrossAdapterDeadIndexes = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                      L"Cross Adapter Particle Dead Index Buffer");

    desc = particlesPool->GetD3D12ResourceDesc();
    CrossAdapterParticles = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                    L"Cross Adapter Particle Buffer");


    newParticles.resize(injectedParticles->GetElementCount());

    {
        std::vector<UINT> deadIndex;

        for (DWORD i = 0; i < emitterData.ParticlesTotalCount; ++i)
        {
            deadIndex.push_back(i);
        }

        auto queue = secondDevice->GetCommandQueue();
        auto cmdList = queue->GetCommandList();

        particlesDead->LoadData(deadIndex.data(), cmdList);
        particlesDead->SetCounterValue(cmdList, emitterData.ParticlesTotalCount);

        cmdList->TransitionBarrier(particlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
        cmdList->FlushResourceBarriers();

        queue->ExecuteCommandList(cmdList);
        queue->Flush();
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = particlesPool->GetElementCount();
    uavDesc.Buffer.StructureByteStride = particlesPool->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;

    particlesPool->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 0);

    uavDesc.Buffer.NumElements = particlesDead->GetElementCount();
    uavDesc.Buffer.StructureByteStride = particlesDead->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = particlesDead->GetBufferSize() - sizeof(DWORD);
    particlesDead->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 1, particlesDead->GetD3D12Resource());
    particlesAlive->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 2, particlesAlive->GetD3D12Resource());

    uavDesc.Buffer.NumElements = injectedParticles->GetElementCount();
    uavDesc.Buffer.StructureByteStride = injectedParticles->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    injectedParticles->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 3);
}
#pragma warning(pop)

CrossAdapterVoxelEmitter::CrossAdapterVoxelEmitter(std::shared_ptr<GDevice> primeDevice,
                                                   const std::shared_ptr<GDevice>& otherDevice,
                                                   DWORD particleCount,
                                                   const VoxelSimulationParameters& initialParameters)
    : secondDevice(otherDevice)
{
    this->device = primeDevice;


    CompileComputeShaders();

    InitPSO(otherDevice);


    updateDescriptors = secondDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);

    primeVoxelWaterfallEmitter = std::make_shared<VoxelWaterfallEmitter>(primeDevice, particleCount, initialParameters);


    CreateBuffers();
}

void CrossAdapterVoxelEmitter::Update()
{
    primeVoxelWaterfallEmitter->gameObject = this->gameObject;
    primeVoxelWaterfallEmitter->UpdateFromCrossAdapterBridge();
}

void CrossAdapterVoxelEmitter::Draw(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    primeVoxelWaterfallEmitter->DrawFromCrossAdapterBridge(cmdList);
}

void CrossAdapterVoxelEmitter::Dispatch(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    auto& emitterData = primeVoxelWaterfallEmitter->GetEmitterData();

    if (pendingSharedStateChange == Enable)
    {
        if (primeVoxelWaterfallEmitter->HasStartedSimulation())
        {
            primeVoxelWaterfallEmitter->GetParticlesAlive().ReadCounter(&emitterData.ParticlesAliveCount);

            cmdList->CopyResource(particlesPool->GetD3D12Resource(),
                                  CrossAdapterParticles->GetSharedResource().GetD3D12Resource());
            cmdList->CopyResource(particlesAlive->GetD3D12Resource(),
                                  CrossAdapterAliveIndexes->GetSharedResource().GetD3D12Resource());
            cmdList->CopyResource(particlesDead->GetD3D12Resource(),
                                  CrossAdapterDeadIndexes->GetSharedResource().GetD3D12Resource());
        }

        pendingSharedStateChange = None;
    }

    if (pendingSharedStateChange == Disable)
    {
        cmdList->CopyResource(primeVoxelWaterfallEmitter->GetParticlesPool().GetD3D12Resource(),
                              CrossAdapterParticles->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeVoxelWaterfallEmitter->GetParticlesAlive().GetD3D12Resource(),
                              CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeVoxelWaterfallEmitter->GetParticlesDead().GetD3D12Resource(),
                              CrossAdapterDeadIndexes->GetPrimeResource().GetD3D12Resource());

        pendingSharedStateChange = None;
    }


    if (useSharedCompute)
    {
        particlesAlive->ReadCounter(&emitterData.ParticlesAliveCount);
        emitterData.ParticlesAliveCount = std::min(emitterData.ParticlesAliveCount,
                                                   emitterData.ParticlesTotalCount);
        primeVoxelWaterfallEmitter->SetLastDispatchVoxelCount(emitterData.ParticlesAliveCount);

        cmdList->TransitionBarrier(particlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(particlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(particlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();

        cmdList->SetComputeRootSignature(*computeRS);
        cmdList->SetDescriptorsHeap(&updateDescriptors);

        cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &updateDescriptors, 0);
        cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticleDead, &updateDescriptors, 1);
        cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &updateDescriptors, 2);

        if (emitterData.ParticlesTotalCount > emitterData.ParticlesAliveCount)
        {
            const DWORD availableSlots = emitterData.ParticlesTotalCount - emitterData.ParticlesAliveCount;

            if (availableSlots >= emitterData.ParticleInjectCount)
            {
                const DWORD firstSpawnIndex = primeVoxelWaterfallEmitter->ConsumeNextSpawnIndex(
                    emitterData.ParticleInjectCount);
                for (DWORD i = 0; i < emitterData.ParticleInjectCount; ++i)
                {
                    newParticles[i] = primeVoxelWaterfallEmitter->GenerateParticleForIndex(firstSpawnIndex + i);
                }

                injectedParticles->LoadData(newParticles.data(), cmdList);
                cmdList->TransitionBarrier(injectedParticles->GetD3D12Resource(),
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmdList->FlushResourceBarriers();

                cmdList->SetPipelineState(*injectPSO.get());

                cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &updateDescriptors, 3);

                cmdList->SetRoot32BitConstants(ParticleComputeSlot::EmitterData,
                                               sizeof(VoxelEmitterData) / sizeof(DWORD),
                                               &emitterData, 0);

                cmdList->Dispatch(emitterData.InjectedGroupCount, emitterData.InjectedGroupCount, 1);

                cmdList->UAVBarrier(injectedParticles->GetD3D12Resource());
                cmdList->FlushResourceBarriers();
            }
        }

        if (emitterData.ParticlesAliveCount > 0)
        {
            emitterData.SimulatedGroupCount = static_cast<DWORD>(
                primeVoxelWaterfallEmitter->CalculateDispatchGroupCount(emitterData.ParticlesAliveCount));

            cmdList->SetRoot32BitConstants(ParticleComputeSlot::EmitterData,
                                           sizeof(VoxelEmitterData) / sizeof(DWORD),
                                           &emitterData, 0);

            cmdList->SetPipelineState(*updatePSO.get());

            cmdList->Dispatch(emitterData.SimulatedGroupCount, emitterData.SimulatedGroupCount, 1);
        }

        particlesAlive->CopyCounterForRead(cmdList);

        cmdList->CopyResource(CrossAdapterParticles->GetSharedResource().GetD3D12Resource(),
                              particlesPool->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterAliveIndexes->GetSharedResource().GetD3D12Resource(),
                              particlesAlive->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterDeadIndexes->GetSharedResource().GetD3D12Resource(),
                              particlesDead->GetD3D12Resource());
    }
    else
    {
        primeVoxelWaterfallEmitter->Dispatch(cmdList);

        cmdList->CopyResource(CrossAdapterParticles->GetPrimeResource().GetD3D12Resource(),
                              primeVoxelWaterfallEmitter->GetParticlesPool().GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource(),
                              primeVoxelWaterfallEmitter->GetParticlesAlive().GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterDeadIndexes->GetPrimeResource().GetD3D12Resource(),
                              primeVoxelWaterfallEmitter->GetParticlesDead().GetD3D12Resource());
    }
}

void CrossAdapterVoxelEmitter::ChangeParticleCount(const UINT count)
{
    ApplySettings(count, primeVoxelWaterfallEmitter->GetParameters());
}

void CrossAdapterVoxelEmitter::ApplySettings(const UINT count, const VoxelSimulationParameters& newParameters)
{
    primeVoxelWaterfallEmitter->ApplySettings(count, newParameters);

    CreateBuffers();
}

UINT CrossAdapterVoxelEmitter::GetParticleCount() const
{
    return primeVoxelWaterfallEmitter->GetParticleCount();
}

const VoxelSimulationParameters& CrossAdapterVoxelEmitter::GetParameters() const
{
    return primeVoxelWaterfallEmitter->GetParameters();
}

void CrossAdapterVoxelEmitter::SetEnabled(const bool value)
{
    enabled = value;
    primeVoxelWaterfallEmitter->SetEnabled(value);
}

bool CrossAdapterVoxelEmitter::IsEnabled() const
{
    return enabled;
}

void CrossAdapterVoxelEmitter::SetUpdateInterval(const uint32_t value)
{
    UpdateInterval = std::max<uint32_t>(1, value);
    primeVoxelWaterfallEmitter->SetUpdateInterval(UpdateInterval);
}

uint32_t CrossAdapterVoxelEmitter::GetUpdateInterval() const
{
    return UpdateInterval;
}

void CrossAdapterVoxelEmitter::SetLastSimulationFrame(const uint64_t value)
{
    LastSimulationFrame = value;
    primeVoxelWaterfallEmitter->SetLastSimulationFrame(value);
}

uint64_t CrossAdapterVoxelEmitter::GetLastSimulationFrame() const
{
    return LastSimulationFrame;
}

void CrossAdapterVoxelEmitter::SetSimulationDeltaTime(const float value)
{
    primeVoxelWaterfallEmitter->SetSimulationDeltaTime(value);
}

UINT CrossAdapterVoxelEmitter::GetLastDispatchVoxelCount() const
{
    return primeVoxelWaterfallEmitter->GetLastDispatchVoxelCount();
}

bool CrossAdapterVoxelEmitter::IsSharedComputeEnabled() const
{
    return useSharedCompute;
}

void CrossAdapterVoxelEmitter::CopySharedToPrimary(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled || !useSharedCompute)
        return;

    auto& emitterData = primeVoxelWaterfallEmitter->GetEmitterData();

    cmdList->CopyResource(primeVoxelWaterfallEmitter->GetParticlesPool().GetD3D12Resource(),
                          CrossAdapterParticles->GetPrimeResource().GetD3D12Resource());
    cmdList->CopyResource(primeVoxelWaterfallEmitter->GetParticlesAlive().GetD3D12Resource(),
                          CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource());
    primeVoxelWaterfallEmitter->GetParticlesAlive().ReadCounter(&emitterData.ParticlesAliveCount);
}

void CrossAdapterVoxelEmitter::EnableShared()
{
    if (useSharedCompute)
        return;

    primeVoxelWaterfallEmitter->GetParticlesAlive().ReadCounter(
        &primeVoxelWaterfallEmitter->GetEmitterData().ParticlesAliveCount);

    useSharedCompute = true;

    pendingSharedStateChange = Enable;
}

void CrossAdapterVoxelEmitter::DisableShared()
{
    if (!useSharedCompute)
        return;

    useSharedCompute = false;

    pendingSharedStateChange = Disable;
}
