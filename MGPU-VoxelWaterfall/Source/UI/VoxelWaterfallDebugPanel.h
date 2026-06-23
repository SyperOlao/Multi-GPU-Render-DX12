#pragma once

#include "Source/Voxels/VoxelTypes.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Research/ResearchRunnerTypes.h"

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
    std::string TransferMode;
    bool PublicationEligible = false;
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
    FinalResolveSource& FinalResolveSourceMode;

    VoxelBenchmarkProfiler& BenchmarkProfiler;
    bool BenchmarkVSyncWasEnabled = true;
    bool AutomaticBenchmarkActive = false;
    bool AutomaticBenchmarkHasConfigs = false;
    size_t AutomaticBenchmarkIndex = 0;
    size_t AutomaticBenchmarkCount = 0;
    std::filesystem::path AutomaticBenchmarkSummaryPath;
    uint64_t ProvenanceFileHashCount = 0;
    uint64_t GitProcessSpawnCount = 0;
    uint64_t SlowFrameValidationCount = 0;
    uint64_t BenchmarkMetadataBuildCount = 0;
    bool ResearchRunnerActive = false;
    std::string ResearchRunnerPhase;
    std::string ResearchRunnerReason;
    std::filesystem::path ResearchRunnerOutputPath;
    uint32_t ResearchRunnerConfigIndex = 0;
    uint32_t ResearchRunnerConfigTotal = 0;

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
    std::function<void(const ResearchRunnerRequest&)> RequestResearchRunner;
    std::function<void()> CancelResearchRunner;
};

class VoxelWaterfallDebugPanel
{
public:
    void Draw(const VoxelWaterfallDebugPanelContext& context) const;
};
