#pragma once

#include "GDevice.h"

#include <memory>
#include <vector>

struct SelectedDevices
{
    std::shared_ptr<PEPEngine::Graphics::GDevice> Primary;
    std::shared_ptr<PEPEngine::Graphics::GDevice> Secondary;
};

class DeviceSelectionPolicy
{
public:
    static SelectedDevices Select(const std::vector<std::shared_ptr<PEPEngine::Graphics::GDevice>>& devices);
};
