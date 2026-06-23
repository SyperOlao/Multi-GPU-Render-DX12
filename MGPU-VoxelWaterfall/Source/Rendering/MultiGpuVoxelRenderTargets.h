#pragma once

#include "GCrossAdapterResource.h"
#include "GDescriptor.h"
#include "GResource.h"
#include "GTexture.h"
#include "Source/Devices/DeviceSelectionPolicy.h"

#include <memory>
#include <string>
#include <vector>
#include <wrl.h>

struct MultiGpuVoxelRenderTargetDesc
{
    UINT FrameCount = 0;
    UINT Width = 0;
    UINT Height = 0;
    DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT LinearDepthFormat = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT DepthStencilFormat = DXGI_FORMAT_D32_FLOAT;
    CrossAdapterTransferMode TransferMode = CrossAdapterTransferMode::Unavailable;
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
    CrossAdapterTransferMode TransferMode = CrossAdapterTransferMode::Unavailable;

    PEPEngine::Graphics::GTexture SecondaryLocalColor;
    PEPEngine::Graphics::GTexture SecondaryLocalLinearDepth;
    PEPEngine::Graphics::GTexture SecondaryLocalDepthStencil;
    std::shared_ptr<GCrossAdapterResource> CrossAdapterColor;
    std::shared_ptr<GCrossAdapterResource> CrossAdapterLinearDepth;
    struct CopyOnlyBridge
    {
        PEPEngine::Graphics::GResource PrimeBuffer;
        PEPEngine::Graphics::GResource SharedBuffer;
        Microsoft::WRL::ComPtr<ID3D12Heap> PrimeHeap;
        Microsoft::WRL::ComPtr<ID3D12Heap> SharedHeap;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        UINT NumRows = 0;
        UINT64 RowSizeInBytes = 0;
        UINT64 TotalBytes = 0;

        bool IsValid() const
        {
            if (!PrimeBuffer.IsValid() || !SharedBuffer.IsValid() || TotalBytes == 0 ||
                Footprint.Footprint.RowPitch == 0)
            {
                return false;
            }

            return PrimeBuffer.GetD3D12ResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                SharedBuffer.GetD3D12ResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
        }
    };
    CopyOnlyBridge CopyOnlyColor;
    CopyOnlyBridge CopyOnlyLinearDepth;
    PEPEngine::Graphics::GTexture PrimaryReceivedSecondaryColor;
    PEPEngine::Graphics::GTexture PrimaryReceivedSecondaryLinearDepth;
    PEPEngine::Graphics::GTexture PrimaryCompositeColor;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> SecondaryPipelineStatsQueryHeap;
    PEPEngine::Graphics::GResource SecondaryPipelineStatsReadback;

    PEPEngine::Graphics::GDescriptor SecondaryRtvDescriptors;
    PEPEngine::Graphics::GDescriptor SecondaryDsvDescriptor;
    PEPEngine::Graphics::GDescriptor PrimarySrvDescriptors;
    PEPEngine::Graphics::GDescriptor PrimaryCompositeDescriptors;
    PEPEngine::Graphics::GDescriptor PrimaryCompositeRtvDescriptor;

    UINT64 ColorTransferBytes = 0;
    UINT64 LinearDepthTransferBytes = 0;
    UINT64 ExpectedLocalToSharedColorBytes = 0;
    UINT64 ExpectedLocalToSharedDepthBytes = 0;
    UINT64 ExpectedSharedToLocalColorBytes = 0;
    UINT64 ExpectedSharedToLocalDepthBytes = 0;
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
