#include "Source/Rendering/VoxelRenderPasses.h"

#include "FrameResource.h"
#include "GCommandList.h"
#include "GDescriptor.h"
#include "GDescriptorHeap.h"
#include "GDevice.h"
#include "GRootSignature.h"
#include "GTexture.h"
#include "Renderer.h"
#include "RenderModeFactory.h"
#include "ShadowMap.h"
#include "SSAA.h"
#include "SSAO.h"
#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"
#include "Source/Voxels/VoxelGpuPartition.h"

#include <array>
#include <cassert>
#include <vector>

using namespace DirectX;
using namespace PEPEngine::Graphics;

namespace
{
    bool HasRenderers(const VoxelRenderPassContext& context, const RenderMode mode)
    {
        return !context.TypedRenderers[static_cast<int>(mode)].empty();
    }

    void ValidateFinalResolveSrv(const VoxelRenderPassContext& context,
                                 const GDescriptor& descriptor,
                                 const UINT descriptorOffset)
    {
        static_assert(StandardShaderSlot::AmbientMap - StandardShaderSlot::SkyMap == 2,
                      "fullscreen pixel shader samples ssaoMap at t2");

        if (context.ResolveSource == FinalResolveSource::PrimaryBase)
            return;

        assert(context.ResolveSourceTexture != nullptr &&
               "final resolve SRV validation requires the selected source resource");
        assert(context.ResolveSourceMetadata != nullptr &&
               "final resolve SRV validation requires descriptor metadata");

        const auto& metadata = *context.ResolveSourceMetadata;
        const auto sourceResource = context.ResolveSourceTexture->GetD3D12Resource();
        const auto sourceDesc = context.ResolveSourceTexture->GetD3D12ResourceDesc();
        const auto descriptorHeap = descriptor.GetDescriptorHeap();

        assert(sourceResource.Get() != nullptr && "final resolve source resource must be valid");
        assert(metadata.ResourceAddress == reinterpret_cast<uint64_t>(sourceResource.Get()) &&
               "final resolve descriptor metadata resource address must match selected source resource");
        assert(metadata.ResourceGeneration == context.CurrentFrameResource.RenderTargetGeneration &&
               "final resolve descriptor resource generation must match the frame render target generation");
        // Final-resolve SRVs are immutable per render-target generation. The frame descriptor
        // generation also covers unrelated shader-visible heaps such as material and ImGui heaps.
        assert(metadata.DescriptorGeneration == metadata.ResourceGeneration &&
               "final resolve descriptor generation must match its source resource generation");
        assert(metadata.FrameResourceIndex == context.FrameResourceIndex &&
               "final resolve descriptor frame index must match the current FrameResource");
        assert(metadata.Format == sourceDesc.Format &&
               "final resolve SRV format must match the selected source resource format");
        assert(metadata.Width == static_cast<UINT>(sourceDesc.Width) &&
               metadata.Height == sourceDesc.Height &&
               "final resolve SRV dimensions must match the selected source resource dimensions");
        assert(metadata.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
               sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               "final resolve SRV view dimension must match a Texture2D source resource");
        assert(!descriptor.IsNull() && descriptor.GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
               "final resolve SRV must come from a CBV/SRV/UAV descriptor heap");
        assert(descriptorHeap && descriptorHeap->GetDirectxHeap() != nullptr &&
               "final resolve SRV descriptor heap must be valid");
        assert(descriptorHeap->GetDevice() == context.PrimaryDevice &&
               "final resolve shader-visible heap must belong to the primary device");
        assert(descriptor.GetGPUHandle(descriptorOffset).ptr != 0 &&
               "final resolve root descriptor table requires a shader-visible GPU handle");
        assert(metadata.GpuHandle.ptr == descriptor.GetGPUHandle(descriptorOffset).ptr &&
               "final resolve root descriptor table must use the current descriptor GPU handle");
    }
}

void VoxelRenderPasses::RecordPrimaryBase(const std::shared_ptr<GCommandList>& cmdList,
                                          const VoxelRenderPassContext& context) const
{
    RecordNormalMap(cmdList, context);
    RecordAmbientMap(cmdList, context);
    if (context.DynamicShadowsEnabled)
    {
        RecordShadowMap(cmdList, context);
    }
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
    static constexpr std::array<float, 4> NormalMapClearValue = {0.0f, 0.0f, 1.0f, 0.0f};
    cmdList->ClearRenderTarget(normalMapRtv, 0, NormalMapClearValue.data());
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

    cmdList->ClearRenderTarget(context.AntiAliasingPath.GetRTV(), 0, context.BackgroundColor.data());
    cmdList->ClearDepthStencil(context.AntiAliasingPath.GetDSV(), 0,
                               D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0);

    cmdList->SetRenderTargets(1, context.AntiAliasingPath.GetRTV(), 0,
                              context.AntiAliasingPath.GetDSV());

    cmdList->SetRootConstantBufferView(StandardShaderSlot::CameraData,
                                      *context.CurrentFrameResource.PrimePassConstantUploadBuffer);

    cmdList->SetRootDescriptorTable(StandardShaderSlot::ShadowMap, context.ShadowPath.GetSrv());
    cmdList->SetRootDescriptorTable(StandardShaderSlot::AmbientMap, context.AmbientPath.AmbientMapSrv(), 0);

    if (HasRenderers(context, RenderMode::SkyBox))
    {
        cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::SkyBox));
        RecordDraw(cmdList, context, RenderMode::SkyBox);
    }

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

    for (const auto& partition : *context.PrimaryVoxelPartitions)
    {
        if (!partition.GpuPartition || partition.LogicalVoxelCount == 0)
            continue;

        partition.GpuPartition->UpdateFrameConstants();
        auto result = partition.GpuPartition->RecordRender(
            cmdList,
            VoxelPartitionRenderOutputMode::PrimaryColor,
            context.CurrentFrameResource.PrimePassConstantUploadBuffer.get(),
            context.BenchmarkProfiler,
            VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
            VoxelBenchmarkProfiler::RangeId::PrimaryLodCompaction);
        result.LayerId = partition.LayerId;
        result.PartitionId = partition.PartitionId;
        result.LogicalVoxelCount = partition.LogicalVoxelCount;
        result.SceneGeneration = partition.SceneGeneration;
        result.PartitionGeneration = partition.PartitionGeneration;
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
    if (context.ResolveSource == FinalResolveSource::SolidColor)
    {
        cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::FinalSolidColor));
        RecordDraw(cmdList, context, RenderMode::Quad);
        return;
    }

    auto* resolveSource = context.ResolveSourceSrv != nullptr
                              ? context.ResolveSourceSrv
                              : context.AntiAliasingPath.GetSRV();
    const auto resolveSourceOffset = context.ResolveSourceSrv != nullptr
                                     ? context.ResolveSourceSrvOffset
                                     : 0u;
    if (context.ResolveSourceTexture)
    {
        cmdList->TransitionBarrier(*context.ResolveSourceTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->FlushResourceBarriers();
    }
    cmdList->SetDescriptorsHeap(resolveSource);

    cmdList->SetRootDescriptorTable(StandardShaderSlot::AmbientMap, resolveSource, resolveSourceOffset);

    cmdList->SetPipelineState(*context.PipelineResources.GetPSO(RenderMode::Quad));
    ValidateFinalResolveSrv(context, *resolveSource, resolveSourceOffset);
    RecordDraw(cmdList, context, RenderMode::Quad);
}

void VoxelRenderPasses::RecordDraw(const std::shared_ptr<GCommandList>& cmdList,
                                   const VoxelRenderPassContext& context,
                                   const RenderMode mode)
{
    for (auto&& renderer : context.TypedRenderers[static_cast<int>(mode)])
        renderer->Draw(cmdList);
}
