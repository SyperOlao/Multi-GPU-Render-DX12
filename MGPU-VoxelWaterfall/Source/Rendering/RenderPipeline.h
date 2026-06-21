#pragma once

#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"
#include "Source/Voxels/VoxelTypes.h"

#include <d3d12.h>
#include <functional>
#include <memory>
#include <wrl.h>

struct FrameResource;
class VoxelGpuPartition;

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GCommandQueue;
}

struct VoxelFrameGraphTelemetry
{
    bool PrimaryComputeSubmitted = false;
    bool PrimaryBaseGraphicsSubmitted = false;
    bool SecondaryComputeSubmitted = false;
    bool SecondaryGraphicsSubmitted = false;
    uint32_t SecondaryDrawCalls = 0;
    uint32_t SecondaryRenderedVoxelCount = 0;
    uint32_t FrameResourceIndex = 0;
    UINT64 PrimaryComputeFenceValue = 0;
    UINT64 PrimaryBaseGraphicsFenceValue = 0;
    UINT64 SecondaryComputeFenceValue = 0;
    UINT64 SecondaryGraphicsFenceValue = 0;
    UINT64 SecondaryLocalToSharedCopyFenceValue = 0;
    UINT64 CrossAdapterRenderReadyFenceValue = 0;
    UINT64 PrimarySharedToLocalCopyFenceValue = 0;
    UINT64 PrimarySecondaryImageReadyFenceValue = 0;
    UINT64 FinalPresentFenceValue = 0;
    UINT64 ColorBytesTransferred = 0;
    UINT64 DepthBytesTransferred = 0;
    UINT64 TotalCrossAdapterBytes = 0;
    UINT64 ParticleTransferBytes = 0;
    UINT64 RenderOutputTransferBytes = 0;
    bool SecondaryImageReused = false;
    bool CompositeSubmitted = false;
    bool CompositeUsedSecondaryImage = false;
    bool VisualValidationPassed = false;
    VoxelCompositeDebugView CompositeDebugView = VoxelCompositeDebugView::FinalComposite;
    VoxelExecutionMode RequestedMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ActualMode = VoxelExecutionMode::SingleGpuFull;
};

struct PrimaryBasePassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> RenderQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimaryComputeFence;
    UINT64 PrimaryComputeFenceValue = 0;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    UINT64& GraphicsPassFenceValue;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
    std::function<void(const std::shared_ptr<PEPEngine::Graphics::GCommandList>&)> RecordPrimaryBaseCommands;
};

struct SecondaryVoxelGraphicsPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> SecondaryGraphicsQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> SecondaryComputeFence;
    UINT64 SecondaryComputeFenceValue = 0;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    VoxelGpuPartition& SecondaryPartition;
    MultiGpuVoxelFrameRenderTargets& RenderTargets;
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
};

struct SecondaryLocalToSharedCopyPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> SecondaryCopyQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> SecondaryRenderFence;
    UINT64 SecondaryRenderFenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> CrossAdapterRenderReadyFence;
    UINT64& CrossAdapterRenderReadyFenceValue;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    MultiGpuVoxelFrameRenderTargets& RenderTargets;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
};

struct PrimarySharedToLocalCopyPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> PrimaryCopyQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> CrossAdapterRenderReadyFence;
    UINT64 CrossAdapterRenderReadyFenceValue = 0;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    MultiGpuVoxelFrameRenderTargets& RenderTargets;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
};

struct FinalCompositeAndPresentPassContext
{
    std::shared_ptr<PEPEngine::Graphics::GCommandQueue> RenderQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimaryBaseRenderFence;
    UINT64 PrimaryBaseRenderFenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> PrimarySecondaryImageReadyFence;
    UINT64 PrimarySecondaryImageReadyFenceValue = 0;
    bool WaitForSecondaryImage = false;
    uint32_t TimestampHeapIndex = 0;
    FrameResource& CurrentFrameResource;
    UINT64& GraphicsPassFenceValue;
    VoxelBenchmarkProfiler& BenchmarkProfiler;
    VoxelFrameGraphTelemetry* Telemetry = nullptr;
    std::function<void(const std::shared_ptr<PEPEngine::Graphics::GCommandList>&)> RecordFinalPresentCommands;
};

class RenderPipeline
{
public:
    void SubmitPrimaryBasePass(const PrimaryBasePassContext& context) const;
    void SubmitSecondaryVoxelPass(const SecondaryVoxelGraphicsPassContext& context) const;
    void SubmitSecondaryLocalToSharedCopyPass(const SecondaryLocalToSharedCopyPassContext& context) const;
    void SubmitPrimarySharedToLocalCopyPass(const PrimarySharedToLocalCopyPassContext& context) const;
    void SubmitFinalCompositeAndPresentPass(const FinalCompositeAndPresentPassContext& context) const;
};
