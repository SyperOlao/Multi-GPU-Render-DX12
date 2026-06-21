#pragma once

#include "GCrossAdapterResource.h"
#include "GDescriptor.h"
#include "GTexture.h"

#include <memory>
#include <string>
#include <vector>

struct MultiGpuVoxelRenderTargetDesc
{
    UINT FrameCount = 0;
    UINT Width = 0;
    UINT Height = 0;
    DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT LinearDepthFormat = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT DepthStencilFormat = DXGI_FORMAT_D32_FLOAT;
};

struct MultiGpuVoxelResourceInfo
{
    std::wstring Name;
    std::wstring OwnerDevice;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    UINT Width = 0;
    UINT Height = 0;
    D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
};

struct MultiGpuVoxelFrameRenderTargets
{
    UINT FrameIndex = 0;

    PEPEngine::Graphics::GTexture SecondaryLocalColor;
    PEPEngine::Graphics::GTexture SecondaryLocalLinearDepth;
    PEPEngine::Graphics::GTexture SecondaryLocalDepthStencil;
    std::shared_ptr<GCrossAdapterResource> CrossAdapterColor;
    std::shared_ptr<GCrossAdapterResource> CrossAdapterLinearDepth;
    PEPEngine::Graphics::GTexture PrimaryReceivedSecondaryColor;
    PEPEngine::Graphics::GTexture PrimaryReceivedSecondaryLinearDepth;
    PEPEngine::Graphics::GTexture PrimaryCompositeColor;

    PEPEngine::Graphics::GDescriptor SecondaryRtvDescriptors;
    PEPEngine::Graphics::GDescriptor SecondaryDsvDescriptor;
    PEPEngine::Graphics::GDescriptor PrimarySrvDescriptors;
    PEPEngine::Graphics::GDescriptor PrimaryCompositeDescriptors;
    PEPEngine::Graphics::GDescriptor PrimaryCompositeRtvDescriptor;

    UINT64 ColorTransferBytes = 0;
    UINT64 LinearDepthTransferBytes = 0;
    bool HasReceivedImage = false;
};

class MultiGpuVoxelRenderTargets
{
public:
    bool ValidateCapabilities(const std::shared_ptr<PEPEngine::Graphics::GDevice>& primaryDevice,
                              const std::shared_ptr<PEPEngine::Graphics::GDevice>& secondaryDevice,
                              const MultiGpuVoxelRenderTargetDesc& desc);

    bool Initialize(const std::shared_ptr<PEPEngine::Graphics::GDevice>& primaryDevice,
                    const std::shared_ptr<PEPEngine::Graphics::GDevice>& secondaryDevice,
                    const MultiGpuVoxelRenderTargetDesc& desc);

    void Reset();

    bool IsInitialized() const;
    const std::wstring& GetFailureMessage() const;
    const std::vector<MultiGpuVoxelResourceInfo>& GetResourceTable() const;
    std::vector<MultiGpuVoxelFrameRenderTargets>& GetFrames();
    const std::vector<MultiGpuVoxelFrameRenderTargets>& GetFrames() const;
    UINT GetFrameCount() const;

private:
    bool initialized = false;
    std::wstring failureMessage;
    MultiGpuVoxelRenderTargetDesc currentDesc{};
    std::vector<MultiGpuVoxelFrameRenderTargets> frames;
    std::vector<MultiGpuVoxelResourceInfo> resourceTable;

    static bool SupportsFormat(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device,
                               DXGI_FORMAT format,
                               D3D12_FORMAT_SUPPORT1 requiredSupport);
    static D3D12_RESOURCE_DESC Texture2DDesc(UINT width,
                                             UINT height,
                                             DXGI_FORMAT format,
                                             D3D12_RESOURCE_FLAGS flags,
                                             D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN);
    static UINT64 CopyableTextureBytes(const std::shared_ptr<PEPEngine::Graphics::GDevice>& device,
                                       const D3D12_RESOURCE_DESC& desc);
    void AppendResourceInfo(const std::wstring& name,
                            const std::shared_ptr<PEPEngine::Graphics::GDevice>& owner,
                            DXGI_FORMAT format,
                            UINT width,
                            UINT height,
                            D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES initialState);
};
