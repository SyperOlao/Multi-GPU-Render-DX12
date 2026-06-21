#pragma once

#include "GDevice.h"

#include <memory>
#include <string>
#include <vector>

struct AdapterSelectionInfo
{
    std::wstring Name;
    bool Hardware = false;
    bool GraphicsQueue = false;
    bool ComputeQueue = false;
    bool CopyQueue = false;
    bool CrossAdapterTexture = false;
    bool SelectedPrimary = false;
    bool SelectedSecondary = false;
    std::wstring Status;
};

struct SelectedDevices
{
    std::shared_ptr<PEPEngine::Graphics::GDevice> Primary;
    std::shared_ptr<PEPEngine::Graphics::GDevice> Secondary;
    std::vector<AdapterSelectionInfo> Adapters;
    std::wstring MultiGpuUnavailableReason;
};

class DeviceSelectionPolicy
{
public:
    static SelectedDevices Select(const std::vector<std::shared_ptr<PEPEngine::Graphics::GDevice>>& devices);
};
