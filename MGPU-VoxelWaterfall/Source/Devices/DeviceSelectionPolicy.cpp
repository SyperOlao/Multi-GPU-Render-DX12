#include "Source/Devices/DeviceSelectionPolicy.h"

#include <algorithm>

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

    AdapterSelectionInfo BuildInfo(const std::shared_ptr<GDevice>& device)
    {
        AdapterSelectionInfo info{};
        if (!device)
        {
            info.Name = L"unavailable";
            info.Status = L"not initialized";
            return info;
        }

        info.Name = device->GetName();
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

    for (const auto& device : devices)
        selected.Adapters.push_back(BuildInfo(device));

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
