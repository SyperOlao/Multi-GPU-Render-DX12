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
    std::shared_ptr<PEPEngine::Graphics::ConstantUploadBuffer<SsaoConstants>> SsaoConstantUploadBuffer;
    std::shared_ptr<PEPEngine::Graphics::StructuredUploadBuffer<MaterialConstants>> MaterialBuffer;


    UINT64 PrimeRenderFenceValue = 0;
    UINT64 PrimeCopyFenceValue = 0;
    UINT64 ComputeFenceValue = 0;
};
