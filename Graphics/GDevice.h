#pragma once
#include <array>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include "dxgi1_6.h"
#include "Lazy.h"
#include "MemoryAllocator.h"

using namespace Microsoft::WRL;

namespace PEPEngine::Graphics
{
    using namespace Allocator;
    using namespace Utils;

    class GCommandQueue;
    class GResource;
    class GAllocator;
    class GDeviceFactory;
    class GDescriptor;

    enum class GQueueType
    {
        Graphics,
        Compute,
        Copy,
        Count
    };

    struct GDeferredFenceSnapshot
    {
        std::array<uint64_t, static_cast<size_t>(GQueueType::Count)> QueueFenceValues{};
    };

    struct GDescriptorAllocatorStats
    {
        uint64_t HeapPages = 0;
        uint64_t DescriptorCapacity = 0;
        uint64_t FreeDescriptors = 0;
        uint64_t StaleRanges = 0;
        uint64_t StaleDescriptors = 0;
        uint64_t ActiveDescriptors = 0;
    };

    struct GCommandQueueLifetimeStats
    {
        uint64_t SubmittedFenceValue = 0;
        uint64_t CompletedFenceValue = 0;
        size_t CreatedCommandLists = 0;
        size_t AvailableCommandLists = 0;
        size_t InFlightCommandLists = 0;
    };

    struct GVideoMemoryStats
    {
        bool Valid = false;
        uint64_t LocalBudget = 0;
        uint64_t LocalCurrentUsage = 0;
        uint64_t LocalAvailableForReservation = 0;
        uint64_t LocalCurrentReservation = 0;
        uint64_t NonLocalBudget = 0;
        uint64_t NonLocalCurrentUsage = 0;
        uint64_t NonLocalAvailableForReservation = 0;
        uint64_t NonLocalCurrentReservation = 0;
    };

    struct GDeviceLifetimeStats
    {
        std::array<GCommandQueueLifetimeStats, static_cast<size_t>(GQueueType::Count)> Queues{};
        std::array<GDescriptorAllocatorStats, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> DescriptorAllocators{};
        GVideoMemoryStats VideoMemory{};
    };

    class GDevice : std::enable_shared_from_this<GDevice>
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> infoQueue;
        ComPtr<IDXGIAdapter3> adapter;


        std::array<std::shared_ptr<GAllocator>, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> graphicAllocators;
        std::array<std::shared_ptr<GCommandQueue>, static_cast<int>(GQueueType::Count)> queues;

        bool crossAdapterTextureSupport;

        // Get the timestamp values from the result buffers.
        D3D12_RANGE readRange = {};
        const D3D12_RANGE emptyRange = {};

        void* mappedData = nullptr;
        Lazy<GResource> timestampResultBuffer;
        Lazy<ComPtr<ID3D12QueryHeap>> timestampQueryHeap;

        DXGI_ADAPTER_DESC2 desc;
        std::wstring name;

        friend GDeviceFactory;


        void InitialDescriptorAllocator();
        void InitialCommandQueue();
        void InitialQueryTimeStamp();
        void InitialDevice();

        bool isInitialized = false;

    public:
        GDevice(const ComPtr<IDXGIAdapter3>& adapter);

        ~GDevice();

        bool IsInitialized() const { return isInitialized; }
        void Initialize();

        const DXGI_ADAPTER_DESC2& GetDesc() const { return desc; }

        HANDLE SharedHandle(const ComPtr<ID3D12DeviceChild>& deviceObject, const SECURITY_ATTRIBUTES* attributes,
                            DWORD access,
                            LPCWSTR name) const;

        void ShareResource(const GResource& resource, const std::shared_ptr<GDevice>& destDevice, GResource& destResource,
                           const SECURITY_ATTRIBUTES* attributes = nullptr,
                           DWORD access = GENERIC_ALL, LPCWSTR name = L"") const;


        void ReleaseSlateDescriptors(uint64_t frameCount) const;
        void ResetAllocators(uint64_t frameCount) const;
        GDeferredFenceSnapshot CaptureSubmittedFenceSnapshot() const;
        bool IsFenceSnapshotComplete(const GDeferredFenceSnapshot& snapshot) const;

        GDescriptor AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t descriptorCount = 1) const;

        UINT GetNodeMask() const;


        bool IsCrossAdapterTextureSupported() const;

        void SharedFence(ComPtr<ID3D12Fence>& primaryFence, const std::shared_ptr<GDevice>& sharedDevice,
                         ComPtr<ID3D12Fence>& sharedFence, UINT64 fenceValue = 0,
                         const SECURITY_ATTRIBUTES* attributes = nullptr,
                         DWORD access = GENERIC_ALL, LPCWSTR name = L"") const;

        /// Non-throwing variant for SharedFence; clears output pointers on failure.
        bool TrySharedFence(ComPtr<ID3D12Fence>& primaryFence, const std::shared_ptr<GDevice>& sharedDevice,
                            ComPtr<ID3D12Fence>& sharedFence, UINT64 fenceValue = 0,
                            const SECURITY_ATTRIBUTES* attributes = nullptr,
                            DWORD access = GENERIC_ALL, LPCWSTR name = L"") const;


        std::shared_ptr<GCommandQueue>& GetCommandQueue(GQueueType type = GQueueType::Graphics);

        UINT GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const;

        void Flush() const;

        void TerminatedQueuesWorker() const;
        GDeviceLifetimeStats GetLifetimeStats() const;
        GVideoMemoryStats QueryVideoMemoryStats() const;
        void ReportLiveDeviceObjects() const;

        ComPtr<ID3D12Device> GetDXDevice() const;
        ComPtr<ID3D12InfoQueue> GetInfoQueue() const;

        std::wstring GetName() const
        {
            return name;
        }

        bool TryGetAdapterDesc3(DXGI_ADAPTER_DESC3& outDesc) const;
    };
}
