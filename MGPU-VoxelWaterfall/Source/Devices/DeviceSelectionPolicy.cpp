#include "Source/Devices/DeviceSelectionPolicy.h"

#include <algorithm>
#include <array>

using PEPEngine::Graphics::GDevice;
using PEPEngine::Graphics::GQueueType;

namespace
{
    bool HasQueue(const std::shared_ptr<GDevice>& device, const GQueueType type)
    {
        return device && device->GetCommandQueue(type) != nullptr;
    }

    bool IsHardwareAdapter(const std::shared_ptr<GDevice>& device)
    {
        if (!device)
            return false;

        const auto& desc = device->GetDesc();
        return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
    }

    bool IsFullyQueueCapable(const std::shared_ptr<GDevice>& device)
    {
        return HasQueue(device, GQueueType::Graphics) &&
            HasQueue(device, GQueueType::Compute) &&
            HasQueue(device, GQueueType::Copy);
    }

    bool SameAdapterLuid(const std::shared_ptr<GDevice>& left,
                         const std::shared_ptr<GDevice>& right)
    {
        if (!left || !right)
            return false;

        const auto& leftDesc = left->GetDesc();
        const auto& rightDesc = right->GetDesc();
        return leftDesc.AdapterLuid.HighPart == rightDesc.AdapterLuid.HighPart &&
            leftDesc.AdapterLuid.LowPart == rightDesc.AdapterLuid.LowPart;
    }

    std::string HighestFeatureLevel(const std::shared_ptr<GDevice>& device)
    {
        if (!device)
            return "unavailable";

        std::array<D3D_FEATURE_LEVEL, 5> levels = {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS data{};
        data.NumFeatureLevels = static_cast<UINT>(levels.size());
        data.pFeatureLevelsRequested = levels.data();
        if (FAILED(device->GetDXDevice()->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data))))
            return "unknown";

        switch (data.MaxSupportedFeatureLevel)
        {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        default: return "unknown";
        }
    }

    bool CanBePrimary(const std::shared_ptr<GDevice>& device)
    {
        return IsHardwareAdapter(device) && IsFullyQueueCapable(device);
    }

    bool CanBeSecondary(const std::shared_ptr<GDevice>& primary,
                        const std::shared_ptr<GDevice>& candidate)
    {
        return candidate &&
            candidate != primary &&
            !SameAdapterLuid(primary, candidate) &&
            IsHardwareAdapter(candidate) &&
            IsFullyQueueCapable(candidate) &&
            primary &&
            primary->IsCrossAdapterTextureSupported() &&
            candidate->IsCrossAdapterTextureSupported();
    }

    AdapterSelectionInfo BuildInfo(const std::shared_ptr<GDevice>& device, const uint32_t adapterIndex)
    {
        AdapterSelectionInfo info{};
        info.AdapterIndex = adapterIndex;
        if (!device)
        {
            info.Name = L"unavailable";
            info.Status = L"not initialized";
            return info;
        }

        info.Name = device->GetName();
        const auto& desc = device->GetDesc();
        info.VendorId = desc.VendorId;
        info.DeviceId = desc.DeviceId;
        info.SubSysId = desc.SubSysId;
        info.Revision = desc.Revision;
        info.DedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.DedicatedSystemMemory = desc.DedicatedSystemMemory;
        info.SharedSystemMemory = desc.SharedSystemMemory;
        info.LuidHighPart = desc.AdapterLuid.HighPart;
        info.LuidLowPart = desc.AdapterLuid.LowPart;
        info.FeatureLevel = HighestFeatureLevel(device);
        info.Hardware = IsHardwareAdapter(device);
        info.GraphicsQueue = HasQueue(device, GQueueType::Graphics);
        info.ComputeQueue = HasQueue(device, GQueueType::Compute);
        info.CopyQueue = HasQueue(device, GQueueType::Copy);
        info.CrossAdapterTexture = device->IsCrossAdapterTextureSupported();

        if (!info.Hardware)
            info.Status = L"software adapter excluded";
        else if (!info.GraphicsQueue || !info.ComputeQueue || !info.CopyQueue)
            info.Status = L"missing required graphics/compute/copy queue";
        else if (!info.CrossAdapterTexture)
            info.Status = L"cross-adapter row-major texture path unavailable";
        else
            info.Status = L"compatible";

        return info;
    }
}

SelectedDevices DeviceSelectionPolicy::Select(const std::vector<std::shared_ptr<GDevice>>& devices)
{
    SelectedDevices selected{};
    selected.Adapters.reserve(devices.size());

    for (uint32_t adapterIndex = 0; adapterIndex < devices.size(); ++adapterIndex)
        selected.Adapters.push_back(BuildInfo(devices[adapterIndex], adapterIndex));

    std::vector<std::shared_ptr<GDevice>> primaryCandidates;
    for (const auto& device : devices)
    {
        if (CanBePrimary(device))
            primaryCandidates.push_back(device);
    }

    if (primaryCandidates.empty())
    {
        selected.MultiGpuUnavailableReason = L"MultiGpu unavailable: no hardware adapter with graphics/compute/copy queues";
        return selected;
    }

    selected.Primary = *std::max_element(
        primaryCandidates.begin(),
        primaryCandidates.end(),
        [](const std::shared_ptr<GDevice>& left, const std::shared_ptr<GDevice>& right)
        {
            return left->GetDesc().DedicatedVideoMemory < right->GetDesc().DedicatedVideoMemory;
        });

    for (const auto& device : devices)
    {
        if (CanBeSecondary(selected.Primary, device))
        {
            selected.Secondary = device;
            break;
        }
    }

    if (!selected.Secondary)
    {
        selected.MultiGpuUnavailableReason =
            L"MultiGpu unavailable: no distinct compatible hardware adapter with cross-adapter texture support";
    }

    for (auto& info : selected.Adapters)
    {
        if (selected.Primary && info.Name == selected.Primary->GetName())
        {
            info.SelectedPrimary = true;
            info.Status += L"; selected primary";
        }
        if (selected.Secondary && info.Name == selected.Secondary->GetName())
        {
            info.SelectedSecondary = true;
            info.Status += L"; selected secondary";
        }
    }

    return selected;
}
