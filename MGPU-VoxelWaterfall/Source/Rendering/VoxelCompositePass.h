#pragma once

#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"
#include "Source/Voxels/VoxelTypes.h"

#include <d3d12.h>
#include <memory>

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GDevice;
    class GRootSignature;
    class GraphicPSO;
    class GTexture;
}

struct VoxelCompositePassContext
{
    PEPEngine::Graphics::GTexture& PrimaryBaseColor;
    PEPEngine::Graphics::GTexture& PrimaryDepth;
    MultiGpuVoxelFrameRenderTargets& FrameTargets;
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    float NearZ = 0.1f;
    float FarZ = 1000.0f;
    VoxelCompositeDebugView DebugView = VoxelCompositeDebugView::FinalComposite;
};

class VoxelCompositePass
{
public:
    void Initialize(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device, DXGI_FORMAT outputFormat);
    bool IsInitialized() const;

    void Record(const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
                const VoxelCompositePassContext& context) const;

private:
    struct Constants
    {
        float InvRenderTargetSize[2] = {0.0f, 0.0f};
        float NearZ = 0.1f;
        float FarZ = 1000.0f;
        float DepthEpsilon = 0.02f;
        uint32_t DebugView = 0;
        float SecondaryInvalidDepth = 3.402823466e+38f;
        float Padding = 0.0f;
    };
    static_assert(sizeof(Constants) == 32);

    std::shared_ptr<PEPEngine::Graphics::GDevice> device;
    std::shared_ptr<PEPEngine::Graphics::GRootSignature> rootSignature;
    std::shared_ptr<PEPEngine::Graphics::GraphicPSO> pipelineState;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;

    void RefreshDescriptors(const VoxelCompositePassContext& context) const;
};
