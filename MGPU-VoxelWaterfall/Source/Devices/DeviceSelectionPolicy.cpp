#include "Source/Devices/DeviceSelectionPolicy.h"

#include <string>
#include <utility>

using PEPEngine::Graphics::GDevice;

namespace
{
    bool IsPreferredNvidiaAdapter(const std::shared_ptr<GDevice>& device)
    {
        return device && device->GetName().find(L"NVIDIA") != std::wstring::npos;
    }
}

SelectedDevices DeviceSelectionPolicy::Select(const std::vector<std::shared_ptr<GDevice>>& devices)
{
    SelectedDevices selected{};
    if (devices.empty())
        return selected;

    selected.Primary = devices[0];
    selected.Secondary = devices.size() > 1 ? devices[1] : devices[0];

    if (devices.size() > 1 && !IsPreferredNvidiaAdapter(selected.Primary) &&
        IsPreferredNvidiaAdapter(selected.Secondary))
    {
        std::swap(selected.Primary, selected.Secondary);
    }

    return selected;
}
