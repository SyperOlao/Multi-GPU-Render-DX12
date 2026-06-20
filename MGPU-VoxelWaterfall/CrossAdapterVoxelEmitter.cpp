#include "pch.h"
#include "CrossAdapterVoxelEmitter.h"


#include "MathHelper.h"
#include "VoxelWaterfallEmitter.h"

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
    injectPSO->SetRootSignature(*computeRS.get());
    injectPSO->Initialize(secondDevice);


    updatePSO = std::make_shared<ComputePSO>();
    updatePSO->SetShader(simulatedShader.get());
    updatePSO->SetRootSignature(*computeRS.get());
    updatePSO->Initialize(secondDevice);
}

void CrossAdapterVoxelEmitter::CreateBuffers()
{
    if (ParticlesPool)
    {
        ParticlesPool->Reset();
        ParticlesPool.reset();
    }

    if (InjectedParticles)
    {
        InjectedParticles->Reset();
        InjectedParticles.reset();
    }

    if (ParticlesAlive)
    {
        ParticlesAlive->Reset();
        ParticlesAlive.reset();
    }

    if (ParticlesDead)
    {
        ParticlesDead->Reset();
        ParticlesDead.reset();
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

    ParticlesPool = std::make_shared<GBuffer>(secondDevice, sizeof(VoxelParticleData),
                                              primeVoxelWaterfallEmitter->emitterData.ParticlesTotalCount,
                                              L"Second Particles Pool Buffer",
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    InjectedParticles = std::make_shared<GBuffer>(secondDevice, sizeof(VoxelParticleData),
                                                  primeVoxelWaterfallEmitter->emitterData.ParticleInjectCount,
                                                  L"Second Injected Particle Buffer",
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ParticlesAlive = std::make_shared<CounteredStructBuffer<DWORD>>(secondDevice,
                                                                    primeVoxelWaterfallEmitter->emitterData.
                                                                    ParticlesTotalCount,
                                                                    L"Second Particles Alive Index Buffer");
    ParticlesDead = std::make_shared<CounteredStructBuffer<DWORD>>(secondDevice,
                                                                   primeVoxelWaterfallEmitter->emitterData.
                                                                   ParticlesTotalCount,
                                                                   L"Second Particles Dead Index Buffer");

    auto desc = ParticlesAlive->GetD3D12ResourceDesc();
    CrossAdapterAliveIndexes = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                       L"Cross Adapter Particle Alive Index Buffer");
    CrossAdapterDeadIndexes = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                      L"Cross Adapter Particle Dead Index Buffer");

    desc = ParticlesPool->GetD3D12ResourceDesc();
    CrossAdapterParticles = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                    L"Cross Adapter Particle Buffer");


    newParticles.resize(InjectedParticles->GetElementCount());

    {
        std::vector<UINT> deadIndex;

        for (int i = 0; i < primeVoxelWaterfallEmitter->emitterData.ParticlesTotalCount; ++i)
        {
            deadIndex.push_back(i);
        }

        auto queue = secondDevice->GetCommandQueue();
        auto cmdList = queue->GetCommandList();

        ParticlesDead->LoadData(deadIndex.data(), cmdList);
        ParticlesDead->SetCounterValue(cmdList, primeVoxelWaterfallEmitter->emitterData.ParticlesTotalCount);

        cmdList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
        cmdList->FlushResourceBarriers();

        queue->ExecuteCommandList(cmdList);
        queue->Flush();
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;

    ParticlesPool->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 0);

    uavDesc.Buffer.NumElements = ParticlesDead->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesDead->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = ParticlesDead->GetBufferSize() - sizeof(DWORD);
    ParticlesDead->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 1, ParticlesDead->GetD3D12Resource());
    ParticlesAlive->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 2, ParticlesAlive->GetD3D12Resource());

    uavDesc.Buffer.NumElements = InjectedParticles->GetElementCount();
    uavDesc.Buffer.StructureByteStride = InjectedParticles->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    InjectedParticles->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 3);
}

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
    primeVoxelWaterfallEmitter->Update();
}

void CrossAdapterVoxelEmitter::Draw(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    primeVoxelWaterfallEmitter->Draw(cmdList);
}

void CrossAdapterVoxelEmitter::Dispatch(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    if (DirtyActivated == Enable)
    {
        if (primeVoxelWaterfallEmitter->isWorked)
        {
            primeVoxelWaterfallEmitter->ParticlesAlive->ReadCounter(&primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount);

            cmdList->CopyResource(ParticlesPool->GetD3D12Resource(),
                                  CrossAdapterParticles->GetSharedResource().GetD3D12Resource());
            cmdList->CopyResource(ParticlesAlive->GetD3D12Resource(),
                                  CrossAdapterAliveIndexes->GetSharedResource().GetD3D12Resource());
            cmdList->CopyResource(ParticlesDead->GetD3D12Resource(),
                                  CrossAdapterDeadIndexes->GetSharedResource().GetD3D12Resource());
        }

        DirtyActivated = None;
    }

    if (DirtyActivated == Disable)
    {
        cmdList->CopyResource(primeVoxelWaterfallEmitter->ParticlesPool->GetD3D12Resource(),
                              CrossAdapterParticles->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeVoxelWaterfallEmitter->ParticlesAlive->GetD3D12Resource(),
                              CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeVoxelWaterfallEmitter->ParticlesDead->GetD3D12Resource(),
                              CrossAdapterDeadIndexes->GetPrimeResource().GetD3D12Resource());

        DirtyActivated = None;
    }


    if (UseSharedCompute)
    {
        ParticlesAlive->ReadCounter(&primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount);
        primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount = std::min(
            primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount,
            primeVoxelWaterfallEmitter->emitterData.ParticlesTotalCount);
        primeVoxelWaterfallEmitter->lastDispatchVoxelCount =
            primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount;

        cmdList->TransitionBarrier(ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();

        cmdList->SetComputeRootSignature(*computeRS);
        cmdList->SetDescriptorsHeap(&updateDescriptors);

        cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &updateDescriptors, 0);
        cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticleDead, &updateDescriptors, 1);
        cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &updateDescriptors, 2);

        if (primeVoxelWaterfallEmitter->emitterData.ParticlesTotalCount > primeVoxelWaterfallEmitter->emitterData.
            ParticlesAliveCount)
        {
            const long check = (primeVoxelWaterfallEmitter->emitterData.ParticlesTotalCount - primeVoxelWaterfallEmitter->
                emitterData.ParticlesAliveCount);

            if (check >= primeVoxelWaterfallEmitter->emitterData.ParticleInjectCount)
            {
                for (int i = 0; i < primeVoxelWaterfallEmitter->emitterData.ParticleInjectCount; ++i)
                {
                    newParticles[i] = primeVoxelWaterfallEmitter->GenerateVoxelParticle(
                        primeVoxelWaterfallEmitter->nextSpawnIndex + i);
                }
                primeVoxelWaterfallEmitter->nextSpawnIndex += primeVoxelWaterfallEmitter->emitterData.ParticleInjectCount;

                InjectedParticles->LoadData(newParticles.data(), cmdList);
                cmdList->TransitionBarrier(InjectedParticles->GetD3D12Resource(),
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmdList->FlushResourceBarriers();

                cmdList->SetPipelineState(*injectPSO.get());

                cmdList->SetRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &updateDescriptors, 3);

                cmdList->SetRoot32BitConstants(ParticleComputeSlot::EmitterData,
                                               sizeof(VoxelEmitterData) / sizeof(DWORD),
                                               &primeVoxelWaterfallEmitter->emitterData, 0);

                cmdList->Dispatch(primeVoxelWaterfallEmitter->emitterData.InjectedGroupCount,
                                  primeVoxelWaterfallEmitter->emitterData.InjectedGroupCount, 1);

                cmdList->UAVBarrier(InjectedParticles->GetD3D12Resource());
                cmdList->FlushResourceBarriers();
            }
        }

        if (primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount > 0)
        {
            primeVoxelWaterfallEmitter->emitterData.SimulatedGroupCount = primeVoxelWaterfallEmitter->CalculateGroupCount(
                primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount);

            cmdList->SetRoot32BitConstants(ParticleComputeSlot::EmitterData,
                                           sizeof(VoxelEmitterData) / sizeof(DWORD),
                                           &primeVoxelWaterfallEmitter->emitterData, 0);

            cmdList->SetPipelineState(*updatePSO.get());

            cmdList->Dispatch(primeVoxelWaterfallEmitter->emitterData.SimulatedGroupCount,
                              primeVoxelWaterfallEmitter->emitterData.SimulatedGroupCount, 1);
        }

        ParticlesAlive->CopyCounterForRead(cmdList);

        cmdList->CopyResource(CrossAdapterParticles->GetSharedResource().GetD3D12Resource(),
                              ParticlesPool->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterAliveIndexes->GetSharedResource().GetD3D12Resource(),
                              ParticlesAlive->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterDeadIndexes->GetSharedResource().GetD3D12Resource(),
                              ParticlesDead->GetD3D12Resource());
    }
    else
    {
        primeVoxelWaterfallEmitter->Dispatch(cmdList);

        cmdList->CopyResource(CrossAdapterParticles->GetPrimeResource().GetD3D12Resource(),
                              primeVoxelWaterfallEmitter->ParticlesPool->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource(),
                              primeVoxelWaterfallEmitter->ParticlesAlive->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterDeadIndexes->GetPrimeResource().GetD3D12Resource(),
                              primeVoxelWaterfallEmitter->ParticlesDead->GetD3D12Resource());
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
    return UseSharedCompute;
}

void CrossAdapterVoxelEmitter::CopySharedToPrimary(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled || !UseSharedCompute)
        return;

    cmdList->CopyResource(primeVoxelWaterfallEmitter->ParticlesPool->GetD3D12Resource(),
                          CrossAdapterParticles->GetPrimeResource().GetD3D12Resource());
    cmdList->CopyResource(primeVoxelWaterfallEmitter->ParticlesAlive->GetD3D12Resource(),
                          CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource());
    primeVoxelWaterfallEmitter->ParticlesAlive->ReadCounter(&primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount);
}

void CrossAdapterVoxelEmitter::EnableShared()
{
    if (UseSharedCompute)
        return;

    primeVoxelWaterfallEmitter->ParticlesAlive->ReadCounter(&primeVoxelWaterfallEmitter->emitterData.ParticlesAliveCount);

    UseSharedCompute = true;

    DirtyActivated = Enable;
}

void CrossAdapterVoxelEmitter::DisableShared()
{
    if (!UseSharedCompute)
        return;

    UseSharedCompute = false;

    DirtyActivated = Disable;
}
