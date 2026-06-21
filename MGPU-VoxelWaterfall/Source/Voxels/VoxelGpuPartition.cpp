#include "pch.h"
#include "Source/Voxels/VoxelGpuPartition.h"

#include "GDescriptorHeap.h"
#include "Source/Voxels/VoxelParticleSpawner.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <utility>

#include "GameObject.h"
#include "Transform.h"

namespace
{
    std::vector<DWORD> BuildSequentialVoxelIds(const DWORD count)
    {
        std::vector<DWORD> ids(count);
        for (DWORD i = 0; i < ids.size(); ++i)
            ids[i] = i;
        return ids;
    }

    bool SameAdapterLuid(const LUID& left, const LUID& right)
    {
        return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    void ReportOwnershipFailure(const std::wstring& message)
    {
        OutputDebugStringW((message + L"\n").c_str());
        assert(false && "VoxelGpuPartition ownership validation failed; see debug output");
    }
}

double VoxelGpuPartition::CalculateGroupCount(const DWORD particleCount) const
{
    if (particleCount == 0)
        return 0;

    const auto numGroups = (particleCount + 1023) / 1024;
    return std::ceil(std::sqrt(static_cast<double>(numGroups)));
}

VoxelParticleData VoxelGpuPartition::GenerateVoxelParticle(const DWORD index) const
{
    assert(!globalVoxelIds.empty());
    assert(index < globalVoxelIds.size());
    const DWORD clampedIndex = std::min<DWORD>(index, static_cast<DWORD>(globalVoxelIds.size() - 1));
    return VoxelParticleSpawner::Generate(globalVoxelIds[clampedIndex], parameters);
}

bool VoxelGpuPartition::HasVoxels() const
{
    return emitterData.ParticlesTotalCount > 0 && !globalVoxelIds.empty();
}

void VoxelGpuPartition::ValidateDescriptorOwnership(
    const GDescriptor& descriptor,
    const char* descriptorName,
    const char* operation) const
{
    if (descriptor.IsNull())
        return;

    const auto descriptorDevice = descriptor.GetDescriptorHeap()->GetDevice();
    if (!descriptorDevice || !gpuResources.Owner.IsValid ||
        !SameAdapterLuid(descriptorDevice->GetDesc().AdapterLuid, gpuResources.Owner.AdapterLuid))
    {
        std::wostringstream message;
        message << L"VoxelGpuPartition " << operation << L" descriptor ownership mismatch for "
            << descriptorName << L". Expected adapter " << gpuResources.Owner.DeviceName
            << L", actual adapter " << (descriptorDevice ? descriptorDevice->GetName() : L"null");
        ReportOwnershipFailure(message.str());
    }
}

void VoxelGpuPartition::ValidateBufferOwnership(
    const GBuffer* buffer,
    const char* bufferName,
    const char* operation) const
{
    if (!buffer)
        return;

    const auto bufferDevice = const_cast<GBuffer*>(buffer)->GetDevice();
    if (!bufferDevice || !gpuResources.Owner.IsValid ||
        !SameAdapterLuid(bufferDevice->GetDesc().AdapterLuid, gpuResources.Owner.AdapterLuid))
    {
        std::wostringstream message;
        message << L"VoxelGpuPartition " << operation << L" buffer ownership mismatch for "
            << bufferName << L". Expected adapter " << gpuResources.Owner.DeviceName
            << L", actual adapter " << (bufferDevice ? bufferDevice->GetName() : L"null");
        ReportOwnershipFailure(message.str());
    }
}

void VoxelGpuPartition::ValidateRootSignatureOwnership(
    const GRootSignature* rootSignature,
    const char* signatureName,
    const char* operation) const
{
    const auto signatureDevice = rootSignature ? rootSignature->GetDevice() : nullptr;
    if (!signatureDevice || !gpuResources.Owner.IsValid ||
        !SameAdapterLuid(signatureDevice->GetDesc().AdapterLuid, gpuResources.Owner.AdapterLuid))
    {
        std::wostringstream message;
        message << L"VoxelGpuPartition " << operation << L" root signature ownership mismatch for "
            << signatureName << L". Expected adapter " << gpuResources.Owner.DeviceName
            << L", actual adapter " << (signatureDevice ? signatureDevice->GetName() : L"null");
        ReportOwnershipFailure(message.str());
    }
}

void VoxelGpuPartition::ValidateGraphicsPsoOwnership(
    const GraphicPSO* pso,
    const char* psoName,
    const char* operation) const
{
    const auto psoDevice = pso ? pso->GetDevice() : nullptr;
    if (!psoDevice || !gpuResources.Owner.IsValid ||
        !SameAdapterLuid(psoDevice->GetDesc().AdapterLuid, gpuResources.Owner.AdapterLuid))
    {
        std::wostringstream message;
        message << L"VoxelGpuPartition " << operation << L" graphics PSO ownership mismatch for "
            << psoName << L". Expected adapter " << gpuResources.Owner.DeviceName
            << L", actual adapter " << (psoDevice ? psoDevice->GetName() : L"null");
        ReportOwnershipFailure(message.str());
    }
}

void VoxelGpuPartition::ValidateCommandListOwnership(
    const std::shared_ptr<GCommandList>& cmdList,
    const char* operation) const
{
    const auto cmdDevice = cmdList ? cmdList->GetDevice() : nullptr;
    if (!cmdDevice || !gpuResources.Owner.IsValid ||
        !SameAdapterLuid(cmdDevice->GetDesc().AdapterLuid, gpuResources.Owner.AdapterLuid))
    {
        std::wostringstream message;
        message << L"VoxelGpuPartition " << operation << L" command list ownership mismatch. Expected adapter "
            << gpuResources.Owner.DeviceName
            << L", actual adapter " << (cmdDevice ? cmdDevice->GetName() : L"null");
        ReportOwnershipFailure(message.str());
    }
}

void VoxelGpuPartition::ValidateGpuResourceOwnership(const char* operation) const
{
    ValidateRootSignatureOwnership(renderSignature.get(), "RenderRootSignature", operation);
    ValidateGraphicsPsoOwnership(renderPSO.get(), "PrimaryRenderPSO", operation);
    ValidateGraphicsPsoOwnership(secondaryRenderPSO.get(), "SecondaryRenderPSO", operation);
    ValidateDescriptorOwnership(gpuResources.ComputeDescriptors, "ComputeDescriptors", operation);
    ValidateDescriptorOwnership(gpuResources.RenderDescriptors, "RenderDescriptors", operation);
    ValidateBufferOwnership(gpuResources.ObjectPositionBuffer.get(), "ObjectPositionBuffer", operation);
    ValidateBufferOwnership(gpuResources.ParticlesPool.get(), "ParticlesPool", operation);
    ValidateBufferOwnership(gpuResources.ParticlesAlive.get(), "ParticlesAlive", operation);
    ValidateBufferOwnership(gpuResources.ParticlesDead.get(), "ParticlesDead", operation);
    ValidateBufferOwnership(gpuResources.InjectedParticles.get(), "InjectedParticles", operation);
    ValidateBufferOwnership(gpuResources.SimulationStats.get(), "SimulationStats", operation);
    ValidateBufferOwnership(gpuResources.SimulationStatsUpload.get(), "SimulationStatsUpload", operation);
    ValidateBufferOwnership(gpuResources.SimulationStatsReadback.get(), "SimulationStatsReadback", operation);
    ValidateDescriptorOwnership(gpuResources.LodBuildDescriptors, "LodBuildDescriptors", operation);
    ValidateBufferOwnership(gpuResources.LodRenderItems.get(), "LodRenderItems", operation);
    ValidateBufferOwnership(gpuResources.LodPreviousLevels.get(), "LodPreviousLevels", operation);
    ValidateBufferOwnership(gpuResources.LodDrawArguments.get(), "LodDrawArguments", operation);
    ValidateBufferOwnership(gpuResources.LodDrawArgumentsUpload.get(), "LodDrawArgumentsUpload", operation);
    ValidateBufferOwnership(gpuResources.LodStats.get(), "LodStats", operation);
    ValidateBufferOwnership(gpuResources.LodStatsUpload.get(), "LodStatsUpload", operation);
    ValidateBufferOwnership(gpuResources.LodStatsReadback.get(), "LodStatsReadback", operation);
}

void VoxelGpuPartition::CreatePipelineState()
{
    auto vertexShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", VertexShader, nullptr, "VS", "vs_5_1");
    auto pixelShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", PixelShader, nullptr, "PS", "ps_5_1");
    auto secondaryPixelShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", PixelShader, nullptr,
                                                          "PSSecondary", "ps_5_1");
    auto geometryShader = std::make_shared<GShader>(L"Shaders\\ParticleDraw.hlsl", GeometryShader, nullptr, "GS", "gs_5_1");
    vertexShader->LoadAndCompile();
    pixelShader->LoadAndCompile();
    secondaryPixelShader->LoadAndCompile();
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

    auto secondaryRenderDesc = renderDesc;
    secondaryRenderDesc.PS = secondaryPixelShader->GetShaderResource();
    secondaryRenderDesc.NumRenderTargets = 2;
    secondaryRenderDesc.RTVFormats[0] = GetSRGBFormat(BackBufferFormat);
    secondaryRenderDesc.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
    secondaryRenderPSO = std::make_shared<GraphicPSO>(RenderMode::Particle);
    secondaryRenderPSO->SetPsoDesc(secondaryRenderDesc);
    secondaryRenderPSO->Initialize(device);

    CD3DX12_DESCRIPTOR_RANGE computeRanges[5];
    for (UINT i = 0; i < 5; ++i)
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

    CD3DX12_DESCRIPTOR_RANGE lodRanges[6];
    lodRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    lodRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    lodRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    lodRanges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
    lodRanges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);
    lodRanges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);

    lodBuildSignature = std::make_shared<GRootSignature>();
    lodBuildSignature->AddConstantParameter(sizeof(VoxelLodBuildData) / sizeof(DWORD), 0);
    for (auto& range : lodRanges)
        lodBuildSignature->AddDescriptorParameter(&range, 1);
    lodBuildSignature->Initialize(device, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    lodBuildShader = std::make_shared<GShader>(L"Shaders\\BuildLodRenderList.hlsl", ComputeShader, nullptr,
                                               "CS", "cs_5_1");
    lodBuildShader->LoadAndCompile();
    lodBuildPSO = std::make_shared<ComputePSO>();
    lodBuildPSO->SetRootSignature(*lodBuildSignature);
    lodBuildPSO->SetShader(lodBuildShader.get());
    lodBuildPSO->Initialize(device);

    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &argumentDesc;
    ThrowIfFailed(device->GetDXDevice()->CreateCommandSignature(
        &signatureDesc, nullptr, IID_PPV_ARGS(&drawCommandSignature)));
}

void VoxelGpuPartition::CreateDescriptors()
{
    gpuResources.AllocateDescriptors(device);
}

void VoxelGpuPartition::CreateBuffers()
{
    gpuResources.ResetResources();

    gpuResources.InjectionCapacity = HasVoxels()
                                         ? std::max<DWORD>(1, emitterData.ParticlesTotalCount / 16)
                                         : 0;
    emitterData.ParticleInjectCount = gpuResources.InjectionCapacity;
    emitterData.InjectedGroupCount = static_cast<DWORD>(CalculateGroupCount(gpuResources.InjectionCapacity));
    emitterData.ParticlesAliveCount = 0;
    nextSpawnIndex = 0;
    lastDispatchVoxelCount = 0;
    lastRecycledVoxelCount = 0;
    lastAliveVoxelCount = 0;
    recordedAliveVoxelCount = 0;
    simulationStatsResetPending = true;
    isWorked = false;

    if (!HasVoxels())
        return;

    gpuResources.EnsureObjectPositionBuffer(device);
    gpuResources.CreateParticleBuffers(device, emitterData.ParticlesTotalCount);
    gpuResources.InitializeDeadParticleList(device, emitterData.ParticlesTotalCount);
    gpuResources.InitializeLodState(device, emitterData.ParticlesTotalCount);
    gpuResources.CreateParticleViews();
    gpuResources.ResizeInjectionScratch();
}

VoxelGpuPartition::VoxelGpuPartition(const std::shared_ptr<GDevice>& owningDevice, const DWORD particleCount,
                                     const VoxelSimulationParameters& initialParameters,
                                     const VoxelAdapterOwner owner)
    : parameters(initialParameters), adapterOwner(owner), globalVoxelIds(BuildSequentialVoxelIds(particleCount))
{
    device = owningDevice;
    gpuResources.SetOwnerDevice(device);
    Initialize();
    ApplySettings(globalVoxelIds, parameters);
}

VoxelGpuPartition::VoxelGpuPartition(const std::shared_ptr<GDevice>& owningDevice,
                                     std::vector<DWORD> voxelIds,
                                     const VoxelSimulationParameters& initialParameters,
                                     const VoxelAdapterOwner owner)
    : parameters(initialParameters), adapterOwner(owner), globalVoxelIds(std::move(voxelIds))
{
    device = owningDevice;
    gpuResources.SetOwnerDevice(device);
    Initialize();
    ApplySettings(globalVoxelIds, parameters);
}

void VoxelGpuPartition::Initialize()
{
    CreatePipelineState();
    CreateDescriptors();
}

void VoxelGpuPartition::ApplySettings(const UINT count, const VoxelSimulationParameters& newParameters)
{
    ApplySettings(BuildSequentialVoxelIds(static_cast<DWORD>(count)), newParameters);
}

void VoxelGpuPartition::ApplySettings(const std::vector<DWORD>& voxelIds,
                                      const VoxelSimulationParameters& newParameters)
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
    emitterData.SimulationTime = 0.0f;
    emitterData.InterpolationAlpha = 0.0f;
    emitterData.RecycleMargin = std::max(parameters.VoxelSize * 8.0f, 3.0f);
    emitterData.GridSnapEnabled = 0.0f;
    emitterData.SpatialLodDebugMode = static_cast<DWORD>(spatialLodSettings.DebugMode);
    emitterData.AdapterOwner = adapterOwner == VoxelAdapterOwner::Secondary ? 1u : 0u;
    globalVoxelIds = voxelIds;
    emitterData.ParticlesTotalCount = static_cast<DWORD>(globalVoxelIds.size());
    emitterData.SimulatedGroupCount = static_cast<DWORD>(CalculateGroupCount(emitterData.ParticlesTotalCount));
    CreateBuffers();
}

void VoxelGpuPartition::ChangeParticleCount(const UINT count)
{
    ApplySettings(count, parameters);
}

const VoxelSimulationParameters& VoxelGpuPartition::GetParameters() const
{
    return parameters;
}

UINT VoxelGpuPartition::GetParticleCount() const
{
    return static_cast<UINT>(globalVoxelIds.size());
}

VoxelEmitterData& VoxelGpuPartition::GetEmitterData()
{
    return emitterData;
}

const VoxelEmitterData& VoxelGpuPartition::GetEmitterData() const
{
    return emitterData;
}

bool VoxelGpuPartition::HasStartedSimulation() const
{
    return isWorked;
}

VoxelParticleData VoxelGpuPartition::GenerateParticleForIndex(const DWORD index) const
{
    return GenerateVoxelParticle(index);
}

DWORD VoxelGpuPartition::ConsumeNextSpawnIndex(const DWORD count)
{
    const DWORD firstIndex = nextSpawnIndex;
    nextSpawnIndex += count;
    return firstIndex;
}

double VoxelGpuPartition::CalculateDispatchGroupCount(const DWORD particleCount) const
{
    return CalculateGroupCount(particleCount);
}

void VoxelGpuPartition::SetLastDispatchVoxelCount(const UINT count)
{
    lastDispatchVoxelCount = count;
}

void VoxelGpuPartition::SetEnabled(const bool value)
{
    simulationEnabled = value;
    renderEnabled = value;
}

void VoxelGpuPartition::SetRenderEnabled(const bool value)
{
    renderEnabled = value;
}

void VoxelGpuPartition::SetSimulationEnabled(const bool value)
{
    simulationEnabled = value;
}

bool VoxelGpuPartition::IsEnabled() const
{
    return simulationEnabled || renderEnabled;
}

bool VoxelGpuPartition::IsRenderEnabled() const
{
    return renderEnabled;
}

bool VoxelGpuPartition::IsSimulationEnabled() const
{
    return simulationEnabled;
}

void VoxelGpuPartition::SetUpdateInterval(const uint32_t value)
{
    UpdateInterval = std::max<uint32_t>(1, value);
}

uint32_t VoxelGpuPartition::GetUpdateInterval() const
{
    return UpdateInterval;
}

void VoxelGpuPartition::SetLastSimulationFrame(const uint64_t value)
{
    LastSimulationFrame = value;
}

uint64_t VoxelGpuPartition::GetLastSimulationFrame() const
{
    return LastSimulationFrame;
}

void VoxelGpuPartition::SetSimulationDeltaTime(const float value)
{
    emitterData.DeltaTime = value;
}

void VoxelGpuPartition::SetSimulationTime(const float value)
{
    emitterData.SimulationTime = value;
}

void VoxelGpuPartition::SetInterpolationAlpha(const float value)
{
    emitterData.InterpolationAlpha = std::clamp(value, 0.0f, 1.0f);
}

void VoxelGpuPartition::ConfigureSpatialLod(
    const VoxelSpatialLodSettings& settings,
    const DirectX::SimpleMath::Vector3& cameraPosition)
{
    spatialLodSettings = settings;
    spatialLodSettings.Lod0Distance = std::max(1.0f, spatialLodSettings.Lod0Distance);
    spatialLodSettings.Lod1Distance = std::max(spatialLodSettings.Lod0Distance + 1.0f,
                                               spatialLodSettings.Lod1Distance);
    spatialLodSettings.Hysteresis = std::max(0.0f, spatialLodSettings.Hysteresis);
    spatialLodCameraPosition = cameraPosition;
}

void VoxelGpuPartition::BeginSimulationFrame()
{
    simulationStatsResetPending = true;
}

UINT VoxelGpuPartition::GetLastDispatchVoxelCount() const
{
    return lastDispatchVoxelCount;
}

UINT VoxelGpuPartition::GetLastRecycledVoxelCount() const
{
    return lastRecycledVoxelCount;
}

UINT VoxelGpuPartition::GetLastAliveVoxelCount() const
{
    return lastAliveVoxelCount;
}

UINT VoxelGpuPartition::GetExpectedVoxelCount() const
{
    return emitterData.ParticlesTotalCount;
}

VoxelPartitionStatistics VoxelGpuPartition::GetStatistics() const
{
    return {
        lastDispatchVoxelCount,
        lastRecycledVoxelCount,
        lastAliveVoxelCount,
        emitterData.ParticlesTotalCount
    };
}

VoxelSpatialLodStats VoxelGpuPartition::GetLastLodStats() const
{
    return lastLodStats;
}

std::shared_ptr<GDevice> VoxelGpuPartition::GetOwningDevice() const
{
    return device;
}

VoxelAdapterOwner VoxelGpuPartition::GetAdapterOwner() const
{
    return adapterOwner;
}

const std::vector<DWORD>& VoxelGpuPartition::GetGlobalVoxelIds() const
{
    return globalVoxelIds;
}

void VoxelGpuPartition::Update()
{
    UpdateFrameConstants();
}

void VoxelGpuPartition::UpdateFrameConstants()
{
    if (!gpuResources.ObjectPositionBuffer)
        return;

    const auto transform = gameObject->GetTransform();
    cachedObjectPosition = transform->GetWorldPosition();
    if (transform->IsDirty())
    {
        objectWorldData.TextureTransform = transform->TextureTransform.Transpose();
        objectWorldData.World = transform->GetWorldMatrix().Transpose();
        gpuResources.ObjectPositionBuffer->CopyData(0, objectWorldData);
    }
}

void VoxelGpuPartition::Draw(const std::shared_ptr<GCommandList>& cmdList)
{
    RecordRender(cmdList);
}

void VoxelGpuPartition::UpdateLodStatsReadback()
{
    if (!gpuResources.LodStatsReadback)
        return;

    DWORD lod0 = 0;
    DWORD lod1 = 0;
    DWORD lod2 = 0;
    DWORD aggregated = 0;
    gpuResources.LodStatsReadback->ReadData(0, lod0);
    gpuResources.LodStatsReadback->ReadData(1, lod1);
    gpuResources.LodStatsReadback->ReadData(2, lod2);
    gpuResources.LodStatsReadback->ReadData(3, aggregated);
    lastLodStats.Lod0Rendered = lod0;
    lastLodStats.Lod1Rendered = lod1;
    lastLodStats.Lod2Rendered = lod2;
    lastLodStats.Aggregated = aggregated;
}

void VoxelGpuPartition::BuildLodRenderList(const std::shared_ptr<GCommandList>& cmdList)
{
    ValidateCommandListOwnership(cmdList, "BuildLodRenderList");
    ValidateGpuResourceOwnership("BuildLodRenderList");

    UpdateLodStatsReadback();

    gpuResources.LodRenderItems->SetCounterValue(cmdList, 0u);
    const DWORD initialArgs[4] = {0u, 1u, 0u, 0u};
    gpuResources.LodDrawArgumentsUpload->CopyData(0, initialArgs, sizeof(initialArgs));
    cmdList->TransitionBarrier(gpuResources.LodDrawArguments->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->FlushResourceBarriers();
    cmdList->CopyBufferRegion(*gpuResources.LodDrawArguments, 0, *gpuResources.LodDrawArgumentsUpload, 0,
                              static_cast<UINT>(sizeof(initialArgs)), false);

    const DWORD zeroStats[4] = {};
    gpuResources.LodStatsUpload->CopyData(0, zeroStats, sizeof(zeroStats));
    cmdList->TransitionBarrier(gpuResources.LodStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->FlushResourceBarriers();
    cmdList->CopyBufferRegion(*gpuResources.LodStats, 0, *gpuResources.LodStatsUpload, 0,
                              static_cast<UINT>(sizeof(zeroStats)), false);

    VoxelLodBuildData lodData{};
    lodData.CameraPosition = spatialLodCameraPosition;
    lodData.ObjectPosition = cachedObjectPosition;
    lodData.Lod0Distance = spatialLodSettings.Lod0Distance;
    lodData.Lod1Distance = std::max(spatialLodSettings.Lod1Distance, spatialLodSettings.Lod0Distance + 1.0f);
    lodData.Hysteresis = spatialLodSettings.Hysteresis;
    lodData.VoxelSize = parameters.VoxelSize;
    lodData.SpawnHeight = parameters.SpawnHeight;
    lodData.FloorHeight = parameters.FloorHeight;
    lodData.WaterfallWidth = parameters.WaterfallWidth;
    lodData.WaterfallDepth = parameters.WaterfallDepth;
    lodData.Seed = parameters.Seed;
    lodData.AliveCount = emitterData.ParticlesAliveCount;
    lodData.SpatialLodMode = static_cast<DWORD>(spatialLodSettings.Mode);
    lodData.AdapterOwner = adapterOwner == VoxelAdapterOwner::Secondary ? 1u : 0u;

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(gpuResources.LodRenderItems->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.LodDrawArguments->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.LodStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.LodPreviousLevels->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->FlushResourceBarriers();

    cmdList->SetComputeRootSignature(*lodBuildSignature);
    cmdList->SetPipelineState(*lodBuildPSO);
    cmdList->SetDescriptorsHeap(&gpuResources.LodBuildDescriptors);
    cmdList->SetComputeRoot32BitConstants(0, sizeof(VoxelLodBuildData) / sizeof(DWORD), &lodData, 0);
    for (UINT slot = 0; slot < 6; ++slot)
        cmdList->SetComputeRootDescriptorTable(slot + 1, &gpuResources.LodBuildDescriptors, slot);
    const uint32_t groups = (emitterData.ParticlesAliveCount + 255u) / 256u;
    if (groups > 0)
        cmdList->Dispatch(groups, 1, 1);
    cmdList->UAVBarrier(gpuResources.LodRenderItems->GetD3D12Resource());
    cmdList->UAVBarrier(gpuResources.LodDrawArguments->GetD3D12Resource());
    cmdList->UAVBarrier(gpuResources.LodStats->GetD3D12Resource());
    cmdList->FlushResourceBarriers();
    cmdList->CopyBufferRegion(*gpuResources.LodStatsReadback, 0, *gpuResources.LodStats, 0,
                              static_cast<UINT>(sizeof(DWORD) * 4u), true);
}

VoxelPartitionRenderResult VoxelGpuPartition::RecordRender(
    const std::shared_ptr<GCommandList>& cmdList,
    const VoxelPartitionRenderOutputMode outputMode,
    const GBuffer* passConstants,
    VoxelBenchmarkProfiler* benchmarkProfiler,
    const VoxelBenchmarkProfiler::QueueId profilerQueue,
    const VoxelBenchmarkProfiler::RangeId lodCompactionRange)
{
    VoxelPartitionRenderResult result{};
    result.OwnerAdapter = adapterOwner;
    result.OwnerAdapterName = gpuResources.Owner.DeviceName;
    const auto cmdDevice = cmdList ? cmdList->GetDevice() : nullptr;
    result.CommandListAdapterName = cmdDevice ? cmdDevice->GetName() : L"null";

    if (!renderEnabled || !HasVoxels() || emitterData.ParticlesAliveCount == 0)
        return result;

    ValidateCommandListOwnership(cmdList, "RecordRender");
    ValidateGpuResourceOwnership("RecordRender");
    if (passConstants)
        ValidateBufferOwnership(passConstants, "PassConstants", "RecordRender");

    if (benchmarkProfiler)
        benchmarkProfiler->BeginRange(cmdList, profilerQueue, lodCompactionRange);
    BuildLodRenderList(cmdList);
    if (benchmarkProfiler)
    {
        benchmarkProfiler->EndRange(cmdList, profilerQueue, lodCompactionRange);
        benchmarkProfiler->ResolveRange(cmdList, profilerQueue, lodCompactionRange);
    }
    emitterData.SpatialLodDebugMode = static_cast<DWORD>(spatialLodSettings.DebugMode);
    emitterData.AdapterOwner = adapterOwner == VoxelAdapterOwner::Secondary ? 1u : 0u;

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(gpuResources.LodRenderItems->GetD3D12Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->FlushResourceBarriers();

    cmdList->SetGraphicsRootSignature(*renderSignature);
    cmdList->SetPipelineState(outputMode == VoxelPartitionRenderOutputMode::SecondaryColorAndLinearDepth
                                  ? *secondaryRenderPSO
                                  : *renderPSO);
    cmdList->SetDescriptorsHeap(&gpuResources.RenderDescriptors);
    cmdList->SetGraphicsRootConstantBufferView(ParticleRenderSlot::ObjectData, *gpuResources.ObjectPositionBuffer);
    if (passConstants)
        cmdList->SetGraphicsRootConstantBufferView(ParticleRenderSlot::CameraData, *passConstants);
    cmdList->SetGraphicsRoot32BitConstants(ParticleRenderSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                           &emitterData, 0);
    cmdList->SetGraphicsRootDescriptorTable(ParticleRenderSlot::ParticlesPool, &gpuResources.RenderDescriptors, 0);
    cmdList->SetGraphicsRootDescriptorTable(ParticleRenderSlot::ParticlesAliveIndex, &gpuResources.RenderDescriptors, 1);
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmdList->SetIBuffer();
    cmdList->SetVBuffer();
    cmdList->ExecuteIndirect(drawCommandSignature.Get(), 1u, *gpuResources.LodDrawArguments);
    result.DrawIssued = true;
    result.DrawCallCount = 1;
    result.UsedIndirectDraw = true;
    result.LodStats = lastLodStats;
    result.SubmittedVoxelCount = lastLodStats.TotalRendered();

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.LodRenderItems->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.LodDrawArguments->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.LodStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.LodPreviousLevels->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
    return result;
}

void VoxelGpuPartition::Dispatch(const std::shared_ptr<GCommandList>& cmdList)
{
    DispatchSimulation(cmdList);
}

void VoxelGpuPartition::DispatchSimulation(const std::shared_ptr<GCommandList>& cmdList)
{
    if (!simulationEnabled || !HasVoxels())
    {
        lastDispatchVoxelCount = 0;
        lastRecycledVoxelCount = 0;
        lastAliveVoxelCount = 0;
        recordedAliveVoxelCount = 0;
        emitterData.ParticlesAliveCount = 0;
        return;
    }

    ValidateCommandListOwnership(cmdList, "RecordSimulation");
    ValidateGpuResourceOwnership("RecordSimulation");

    isWorked = true;
    DWORD readbackAliveCount = 0;
    gpuResources.ParticlesAlive->ReadCounter(&readbackAliveCount);
    gpuResources.SimulationStatsReadback->ReadData(0, lastRecycledVoxelCount);
    emitterData.ParticlesAliveCount = std::min(
        std::max(readbackAliveCount, recordedAliveVoxelCount), emitterData.ParticlesTotalCount);
    lastAliveVoxelCount = emitterData.ParticlesAliveCount;

    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->TransitionBarrier(gpuResources.ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->SetComputeRootSignature(*computeSignature);
    cmdList->SetDescriptorsHeap(&gpuResources.ComputeDescriptors);

    if (simulationStatsResetPending)
    {
        const DWORD zeroStats = 0;
        gpuResources.SimulationStatsUpload->CopyData(0, &zeroStats, sizeof(DWORD));
        cmdList->TransitionBarrier(gpuResources.SimulationStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->FlushResourceBarriers();
        cmdList->CopyBufferRegion(*gpuResources.SimulationStats, 0, *gpuResources.SimulationStatsUpload, 0,
                                  sizeof(DWORD), false);
        simulationStatsResetPending = false;
    }
    cmdList->TransitionBarrier(gpuResources.SimulationStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->FlushResourceBarriers();

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
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::Count, &gpuResources.ComputeDescriptors, 4);
        cmdList->Dispatch(emitterData.InjectedGroupCount, emitterData.InjectedGroupCount, 1);
        cmdList->UAVBarrier(gpuResources.ParticlesPool->GetD3D12Resource());
        cmdList->UAVBarrier(gpuResources.ParticlesAlive->GetD3D12Resource());
        cmdList->FlushResourceBarriers();
    }

    const DWORD simulatedCount = std::min(emitterData.ParticlesTotalCount, emitterData.ParticlesAliveCount + spawnCount);
    lastDispatchVoxelCount = simulatedCount;
    lastAliveVoxelCount = simulatedCount;
    recordedAliveVoxelCount = simulatedCount;

    if (simulatedCount > 0)
    {
        emitterData.ParticlesAliveCount = simulatedCount;
        emitterData.SimulatedGroupCount = static_cast<DWORD>(CalculateGroupCount(simulatedCount));
        cmdList->SetPipelineState(*simulatedPSO);
        cmdList->SetComputeRoot32BitConstants(ParticleComputeSlot::EmitterData, sizeof(VoxelEmitterData) / sizeof(DWORD),
                                              &emitterData, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticlesPool, &gpuResources.ComputeDescriptors, 0);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleDead, &gpuResources.ComputeDescriptors, 1);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleAlive, &gpuResources.ComputeDescriptors, 2);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::ParticleInjection, &gpuResources.ComputeDescriptors, 3);
        cmdList->SetComputeRootDescriptorTable(ParticleComputeSlot::Count, &gpuResources.ComputeDescriptors, 4);
        cmdList->Dispatch(emitterData.SimulatedGroupCount, emitterData.SimulatedGroupCount, 1);
    }

    gpuResources.ParticlesAlive->CopyCounterForRead(cmdList);
    cmdList->CopyBufferRegion(*gpuResources.SimulationStatsReadback, 0, *gpuResources.SimulationStats, 0,
                              sizeof(DWORD), true);
    cmdList->TransitionBarrier(gpuResources.ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(gpuResources.SimulationStats->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
}
