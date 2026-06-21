#include "Source/Rendering/VoxelCompositePass.h"

#include "GCommandList.h"
#include "GDescriptor.h"
#include "GDevice.h"
#include "GRootSignature.h"
#include "GraphicPSO.h"
#include "GShader.h"
#include "GTexture.h"
#include "ShaderBuffersData.h"
#include "d3dx12.h"

#include <cassert>

using namespace PEPEngine::Graphics;

namespace
{
    D3D12_SHADER_RESOURCE_VIEW_DESC Texture2DSrv(const DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Format = format;
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = 1;
        return desc;
    }

    bool IsR32DepthSrvCompatible(const DXGI_FORMAT resourceFormat)
    {
        return resourceFormat == DXGI_FORMAT_R32_TYPELESS ||
               resourceFormat == DXGI_FORMAT_R32_FLOAT;
    }

    void ValidateDepthSrvCompatibility(const VoxelCompositePassContext& context)
    {
        const auto primaryDepthFormat = context.PrimaryDepth.GetD3D12ResourceDesc().Format;
        const auto secondaryLinearDepthFormat =
            context.FrameTargets.PrimaryReceivedSecondaryLinearDepth.GetD3D12ResourceDesc().Format;
        assert(IsR32DepthSrvCompatible(primaryDepthFormat) &&
               "Primary depth must be R32_TYPELESS-compatible before creating an R32_FLOAT SRV");
        assert(secondaryLinearDepthFormat == DXGI_FORMAT_R32_FLOAT &&
               "Secondary depth composition input must already be linear R32_FLOAT depth");
        assert(context.NearZ > 0.0f && context.FarZ > context.NearZ &&
               "Depth linearization requires explicit non-reversed near/far plane convention");
    }
}

void VoxelCompositePass::Initialize(const std::shared_ptr<GDevice>& inDevice, const DXGI_FORMAT inOutputFormat)
{
    device = inDevice;
    outputFormat = inOutputFormat;

    auto vertexShader = std::make_shared<GShader>(L"Shaders\\VoxelComposite.hlsl", VertexShader,
                                                  nullptr, "VS", "vs_5_1");
    auto pixelShader = std::make_shared<GShader>(L"Shaders\\VoxelComposite.hlsl", PixelShader,
                                                 nullptr, "PS", "ps_5_1");
    vertexShader->LoadAndCompile();
    pixelShader->LoadAndCompile();

    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

    rootSignature = std::make_shared<GRootSignature>();
    rootSignature->AddConstantParameter(sizeof(Constants) / sizeof(uint32_t), 0);
    rootSignature->AddDescriptorParameter(&srvRange, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->Initialize(device);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.InputLayout = {nullptr, 0};
    desc.pRootSignature = rootSignature->GetNativeSignature().Get();
    desc.VS = vertexShader->GetShaderResource();
    desc.PS = pixelShader->GetShaderResource();
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthEnable = false;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = outputFormat;
    desc.SampleDesc.Count = 1;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    pipelineState = std::make_shared<GraphicPSO>(RenderMode::Debug);
    pipelineState->SetPsoDesc(desc);
    pipelineState->Initialize(device);
}

bool VoxelCompositePass::IsInitialized() const
{
    return rootSignature != nullptr && pipelineState != nullptr;
}

void VoxelCompositePass::RefreshDescriptors(const VoxelCompositePassContext& context) const
{
    ValidateDepthSrvCompatibility(context);
    auto& descriptors = context.FrameTargets.PrimaryCompositeDescriptors;

    auto primaryColorDesc = Texture2DSrv(context.PrimaryBaseColor.GetD3D12ResourceDesc().Format);
    context.PrimaryBaseColor.CreateShaderResourceView(&primaryColorDesc, &descriptors, 0);

    auto primaryDepthDesc = Texture2DSrv(DXGI_FORMAT_R32_FLOAT);
    context.PrimaryDepth.CreateShaderResourceView(&primaryDepthDesc, &descriptors, 1);

    auto secondaryColorDesc = Texture2DSrv(
        context.FrameTargets.PrimaryReceivedSecondaryColor.GetD3D12ResourceDesc().Format);
    context.FrameTargets.PrimaryReceivedSecondaryColor.CreateShaderResourceView(&secondaryColorDesc, &descriptors, 2);

    auto secondaryDepthDesc = Texture2DSrv(DXGI_FORMAT_R32_FLOAT);
    context.FrameTargets.PrimaryReceivedSecondaryLinearDepth.CreateShaderResourceView(
        &secondaryDepthDesc, &descriptors, 3);
}

void VoxelCompositePass::Record(const std::shared_ptr<GCommandList>& cmdList,
                                const VoxelCompositePassContext& context) const
{
    assert(IsInitialized());
    RefreshDescriptors(context);

    const auto colorDesc = context.PrimaryBaseColor.GetD3D12ResourceDesc();

    Constants constants{};
    constants.InvRenderTargetSize[0] = 1.0f / static_cast<float>(colorDesc.Width);
    constants.InvRenderTargetSize[1] = 1.0f / static_cast<float>(colorDesc.Height);
    constants.NearZ = context.NearZ;
    constants.FarZ = context.FarZ;
    constants.DebugView = static_cast<uint32_t>(context.DebugView);

    cmdList->SetViewports(&context.Viewport, 1);
    cmdList->SetScissorRects(&context.ScissorRect, 1);

    cmdList->TransitionBarrier(context.PrimaryBaseColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(context.PrimaryDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(context.FrameTargets.PrimaryReceivedSecondaryColor,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(context.FrameTargets.PrimaryReceivedSecondaryLinearDepth,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->TransitionBarrier(context.FrameTargets.PrimaryCompositeColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->FlushResourceBarriers();

    cmdList->SetRenderTargets(1, &context.FrameTargets.PrimaryCompositeRtvDescriptor, 0);
    cmdList->SetRootSignature(*rootSignature);
    cmdList->SetPipelineState(*pipelineState);
    cmdList->SetDescriptorsHeap(&context.FrameTargets.PrimaryCompositeDescriptors);
    cmdList->SetRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &constants, 0);
    cmdList->SetRootDescriptorTable(1, &context.FrameTargets.PrimaryCompositeDescriptors, 0);
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->Draw(3);

    cmdList->TransitionBarrier(context.FrameTargets.PrimaryCompositeColor,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->FlushResourceBarriers();
}
