#pragma once

#include "Source/Voxels/VoxelTypes.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GDescriptor;
}

struct VoxelWaterfallDebugPanelContext
{
    bool ImGuiInitialized = false;
    std::shared_ptr<PEPEngine::Graphics::GCommandList> CommandList;
    PEPEngine::Graphics::GDescriptor* ImGuiSrvMemory = nullptr;

    VoxelLodArray& Lods;
    VoxelExecutionMode ExecutionMode = VoxelExecutionMode::PrimaryOnly;
    bool SplitMultiGpuAvailable = false;
    std::wstring SplitMultiGpuStatus;
    std::wstring PrimaryAdapterName;
    std::wstring SecondaryAdapterName;
    uint64_t SimulationFrameIndex = 0;

    VoxelBenchmarkProfiler& BenchmarkProfiler;
    bool BenchmarkVSyncWasEnabled = true;
    bool AutomaticBenchmarkActive = false;
    bool AutomaticBenchmarkHasConfigs = false;
    size_t AutomaticBenchmarkIndex = 0;
    size_t AutomaticBenchmarkCount = 0;
    std::filesystem::path AutomaticBenchmarkSummaryPath;

    std::function<void()> DrawSceneLabels;
    std::function<void(VoxelExecutionMode)> ApplyExecutionMode;
    std::function<void()> StartBenchmark;
    std::function<void()> StopBenchmark;
    std::function<void()> StartAutomaticBenchmark;
    std::function<void()> StopAutomaticBenchmark;
    std::function<void(size_t, bool)> SetLodEnabled;
    std::function<void(size_t)> RequestApplyLodSettings;
};

class VoxelWaterfallDebugPanel
{
public:
    void Draw(const VoxelWaterfallDebugPanelContext& context) const;
};
