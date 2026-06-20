#include "pch.h"
#include "VoxelWaterfallEmitter.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "GameObject.h"
#include "Transform.h"

namespace
{
    DWORD HashVoxel(const DWORD value)
    {
        DWORD x = value;
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    float HashUnitFloat(const DWORD value)
    {
        return static_cast<float>(HashVoxel(value) & 0x00ffffffu) / static_cast<float>(0x01000000u);
    }
}

double VoxelWaterfallEmitter::CalculateGroupCount(const DWORD particleCount) const
{
    if (particleCount == 0)
        return 0;

    const auto numGroups = (particleCount + 1023) / 1024;
    return std::ceil(std::sqrt(static_cast<double>(numGroups)));
}

VoxelParticleData VoxelWaterfallEmitter::GenerateVoxelParticle(const DWORD index) const
{
    const float voxelSize = std::max(parameters.VoxelSize, 0.05f);
    const DWORD widthCells = std::max<DWORD>(1, static_cast<DWORD>(std::floor(parameters.WaterfallWidth / voxelSize)));
    const DWORD depthCells = std::max<DWORD>(1, static_cast<DWORD>(std::floor(parameters.WaterfallDepth / voxelSize)));
    const DWORD horizontalCells = widthCells * depthCells;
    const DWORD verticalCells = std::max<DWORD>(
        1, static_cast<DWORD>(std::floor((parameters.SpawnHeight - parameters.FloorHeight) / voxelSize)));

    const DWORD laneCount = std::max<DWORD>(
        1, std::min<DWORD>(horizontalCells, std::max<DWORD>(3, (horizontalCells * 3) / 4)));
    const DWORD laneIndex = index % laneCount;
    const DWORD laneHash = HashVoxel(laneIndex ^ parameters.Seed);
    const DWORD xIndex = laneHash % widthCells;
    const DWORD zIndex = HashVoxel(laneHash + parameters.Seed * 17u) % depthCells;
    const DWORD yPhase = HashVoxel(index + parameters.Seed * 31u) % verticalCells;
    const DWORD yIndex = ((index / laneCount) + yPhase) % verticalCells;

    const float x = (static_cast<float>(xIndex) - 0.5f * static_cast<float>(widthCells - 1)) * voxelSize;
    const float y = parameters.SpawnHeight - static_cast<float>(yIndex) * voxelSize;
    const float z = (static_cast<float>(zIndex) - 0.5f * static_cast<float>(depthCells - 1)) * voxelSize;
    const float speedVariation = 0.75f + 0.5f * HashUnitFloat(index ^ parameters.Seed ^ 0x9e3779b9u);

    VoxelParticleData particle{};
    particle.Position = Vector3(x, y, z);
    particle.ContinuousPosition = particle.Position;
    particle.Velocity = Vector3(0.0f, -parameters.InitialFallSpeed * speedVariation, 0.0f);
    particle.VoxelIndex = index;
    return particle;
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
    particlesComputeDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
    particlesRenderDescriptors = device->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
}

void VoxelWaterfallEmitter::BufferInitialize()
{
    if (!objectPositionBuffer)
        objectPositionBuffer = std::make_shared<ConstantUploadBuffer<ObjectConstants>>(device, 1, L"Voxel Emitter Position");

    ParticlesPool.reset();
    InjectedParticles.reset();
    ParticlesAlive.reset();
    ParticlesDead.reset();

    injectionCapacity = std::max<DWORD>(1, emitterData.ParticlesTotalCount / 16);
    emitterData.ParticleInjectCount = injectionCapacity;
    emitterData.InjectedGroupCount = static_cast<DWORD>(CalculateGroupCount(injectionCapacity));
    emitterData.ParticlesAliveCount = 0;
    nextSpawnIndex = 0;
    isWorked = false;

    ParticlesPool = std::make_shared<GBuffer>(device, sizeof(VoxelParticleData), emitterData.ParticlesTotalCount,
                                              L"Voxel Pool Buffer", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    InjectedParticles = std::make_shared<GBuffer>(device, sizeof(VoxelParticleData), injectionCapacity,
                                                  L"Injected Voxel Buffer", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ParticlesAlive = std::make_shared<CounteredStructBuffer<DWORD>>(device, emitterData.ParticlesTotalCount,
                                                                    L"Voxel Alive Index Buffer");
    ParticlesDead = std::make_shared<CounteredStructBuffer<DWORD>>(device, emitterData.ParticlesTotalCount,
                                                                   L"Voxel Dead Index Buffer");

    std::vector<UINT> deadIndices(emitterData.ParticlesTotalCount);
    std::iota(deadIndices.begin(), deadIndices.end(), 0u);
    auto queue = device->GetCommandQueue();
    auto initList = queue->GetCommandList();
    ParticlesDead->LoadData(deadIndices.data(), initList);
    ParticlesDead->SetCounterValue(initList, emitterData.ParticlesTotalCount);
    initList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    initList->FlushResourceBarriers();
    queue->WaitForFenceValue(queue->ExecuteCommandList(initList));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    ParticlesPool->CreateUnorderedAccessView(&uavDesc, &particlesComputeDescriptors, 0);

    uavDesc.Buffer.NumElements = ParticlesDead->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesDead->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = ParticlesDead->GetBufferSize() - sizeof(DWORD);
    ParticlesDead->CreateUnorderedAccessView(&uavDesc, &particlesComputeDescriptors, 1, ParticlesDead->GetD3D12Resource());
    ParticlesAlive->CreateUnorderedAccessView(&uavDesc, &particlesComputeDescriptors, 2, ParticlesAlive->GetD3D12Resource());

    uavDesc.Buffer.NumElements = InjectedParticles->GetElementCount();
    uavDesc.Buffer.StructureByteStride = InjectedParticles->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    InjectedParticles->CreateUnorderedAccessView(&uavDesc, &particlesComputeDescriptors, 3);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    srvDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    ParticlesPool->CreateShaderResourceView(&srvDesc, &particlesRenderDescriptors, 0);

    srvDesc.Buffer.NumElements = ParticlesAlive->GetElementCount();
    srvDesc.Buffer.StructureByteStride = ParticlesAlive->GetStride();
    ParticlesAlive->CreateShaderResourceView(&srvDesc, &particlesRenderDescriptors, 1);

    newParticles.resize(injectionCapacity);
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
        objectPositionBuffer->CopyData(0, objectWorldData);
    }
}

void VoxelWaterfallEmitter::Draw(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    cmdList->TransitionBarrier(ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->FlushResourceBarriers();

    cmdList->SetGraphicsRootSignature(*renderSignature);
    cmdList->SetPipelineState(*renderPSO);
    cmdList->SetDescriptorsHeap(&particlesRenderDescriptors);
    cmdList->SetGraphicsRootConstantBufferView(ParticleRenderSlot::ObjectData, *objectPositionBuffer);
    cmdList->SetGraphicsRoot32BitConstants(ParticleRenderSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                           &emitterData, 0);
    cmdList->SetGraphicsRootDescriptorTable(ParticleRenderSlot::ParticlesPool, &particlesRenderDescriptors, 0);
    cmdList->SetGraphicsRootDescriptorTable(ParticleRenderSlot::ParticlesAliveIndex, &particlesRenderDescriptors, 1);
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmdList->SetIBuffer();
    cmdList->SetVBuffer();
    cmdList->Draw(emitterData.ParticlesAliveCount);

    cmdList->TransitionBarrier(ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
}

void VoxelWaterfallEmitter::Dispatch(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!enabled)
        return;

    isWorked = true;
    ParticlesAlive->ReadCounter(&emitterData.ParticlesAliveCount);
    emitterData.ParticlesAliveCount = std::min(emitterData.ParticlesAliveCount, emitterData.ParticlesTotalCount);
    lastDispatchVoxelCount = emitterData.ParticlesAliveCount;

    cmdList->TransitionBarrier(ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->SetComputeRootSignature(*computeSignature);
    cmdList->SetDescriptorsHeap(&particlesComputeDescriptors);

    const DWORD remaining = emitterData.ParticlesTotalCount - emitterData.ParticlesAliveCount;
    const DWORD spawnCount = std::min(injectionCapacity, remaining);
    if (spawnCount > 0)
    {
        emitterData.ParticleInjectCount = spawnCount;
        emitterData.InjectedGroupCount = static_cast<DWORD>(CalculateGroupCount(spawnCount));
        for (DWORD i = 0; i < spawnCount; ++i)
            newParticles[i] = GenerateVoxelParticle(nextSpawnIndex + i);
        nextSpawnIndex += spawnCount;

        InjectedParticles->LoadData(newParticles.data(), cmdList);
        cmdList->TransitionBarrier(InjectedParticles->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();
        cmdList->SetPipelineState(*injectedPSO);
        cmdList->SetComputeRoot32BitConstants(ParticleComputeSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                              &emitterData, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &particlesComputeDescriptors, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleDead, &particlesComputeDescriptors, 1);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &particlesComputeDescriptors, 2);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &particlesComputeDescriptors, 3);
        cmdList->Dispatch(emitterData.InjectedGroupCount, emitterData.InjectedGroupCount, 1);
        cmdList->UAVBarrier(ParticlesPool->GetD3D12Resource());
        cmdList->UAVBarrier(ParticlesAlive->GetD3D12Resource());
        cmdList->FlushResourceBarriers();
    }

    if (emitterData.ParticlesAliveCount > 0)
    {
        emitterData.SimulatedGroupCount = static_cast<DWORD>(CalculateGroupCount(emitterData.ParticlesAliveCount));
        cmdList->SetPipelineState(*simulatedPSO);
        cmdList->SetComputeRoot32BitConstants(ParticleComputeSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                              &emitterData, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &particlesComputeDescriptors, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleDead, &particlesComputeDescriptors, 1);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &particlesComputeDescriptors, 2);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &particlesComputeDescriptors, 3);
        cmdList->Dispatch(emitterData.SimulatedGroupCount, emitterData.SimulatedGroupCount, 1);
    }

    ParticlesAlive->CopyCounterForRead(cmdList);
    cmdList->TransitionBarrier(ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
}
