#pragma once

#include "VoxelBenchmarkProfiler.h"

#include <d3d12.h>
#include <functional>
#include <memory>
#include <wrl.h>

struct FrameResource;

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GCommandQueue;
}

struct RenderPipelineContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> RenderQueue;
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> PrimaryComputeQueue;
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> CrossAdapterCopyQueue;
    bool SecondaryWorkThisFrame = false;
    bool UsedSplitMultiGpu = false;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimeRenderFence;
    UINT64& SharedRenderFenceValue;
    UINT64& GraphicsPassFenceValue;
    std::function<void(const std::shared_ptr<PEPEngine::Graphics::GCommandList>&)> RecordGraphicsCommands;
};

class RenderPipeline
{
public:
    void RenderFrame(const RenderPipelineContext& context) const;
};
