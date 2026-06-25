#pragma once

#include "GDescriptor.h"
#include "ShaderBuffersData.h"
#include "GTexture.h"

struct FrameResource
{
    FrameResource(std::shared_ptr<PEPEngine::Graphics::GDevice> primeDevices,
                  std::shared_ptr<PEPEngine::Graphics::GDevice> secondDevice, UINT passCount,
                  UINT materialCount);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();


    PEPEngine::Graphics::GDescriptor BackBufferRTVMemory;

    std::shared_ptr<PEPEngine::Graphics::ConstantUploadBuffer<PassConstants>> PrimePassConstantUploadBuffer;
    std::shared_ptr<PEPEngine::Graphics::ConstantUploadBuffer<PassConstants>> SecondaryPassConstantUploadBuffer;
    std::shared_ptr<PEPEngine::Graphics::ConstantUploadBuffer<SsaoConstants>> SsaoConstantUploadBuffer;
    std::shared_ptr<PEPEngine::Graphics::StructuredUploadBuffer<MaterialConstants>> MaterialBuffer;


    UINT64 PrimeRenderFenceValue = 0;
    UINT64 PrimaryBaseRenderFenceValue = 0;
    UINT64 SecondaryRenderFenceValue = 0;
    UINT64 SecondaryLocalToSharedCopyFenceValue = 0;
    UINT64 CrossAdapterRenderReadyFenceValue = 0;
    UINT64 PrimarySharedToLocalCopyFenceValue = 0;
    UINT64 PrimarySecondaryImageReadyFenceValue = 0;
    UINT64 PrimeCopyFenceValue = 0;
    UINT64 PrimaryComputeFenceValue = 0;
    UINT64 SecondaryComputeFenceValue = 0;
    UINT64 ComputeFenceValue = 0;

    uint64_t SceneGeneration = 0;
    uint64_t PartitionGeneration = 0;
    uint64_t RenderTargetGeneration = 0;
    uint64_t DescriptorGeneration = 0;
};
