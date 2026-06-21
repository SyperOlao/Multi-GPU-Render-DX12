#include "Source/Rendering/VoxelRenderPasses.h"

#include "FrameResource.h"
#include "GCommandList.h"
#include "GDescriptor.h"
#include "GRootSignature.h"
#include "GTexture.h"
#include "Renderer.h"
#include "RenderModeFactory.h"
#include "ShadowMap.h"
#include "SSAA.h"
#include "SSAO.h"
#include "Source/Voxels/VoxelGpuPartition.h"

using namespace DirectX;
using namespace PEPEngine::Graphics;

namespace
{
    constexpr float BenchmarkBackgroundColor[4] = {0.03f, 0.035f, 0.04f, 1.0f};

    bool HasRenderers(const VoxelRenderPassContext& context, const RenderMode mode)
    {
        return !context.TypedRenderers[static_cast<int>(mode)].empty();
    }
}

void VoxelRenderPasses::RecordPrimaryBase(const std::shared_ptr<GCommandList>& cmdList,
                                          const VoxelRenderPassContext& context) const
{
    RecordNormalMap(cmdList, context);
    RecordAmbientMap(cmdList, context);
    if (context.DynamicShadowsEnabled)
        RecordShadowMap(cmdList, context);
    RecordForwardPath(cmdList, context);
}

void VoxelRenderPasses::RecordFinalPresent(const std::shared_ptr<GCommandList>& cmdList,
                                           const VoxelRenderPassContext& context) const
{
    RecordBackBufferInit(cmdList, context);
    RecordFullQuad(cmdList, context);
}

void VoxelRenderPasses::RecordShadowMap(const std::shared_ptr<GCommandList>& cmdList,
                                        const VoxelRenderPassContext& context)
{
    if (!HasRenderers(context, RenderMode::Opaque) && !HasRenderers(context, RenderMode::OpaqueAlphaDrop))
        return;

    cmdList->SetRootSignature(*context.PrimeDeviceSignature.get());
    cmdList->SetRootShaderResourceView(StandardShaderSlot::MaterialData,
                                       *context.CurrentFrameResource.MaterialBuffer, 1);
    cmdList->SetRootDescriptorTable(StandardShaderSlot::TexturesMap, &context.SrvTexturesMemory);
    cmdList->SetRootConstantBufferView(StandardShaderSlot::CameraData,
                                       *context.CurrentFrameResource.PrimePassConstantUploadBuffer, 1);

    context.ShadowPath.PopulatePreRenderCommands(cmdList);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::ShadowMapOpaque));
    RecordDraw(cmdList, context, RenderMode::Opaque);
    RecordDraw(cmdList, context, RenderMode::OpaqueAlphaDrop);

    cmdList->TransitionBarrier(context.ShadowPath.GetTexture(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->FlushResourceBarriers();
}

void VoxelRenderPasses::RecordNormalMap(const std::shared_ptr<GCommandList>& cmdList,
                                        const VoxelRenderPassContext& context)
{
    cmdList->SetDescriptorsHeap(&context.SrvTexturesMemory);
    cmdList->SetRootSignature(*context.PrimeDeviceSignature.get());
    cmdList->SetRootShaderResourceView(StandardShaderSlot::MaterialData,
                                       *context.CurrentFrameResource.MaterialBuffer);
    cmdList->SetRootDescriptorTable(StandardShaderSlot::TexturesMap, &context.SrvTexturesMemory);

    cmdList->SetViewports(&context.FullViewport, 1);
    cmdList->SetScissorRects(&context.FullRect, 1);

    const auto normalMap = context.AmbientPath.NormalMap();
    const auto normalDepthMap = context.AmbientPath.NormalDepthMap();
    const auto normalMapRtv = context.AmbientPath.NormalMapRtv();
    const auto normalMapDsv = context.AmbientPath.NormalMapDSV();

    cmdList->TransitionBarrier(normalMap, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->TransitionBarrier(normalDepthMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->FlushResourceBarriers();
    float clearValue[] = {0.0f, 0.0f, 1.0f, 0.0f};
    cmdList->ClearRenderTarget(normalMapRtv, 0, clearValue);
    cmdList->ClearDepthStencil(normalMapDsv, 0,
                               D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0);

    cmdList->SetRenderTargets(1, normalMapRtv, 0, normalMapDsv);
    cmdList->SetRootConstantBufferView(1, *context.CurrentFrameResource.PrimePassConstantUploadBuffer);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::DrawNormalsOpaque));
    RecordDraw(cmdList, context, RenderMode::Opaque);
    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::DrawNormalsOpaqueDrop));
    RecordDraw(cmdList, context, RenderMode::OpaqueAlphaDrop);

    cmdList->TransitionBarrier(normalMap, D3D12_RESOURCE_STATE_COMMON);
    cmdList->TransitionBarrier(normalDepthMap, D3D12_RESOURCE_STATE_COMMON);
    cmdList->FlushResourceBarriers();
}

void VoxelRenderPasses::RecordAmbientMap(const std::shared_ptr<GCommandList>& cmdList,
                                         const VoxelRenderPassContext& context)
{
    cmdList->SetDescriptorsHeap(&context.SrvTexturesMemory);
    cmdList->SetRootSignature(*context.PrimeDeviceSignature.get());
    cmdList->SetRootShaderResourceView(StandardShaderSlot::MaterialData,
                                       *context.CurrentFrameResource.MaterialBuffer);
    cmdList->SetRootDescriptorTable(StandardShaderSlot::TexturesMap, &context.SrvTexturesMemory);

    cmdList->SetRootSignature(*context.SsaoRootSignature.get());
    context.AmbientPath.ComputeSsao(cmdList, context.CurrentFrameResource.SsaoConstantUploadBuffer, 3);
}

void VoxelRenderPasses::RecordForwardPath(const std::shared_ptr<GCommandList>& cmdList,
                                          const VoxelRenderPassContext& context)
{
    cmdList->SetDescriptorsHeap(&context.SrvTexturesMemory);
    cmdList->SetRootSignature(*context.PrimeDeviceSignature.get());
    cmdList->SetRootShaderResourceView(StandardShaderSlot::MaterialData,
                                       *context.CurrentFrameResource.MaterialBuffer);
    cmdList->SetRootDescriptorTable(StandardShaderSlot::TexturesMap, &context.SrvTexturesMemory);

    cmdList->SetViewports(&context.AntiAliasingPath.GetViewPort(), 1);
    cmdList->SetScissorRects(&context.AntiAliasingPath.GetRect(), 1);

    cmdList->TransitionBarrier(context.AntiAliasingPath.GetRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->TransitionBarrier(context.AntiAliasingPath.GetDepthMap(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->FlushResourceBarriers();

    cmdList->ClearRenderTarget(context.AntiAliasingPath.GetRTV(), 0, BenchmarkBackgroundColor);
    cmdList->ClearDepthStencil(context.AntiAliasingPath.GetDSV(), 0,
                               D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0);

    cmdList->SetRenderTargets(1, context.AntiAliasingPath.GetRTV(), 0,
                              context.AntiAliasingPath.GetDSV());

    cmdList->SetRootConstantBufferView(StandardShaderSlot::CameraData,
                                      *context.CurrentFrameResource.PrimePassConstantUploadBuffer);

    cmdList->SetRootDescriptorTable(StandardShaderSlot::ShadowMap, context.ShadowPath.GetSrv());
    cmdList->SetRootDescriptorTable(StandardShaderSlot::AmbientMap, context.AmbientPath.AmbientMapSrv(), 0);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::SkyBox));
    RecordDraw(cmdList, context, RenderMode::SkyBox);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::Opaque));
    RecordDraw(cmdList, context, RenderMode::Opaque);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::OpaqueAlphaDrop));
    RecordDraw(cmdList, context, RenderMode::OpaqueAlphaDrop);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::Transparent));
    RecordDraw(cmdList, context, RenderMode::Transparent);

    RecordPrimaryVoxelPartitions(cmdList, context);

    cmdList->TransitionBarrier(context.AntiAliasingPath.GetRenderTarget(),
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(context.AntiAliasingPath.GetDepthMap(), D3D12_RESOURCE_STATE_DEPTH_READ);
    cmdList->FlushResourceBarriers();
}

void VoxelRenderPasses::RecordPrimaryVoxelPartitions(
    const std::shared_ptr<GCommandList>& cmdList,
    const VoxelRenderPassContext& context)
{
    if (!context.PrimaryVoxelPartitions)
        return;

    for (const auto* partition : *context.PrimaryVoxelPartitions)
    {
        if (!partition || !partition->GpuPartition || partition->VoxelCount() == 0)
            continue;

        partition->GpuPartition->UpdateFrameConstants();
        auto result = partition->GpuPartition->RecordRender(
            cmdList,
            VoxelPartitionRenderOutputMode::PrimaryColor,
            context.CurrentFrameResource.PrimePassConstantUploadBuffer.get(),
            context.BenchmarkProfiler,
            VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
            VoxelBenchmarkProfiler::RangeId::PrimaryLodCompaction);
        result.PartitionId = partition->PartitionId;
        result.LogicalVoxelCount = partition->VoxelCount();
        if (context.PrimaryVoxelRenderResults)
            context.PrimaryVoxelRenderResults->push_back(result);
    }
}

void VoxelRenderPasses::RecordBackBufferInit(const std::shared_ptr<GCommandList>& cmdList,
                                             const VoxelRenderPassContext& context)
{
    cmdList->SetViewports(&context.FullViewport, 1);
    cmdList->SetScissorRects(&context.FullRect, 1);

    cmdList->TransitionBarrier(context.BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->FlushResourceBarriers();
    cmdList->ClearRenderTarget(&context.CurrentFrameResource.BackBufferRTVMemory, 0, Colors::Black);

    cmdList->SetRenderTargets(1, &context.CurrentFrameResource.BackBufferRTVMemory, 0);
}

void VoxelRenderPasses::RecordFullQuad(const std::shared_ptr<GCommandList>& cmdList,
                                       const VoxelRenderPassContext& context)
{
    cmdList->SetRootSignature(*context.PrimeDeviceSignature.get());
    auto* resolveSource = context.ResolveSourceSrv != nullptr
                              ? context.ResolveSourceSrv
                              : context.AntiAliasingPath.GetSRV();
    const auto resolveSourceOffset = context.ResolveSourceSrv != nullptr
                                     ? context.ResolveSourceSrvOffset
                                     : 0u;
    cmdList->SetDescriptorsHeap(resolveSource);

    cmdList->SetRootDescriptorTable(StandardShaderSlot::AmbientMap, resolveSource, resolveSourceOffset);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::Quad));
    RecordDraw(cmdList, context, RenderMode::Quad);
}

void VoxelRenderPasses::RecordDraw(const std::shared_ptr<GCommandList>& cmdList,
                                   const VoxelRenderPassContext& context,
                                   const RenderMode mode)
{
    for (auto&& renderer : context.TypedRenderers[static_cast<int>(mode)])
        renderer->Draw(cmdList);
}
