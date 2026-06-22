#pragma once

#include "GDevice.h"

#include <memory>
#include <string>
#include <vector>

struct AdapterSelectionInfo
{
    uint32_t AdapterIndex = 0;
    std::wstring Name;
    uint32_t VendorId = 0;
    uint32_t DeviceId = 0;
    uint32_t SubSysId = 0;
    uint32_t Revision = 0;
    uint64_t DedicatedVideoMemory = 0;
    uint64_t DedicatedSystemMemory = 0;
    uint64_t SharedSystemMemory = 0;
    int32_t LuidHighPart = 0;
    uint32_t LuidLowPart = 0;
    std::string FeatureLevel;
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
