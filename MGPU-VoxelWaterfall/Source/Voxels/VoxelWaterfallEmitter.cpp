#include "pch.h"
#include "Source/Voxels/VoxelWaterfallEmitter.h"

#include "Source/Voxels/VoxelParticleSpawner.h"

#include <algorithm>
#include <cmath>

#include "GameObject.h"
#include "Transform.h"

double VoxelWaterfallEmitter::CalculateGroupCount(const DWORD particleCount) const
{
    if (particleCount == 0)
        return 0;

    const auto numGroups = (particleCount + 1023) / 1024;
    return std::ceil(std::sqrt(static_cast<double>(numGroups)));
}

VoxelParticleData VoxelWaterfallEmitter::GenerateVoxelParticle(const DWORD index) const
{
    return VoxelParticleSpawner::Generate(index, parameters);
}

void VoxelWaterfallEmitter::PSOInitialize()
{
    auto vertexShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
    auto pixelShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", PixelShader, nullptr, "PS", "ps_5_1");
    auto geometryShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", GeometryShader, nullptr, "GS", "gs_5_1");
    vertexShader->LoadAndCompile();
    pixelShader->LoadAndCompile();
    geometryShader->LoadAndCompile();

    CD3DX12_DESCRIPTOR_RANGE renderRanges[2];
    renderRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1);
    renderRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 1);

    renderSignature = std::make_shared<GRootSignature>();
    renderSignature->AddConstantBufferParameter(0);
    renderSignature->AddConstantBufferParameter(1);
    renderSignature->AddConstantParameter(sizeof(VoxelEmitterData) / sizeof(DWORD), 0, 1);
    renderSignature->AddDescriptorParameter(&renderRanges[0], 1);
    renderSignature->AddDescriptorParameter(&renderRanges[1], 1);
    renderSignature->Initialize(device);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC renderDesc{};
    renderDesc.InputLayout = {nullptr, 0};
    renderDesc.pRootSignature = renderSignature->GetNativeSignature().Get();
    renderDesc.VS = vertexShader->GetShaderResource();
    renderDesc.PS = pixelShader->GetShaderResource();
    renderDesc.GS = geometryShader->GetShaderResource();
    renderDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    renderDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    renderDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    renderDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    renderDesc.SampleMask = UINT_MAX;
    renderDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    renderDesc.NumRenderTargets = 1;
    renderDesc.RTVFormats[0] = GetSRGBFormat(BackBufferFormat);
    renderDesc.SampleDesc.Count = 1;
    renderDesc.DSVFormat = DepthStencilFormat;

    renderPSO = std::make_shared<GraphicPSO>(RenderMode::Particle);
    renderPSO->SetPsoDesc(renderDesc);
    renderPSO->Initialize(device);

    CD3DX12_DESCRIPTOR_RANGE computeRanges[4];
    for (UINT i = 0; i < 4; ++i)
        computeRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, i);

    computeSignature = std::make_shared<GRootSignature>();
    computeSignature->AddConstantParameter(sizeof(VoxelEmitterData) / sizeof(DWORD), 0);
    for (auto& range : computeRanges)
        computeSignature->AddDescriptorParameter(&range, 1);
    computeSignature->Initialize(device, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    CompileComputeShaders();
    injectedPSO = std::make_shared<ComputePSO>();
    injectedPSO->SetRootSignature(*computeSignature);
    injectedPSO->SetShader(injectedShader.get());
    injectedPSO->Initialize(device);

    simulatedPSO = std::make_shared<ComputePSO>();
    simulatedPSO->SetRootSignature(*computeSignature);
    simulatedPSO->SetShader(simulatedShader.get());
    simulatedPSO->Initialize(device);
}

void VoxelWaterfallEmitter::DescriptorInitialize()
{
    gpuResources.AllocateDescriptors(device);
}

void VoxelWaterfallEmitter::BufferInitialize()
{
    gpuResources.EnsureObjectPositionBuffer(device);
    gpuResources.ResetParticleBuffers();

    gpuResources.InjectionCapacity = std::max<DWORD>(1, emitterData.ParticlesTotalCount / 16);
    emitterData.ParticleInjectCount = gpuResources.InjectionCapacity;
    emitterData.InjectedGroupCount = static_cast<DWORD>(CalculateGroupCount(gpuResources.InjectionCapacity));
    emitterData.ParticlesAliveCount = 0;
    nextSpawnIndex = 0;
    isWorked = false;

    gpuResources.CreateParticleBuffers(device, emitterData.ParticlesTotalCount);
    gpuResources.InitializeDeadParticleList(device, emitterData.ParticlesTotalCount);
    gpuResources.CreateParticleViews();
    gpuResources.ResizeInjectionScratch();
}

VoxelWaterfallEmitter::VoxelWaterfallEmitter(const std::shared_ptr<GDevice>& primeDevice, const DWORD particleCount,
                                             const VoxelSimulationParameters& initialParameters)
    : parameters(initialParameters)
{
    device = primeDevice;
    PSOInitialize();
    DescriptorInitialize();
    ApplySettings(particleCount, parameters);
}

void VoxelWaterfallEmitter::UpdateFromCrossAdapterBridge()
{
    Update();
}

void VoxelWaterfallEmitter::DrawFromCrossAdapterBridge(const std::shared_ptr<GCommandList>& cmdList)
{
    Draw(cmdList);
}

void VoxelWaterfallEmitter::ApplySettings(const UINT count, const VoxelSimulationParameters& newParameters)
{
    parameters = newParameters;
    parameters.VoxelSize = std::max(parameters.VoxelSize, 0.05f);
    parameters.SpawnHeight = std::max(parameters.SpawnHeight, parameters.FloorHeight + parameters.VoxelSize);
    parameters.WaterfallWidth = std::max(parameters.WaterfallWidth, parameters.VoxelSize);
    parameters.WaterfallDepth = std::max(parameters.WaterfallDepth, parameters.VoxelSize);

    emitterData.Color = Vector4(0.02f, 0.48f, 0.95f, 1.0f);
    emitterData.Force = Vector3(0.0f, -std::abs(parameters.Gravity), 0.0f);
    emitterData.DeltaTime = 1.0f / 60.0f;
    emitterData.VoxelSize = parameters.VoxelSize;
    emitterData.SpawnHeight = parameters.SpawnHeight;
    emitterData.FloorHeight = parameters.FloorHeight;
    emitterData.WaterfallWidth = parameters.WaterfallWidth;
    emitterData.WaterfallDepth = parameters.WaterfallDepth;
    emitterData.InitialFallSpeed = std::abs(parameters.InitialFallSpeed);
    emitterData.Seed = parameters.Seed;
    emitterData.ParticlesTotalCount = std::max<UINT>(1, count);
    emitterData.SimulatedGroupCount = static_cast<DWORD>(CalculateGroupCount(emitterData.ParticlesTotalCount));
    BufferInitialize();
}

void VoxelWaterfallEmitter::ChangeParticleCount(const UINT count)
{
    ApplySettings(count, parameters);
}

const VoxelSimulationParameters& VoxelWaterfallEmitter::GetParameters() const
{
    return parameters;
}

UINT VoxelWaterfallEmitter::GetParticleCount() const
{
    return emitterData.ParticlesTotalCount;
}

VoxelEmitterData& VoxelWaterfallEmitter::GetEmitterData()
{
    return emitterData;
}

const VoxelEmitterData& VoxelWaterfallEmitter::GetEmitterData() const
{
    return emitterData;
}

GBuffer& VoxelWaterfallEmitter::GetParticlesPool() const
{
    return *gpuResources.ParticlesPool;
}

CounteredStructBuffer<DWORD>& VoxelWaterfallEmitter::GetParticlesAlive() const
{
    return *gpuResources.ParticlesAlive;
}

CounteredStructBuffer<DWORD>& VoxelWaterfallEmitter::GetParticlesDead() const
{
    return *gpuResources.ParticlesDead;
}

bool VoxelWaterfallEmitter::HasStartedSimulation() const
{
    return isWorked;
}

VoxelParticleData VoxelWaterfallEmitter::GenerateParticleForIndex(const DWORD index) const
{
    return GenerateVoxelParticle(index);
}

DWORD VoxelWaterfallEmitter::ConsumeNextSpawnIndex(const DWORD count)
{
    const DWORD firstIndex = nextSpawnIndex;
    nextSpawnIndex += count;
    return firstIndex;
}

double VoxelWaterfallEmitter::CalculateDispatchGroupCount(const DWORD particleCount) const
{
    return CalculateGroupCount(particleCount);
}

void VoxelWaterfallEmitter::SetLastDispatchVoxelCount(const UINT count)
{
    lastDispatchVoxelCount = count;
}

void VoxelWaterfallEmitter::SetEnabled(const bool value)
{
    enabled = value;
}

bool VoxelWaterfallEmitter::IsEnabled() const
{
    return enabled;
}

void VoxelWaterfallEmitter::SetUpdateInterval(const uint32_t value)
{
    UpdateInterval = std::max<uint32_t>(1, value);
}

uint32_t VoxelWaterfallEmitter::GetUpdateInterval() const
{
    return UpdateInterval;
}

void VoxelWaterfallEmitter::SetLastSimulationFrame(const uint64_t value)
{
    LastSimulationFrame = value;
}

uint64_t VoxelWaterfallEmitter::GetLastSimulationFrame() const
{
    return LastSimulationFrame;
}

void VoxelWaterfallEmitter::SetSimulationDeltaTime(const float value)
{
    emitterData.DeltaTime = value;
}

UINT VoxelWaterfallEmitter::GetLastDispatchVoxelCount() const
{
    return lastDispatchVoxelCount;
}

void VoxelWaterfallEmitter::Update()
{
    const auto transform = gameObject->GetTransform();
    if (transform->IsDirty())
    {
        objectWorldData.TextureTransform = transform->TextureTransform.Transpose();
        objectWorldData.World = transform->GetWorldMatrix().Transpose();
        gpuResources.ObjectPositionBuffer->CopyData(0, objectWorldData);
    }
}

void VoxelWaterfallEmitter::Draw(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->FlushResourceBarriers();

    cmdList->SetGraphicsRootSignature(*renderSignature);
    cmdList->SetPipelineState(*renderPSO);
    cmdList->SetDescriptorsHeap(&gpuResources.RenderDescriptors);
    cmdList->SetGraphicsRootConstantBufferView(ParticleRenderSlot::ObjectData, *gpuResources.ObjectPositionBuffer);
    cmdList->SetGraphicsRoot32BitConstants(ParticleRenderSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                           &emitterData, 0);
    cmdList->SetGraphicsRootDescriptorTable(ParticleRenderSlot::ParticlesPool, &gpuResources.RenderDescriptors, 0);
    cmdList->SetGraphicsRootDescriptorTable(ParticleRenderSlot::ParticlesAliveIndex, &gpuResources.RenderDescriptors, 1);
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmdList->SetIBuffer();
    cmdList->SetVBuffer();
    cmdList->Draw(emitterData.ParticlesAliveCount);

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
}

void VoxelWaterfallEmitter::Dispatch(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    isWorked = true;
    gpuResources.ParticlesAlive->ReadCounter(&emitterData.ParticlesAliveCount);
    emitterData.ParticlesAliveCount = std::min(emitterData.ParticlesAliveCount, emitterData.ParticlesTotalCount);
    lastDispatchVoxelCount = emitterData.ParticlesAliveCount;

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->SetComputeRootSignature(*computeSignature);
    cmdList->SetDescriptorsHeap(&gpuResources.ComputeDescriptors);

    const DWORD remaining = emitterData.ParticlesTotalCount - emitterData.ParticlesAliveCount;
    const DWORD spawnCount = std::min(gpuResources.InjectionCapacity, remaining);
    if (spawnCount > 0)
    {
        emitterData.ParticleInjectCount = spawnCount;
        emitterData.InjectedGroupCount = static_cast<DWORD>(CalculateGroupCount(spawnCount));
        for (DWORD i = 0; i < spawnCount; ++i)
            gpuResources.NewParticles[i] = GenerateVoxelParticle(nextSpawnIndex + i);
        nextSpawnIndex += spawnCount;

        gpuResources.InjectedParticles->LoadData(gpuResources.NewParticles.data(), cmdList);
        cmdList->TransitionBarrier(gpuResources.InjectedParticles->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();
        cmdList->SetPipelineState(*injectedPSO);
        cmdList->SetComputeRoot32BitConstants(ParticleComputeSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                              &emitterData, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &gpuResources.ComputeDescriptors, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleDead, &gpuResources.ComputeDescriptors, 1);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &gpuResources.ComputeDescriptors, 2);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &gpuResources.ComputeDescriptors, 3);
        cmdList->Dispatch(emitterData.InjectedGroupCount, emitterData.InjectedGroupCount, 1);
        cmdList->UAVBarrier(gpuResources.ParticlesPool->GetD3D12Resource());
        cmdList->UAVBarrier(gpuResources.ParticlesAlive->GetD3D12Resource());
        cmdList->FlushResourceBarriers();
    }

    if (emitterData.ParticlesAliveCount > 0)
    {
        emitterData.SimulatedGroupCount = static_cast<DWORD>(CalculateGroupCount(emitterData.ParticlesAliveCount));
        cmdList->SetPipelineState(*simulatedPSO);
        cmdList->SetComputeRoot32BitConstants(ParticleComputeSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                              &emitterData, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &gpuResources.ComputeDescriptors, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleDead, &gpuResources.ComputeDescriptors, 1);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &gpuResources.ComputeDescriptors, 2);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &gpuResources.ComputeDescriptors, 3);
        cmdList->Dispatch(emitterData.SimulatedGroupCount, emitterData.SimulatedGroupCount, 1);
    }

    gpuResources.ParticlesAlive->CopyCounterForRead(cmdList);
    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
}
