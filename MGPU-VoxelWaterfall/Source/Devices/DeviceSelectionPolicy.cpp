#include "Source/Devices/DeviceSelectionPolicy.h"

#include <algorithm>
#include <array>

using PEPEngine::Graphics::GDevice;
using PEPEngine::Graphics::GQueueType;

const char* CrossAdapterTransferModeName(const CrossAdapterTransferMode mode)
{
    switch (mode)
    {
    case CrossAdapterTransferMode::DirectCrossAdapterTexture: return "DirectCrossAdapterTexture";
    case CrossAdapterTransferMode::CopyOnlyCrossAdapter: return "CopyOnlyCrossAdapter";
    case CrossAdapterTransferMode::WARPVerification: return "WARPVerification";
    case CrossAdapterTransferMode::Unavailable:
    default: return "unavailable";
    }
}

const wchar_t* CrossAdapterTransferModeNameW(const CrossAdapterTransferMode mode)
{
    switch (mode)
    {
    case CrossAdapterTransferMode::DirectCrossAdapterTexture: return L"DirectCrossAdapterTexture";
    case CrossAdapterTransferMode::CopyOnlyCrossAdapter: return L"CopyOnlyCrossAdapter";
    case CrossAdapterTransferMode::WARPVerification: return L"WARPVerification";
    case CrossAdapterTransferMode::Unavailable:
    default: return L"unavailable";
    }
}

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

        if (FAILED(device->GetDXDevice()->CheckFeatureSupport(
                D3D12_FEATURE_FEATURE_LEVELS,
                &data,
                sizeof(data))))
        {
            return "unknown";
        }

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
        return IsHardwareAdapter(device) &&
               IsFullyQueueCapable(device);
    }

    bool CanUseCopyOnlyCrossAdapterPath(const std::shared_ptr<GDevice>& primary,
                                        const std::shared_ptr<GDevice>& candidate)
    {
        return primary &&
               candidate &&
               candidate != primary &&
               !SameAdapterLuid(primary, candidate) &&
               IsHardwareAdapter(candidate) &&
               IsFullyQueueCapable(candidate);
    }

    bool CanUseDirectCrossAdapterTexturePath(const std::shared_ptr<GDevice>& primary,
                                             const std::shared_ptr<GDevice>& candidate)
    {
        return CanUseCopyOnlyCrossAdapterPath(primary, candidate) &&
               primary->IsCrossAdapterTextureSupported() &&
               candidate->IsCrossAdapterTextureSupported();
    }

    bool CanBeSecondary(const std::shared_ptr<GDevice>& primary,
                        const std::shared_ptr<GDevice>& candidate)
    {
        // Важно:
        // CrossAdapterTextureSupported == false НЕ означает, что адаптер нельзя
        // использовать вообще. Это значит, что нельзя безопасно рассчитывать
        // на direct RTV/SRV/UAV usage для cross-adapter row-major textures.
        //
        // Для проверки текущего explicit multi-adapter pipeline разрешаем
        // copy-only secondary path: второй hardware adapter + graphics/compute/copy queues.
        return CanUseCopyOnlyCrossAdapterPath(primary, candidate);
    }

    CrossAdapterTransferMode ResolveTransferMode(const std::shared_ptr<GDevice>& primary,
                                                 const std::shared_ptr<GDevice>& secondary)
    {
        if (!CanUseCopyOnlyCrossAdapterPath(primary, secondary))
            return CrossAdapterTransferMode::Unavailable;
        if (CanUseDirectCrossAdapterTexturePath(primary, secondary))
            return CrossAdapterTransferMode::DirectCrossAdapterTexture;
        return CrossAdapterTransferMode::CopyOnlyCrossAdapter;
    }

    bool IsBetterPrimaryCandidate(const std::shared_ptr<GDevice>& left,
                                  const std::shared_ptr<GDevice>& right)
    {
        if (!left)
            return true;
        if (!right)
            return false;

        const auto& leftDesc = left->GetDesc();
        const auto& rightDesc = right->GetDesc();

        return leftDesc.DedicatedVideoMemory < rightDesc.DedicatedVideoMemory;
    }

    bool IsBetterSecondaryCandidate(const std::shared_ptr<GDevice>& primary,
                                    const std::shared_ptr<GDevice>& current,
                                    const std::shared_ptr<GDevice>& candidate)
    {
        if (!current)
            return true;
        if (!candidate)
            return false;

        const bool currentDirect = CanUseDirectCrossAdapterTexturePath(primary, current);
        const bool candidateDirect = CanUseDirectCrossAdapterTexturePath(primary, candidate);

        if (currentDirect != candidateDirect)
            return candidateDirect;

        const auto& currentDesc = current->GetDesc();
        const auto& candidateDesc = candidate->GetDesc();

        return currentDesc.DedicatedVideoMemory < candidateDesc.DedicatedVideoMemory;
    }

    std::wstring BaseStatusForDevice(const std::shared_ptr<GDevice>& device)
    {
        if (!device)
            return L"not initialized";

        if (!IsHardwareAdapter(device))
            return L"software adapter excluded";

        if (!IsFullyQueueCapable(device))
            return L"missing required graphics/compute/copy queue";

        if (!device->IsCrossAdapterTextureSupported())
        {
            return L"queue-capable hardware adapter; direct cross-adapter RTV/SRV/UAV unsupported; copy-only candidate";
        }

        return L"queue-capable hardware adapter; direct cross-adapter texture supported";
    }

    AdapterSelectionInfo BuildInfo(const std::shared_ptr<GDevice>& device,
                                   const uint32_t adapterIndex)
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

        info.Status = BaseStatusForDevice(device);

        return info;
    }

    bool SameAdapterInfoAsDevice(const AdapterSelectionInfo& info,
                                 const std::shared_ptr<GDevice>& device)
    {
        if (!device)
            return false;

        const auto& desc = device->GetDesc();

        return info.LuidHighPart == desc.AdapterLuid.HighPart &&
               info.LuidLowPart == desc.AdapterLuid.LowPart;
    }
}

SelectedDevices DeviceSelectionPolicy::Select(const std::vector<std::shared_ptr<GDevice>>& devices)
{
    SelectedDevices selected{};
    selected.Adapters.reserve(devices.size());

    for (uint32_t adapterIndex = 0; adapterIndex < devices.size(); ++adapterIndex)
    {
        selected.Adapters.push_back(BuildInfo(devices[adapterIndex], adapterIndex));
    }

    std::vector<std::shared_ptr<GDevice>> primaryCandidates;
    primaryCandidates.reserve(devices.size());

    for (const auto& device : devices)
    {
        if (CanBePrimary(device))
            primaryCandidates.push_back(device);
    }

    if (primaryCandidates.empty())
    {
        selected.MultiGpuUnavailableReason =
            L"MultiGpu unavailable: no hardware adapter with graphics/compute/copy queues";
        return selected;
    }

    selected.Primary = *std::max_element(
        primaryCandidates.begin(),
        primaryCandidates.end(),
        IsBetterPrimaryCandidate);

    std::shared_ptr<GDevice> bestSecondary;

    for (const auto& device : devices)
    {
        if (!CanBeSecondary(selected.Primary, device))
            continue;

        if (IsBetterSecondaryCandidate(selected.Primary, bestSecondary, device))
            bestSecondary = device;
    }

    selected.Secondary = bestSecondary;
    selected.TransferMode = ResolveTransferMode(selected.Primary, selected.Secondary);
    selected.PublicationEligible = selected.TransferMode == CrossAdapterTransferMode::DirectCrossAdapterTexture ||
        selected.TransferMode == CrossAdapterTransferMode::CopyOnlyCrossAdapter;

    if (!selected.Secondary)
    {
        selected.MultiGpuUnavailableReason =
            L"MultiGpu unavailable: no distinct hardware adapter with graphics/compute/copy queues";
    }
    else
    {
        selected.MultiGpuUnavailableReason.clear();
    }

    for (auto& info : selected.Adapters)
    {
        if (SameAdapterInfoAsDevice(info, selected.Primary))
        {
            info.SelectedPrimary = true;
            info.Status += L"; selected primary";
        }

        if (SameAdapterInfoAsDevice(info, selected.Secondary))
        {
            info.SelectedSecondary = true;

            if (CanUseDirectCrossAdapterTexturePath(selected.Primary, selected.Secondary))
            {
                info.Status += L"; selected secondary; direct cross-adapter texture path";
            }
            else
            {
                info.Status += L"; selected secondary; copy-only cross-adapter path; direct RTV/SRV/UAV unsupported";
            }
        }
    }

    return selected;
}
