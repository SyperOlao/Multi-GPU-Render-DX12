#pragma once

#include "Source/Voxels/VoxelTypes.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace PEPEngine::Graphics
{
    class GCommandList;
    class GDescriptor;
}

struct VoxelFrameGraphTelemetry;

struct VoxelWaterfallDebugPanelContext
{
    bool ImGuiInitialized = false;
    std::shared_ptr<PEPEngine::Graphics::GCommandList> CommandList;
    PEPEngine::Graphics::GDescriptor* ImGuiSrvMemory = nullptr;

    VoxelSceneWorkload& Workload;
    VoxelExecutionMode RequestedExecutionMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ExecutionMode = VoxelExecutionMode::SingleGpuFull;
    bool MultiGpuAvailable = false;
    std::wstring MultiGpuStatus;
    std::wstring PrimaryAdapterName;
    std::wstring SecondaryAdapterName;
    const std::vector<std::wstring>* AdapterReportLines = nullptr;
    uint64_t SimulationFrameIndex = 0;
    double AccumulatedSimulationTime = 0.0;
    uint32_t SimulationStepsThisFrame = 0;
    float InterpolationAlpha = 0.0f;
    uint32_t RecycledVoxelCount = 0;
    uint32_t AliveVoxelCount = 0;
    uint32_t ExpectedVoxelCount = 0;
    const VoxelFrameGraphTelemetry* FrameGraphTelemetry = nullptr;
    VoxelCompositeDebugView& CompositeDebugView;

    VoxelBenchmarkProfiler& BenchmarkProfiler;
    bool BenchmarkVSyncWasEnabled = true;
    bool AutomaticBenchmarkActive = false;
    bool AutomaticBenchmarkHasConfigs = false;
    size_t AutomaticBenchmarkIndex = 0;
    size_t AutomaticBenchmarkCount = 0;
    std::filesystem::path AutomaticBenchmarkSummaryPath;

    std::function<void(VoxelResearchWorkloadProfile)> ApplyWorkloadProfile;
    std::function<void(VoxelResearchCameraMode)> ApplyCameraMode;
    std::function<void(VoxelResearchLightingPreset)> ApplyLightingPreset;
    std::function<void(VoxelRenderResolutionPreset)> ApplyRenderResolutionPreset;
    std::function<void(VoxelExecutionMode)> ApplyExecutionMode;
    std::function<void()> StartBenchmark;
    std::function<void()> StopBenchmark;
    std::function<void()> RunVisualValidation;
    std::function<void()> StartAutomaticBenchmark;
    std::function<void()> StopAutomaticBenchmark;
    std::function<void()> RequestApplyWorkloadSettings;
};

class VoxelWaterfallDebugPanel
{
public:
    void Draw(const VoxelWaterfallDebugPanelContext& context) const;
};
