#pragma once

#include "GraphicPSO.h"
#include "MemoryAllocator.h"

#include <d3d12.h>
#include <memory>

struct FrameResource;
class Renderer;
class RenderModeFactory;
class ShadowMap;
class SSAO;
class SSAA;

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GDescriptor;
    class GRootSignature;
    class GTexture;
}

struct VoxelRenderPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GRootSignature> PrimeDeviceSignature;
    std::shared_ptr<PEPEngine::Graphics::GRootSignature> SsaoRootSignature;
    PEPEngine::Graphics::GDescriptor& SrvTexturesMemory;
    FrameResource& CurrentFrameResource;
    D3D12_VIEWPORT FullViewport{};
    D3D12_RECT FullRect{};
    ShadowMap& ShadowPath;
    SSAO& AmbientPath;
    SSAA& AntiAliasingPath;
    RenderModeFactory& PipelineResources;
    PEPEngine::Allocator::custom_vector<PEPEngine::Allocator::custom_vector<std::shared_ptr<Renderer>>>& TypedRenderers;
    PEPEngine::Graphics::GTexture& BackBuffer;
    PEPEngine::Graphics::GDescriptor* ResolveSourceSrv = nullptr;
    UINT ResolveSourceSrvOffset = 0;
};

class VoxelRenderPasses
{
public:
    void RecordPrimaryBase(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                           const VoxelRenderPassContext& context) const;
    void RecordFinalPresent(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                            const VoxelRenderPassContext& context) const;

private:
    static void RecordShadowMap(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                                const VoxelRenderPassContext& context);
    static void RecordNormalMap(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                                const VoxelRenderPassContext& context);
    static void RecordAmbientMap(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                                 const VoxelRenderPassContext& context);
    static void RecordForwardPath(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                                  const VoxelRenderPassContext& context);
    static void RecordBackBufferInit(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                                     const VoxelRenderPassContext& context);
    static void RecordFullQuad(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                               const VoxelRenderPassContext& context);
    static void RecordDraw(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                           const VoxelRenderPassContext& context,
                           PEPEngine::Graphics::RenderMode mode);
};
