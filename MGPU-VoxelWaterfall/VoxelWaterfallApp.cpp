#include "VoxelWaterfallApp.h"
#include "GDescriptorHeap.h"
#include "Source/Assets/SampleAssetManifest.h"
#include "Source/Devices/DeviceSelectionPolicy.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <DirectXMath.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "GameObject.h"
#include "GDeviceFactory.h"
#include "GModel.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MathHelper.h"
#include "ModelRenderer.h"
#include "Source/Voxels/VoxelGpuPartition.h"
#include "Source/Voxels/VoxelResearchEnvironmentGenerator.h"
#include "Source/Scene/VoxelResearchCameraController.h"
#include "Transform.h"
#include "Window.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr float DebugUiScale = 2.25f;
    constexpr const char* VoxelSceneGpuObjectName = "VoxelSceneGpuPartitions";

    struct StaticLayerSnapshot
    {
        bool Valid = false;
        uint32_t StaticVoxelBudget = 0;
        StaticVoxelBudgetPreset StaticBudgetPreset = StaticVoxelBudgetPreset::Small;
        StaticVoxelStorageMode StaticStorageMode = StaticVoxelStorageMode::SurfaceOnly;
        uint32_t StaticGenerationSeed = 0;
        float StaticVoxelSize = 0.0f;
        uint32_t ActualStaticVoxelCount = 0;
        VoxelGridCoordinate GridOrigin{};
        DirectX::SimpleMath::Vector3 BoundsMin = DirectX::SimpleMath::Vector3::Zero;
        DirectX::SimpleMath::Vector3 BoundsMax = DirectX::SimpleMath::Vector3::Zero;
    };

    std::string LuidToString(const LUID& luid)
    {
        std::ostringstream stream;
        stream << std::hex << std::uppercase
            << static_cast<uint32_t>(luid.HighPart) << ':'
            << static_cast<uint32_t>(luid.LowPart);
        return stream.str();
    }

    void FillAdapterMetadata(
        const std::shared_ptr<GDevice>& device,
        uint32_t& vendorId,
        uint32_t& deviceId,
        uint64_t& dedicatedMemory,
        std::string& adapterLuid)
    {
        if (!device)
            return;

        const auto& desc = device->GetDesc();
        vendorId = desc.VendorId;
        deviceId = desc.DeviceId;
        dedicatedMemory = static_cast<uint64_t>(desc.DedicatedVideoMemory);
        adapterLuid = LuidToString(desc.AdapterLuid);
    }

    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    const VoxelSceneLayer* FindStaticLayer(const VoxelSceneWorkload& workload)
    {
        const auto it = std::find_if(
            workload.Layers.begin(),
            workload.Layers.end(),
            [](const VoxelSceneLayer& layer)
            {
                return layer.LayerType == VoxelSceneLayerType::Static;
            });
        return it == workload.Layers.end() ? nullptr : &(*it);
    }

    StaticLayerSnapshot CaptureStaticLayerSnapshot(const VoxelSceneWorkload& workload)
    {
        StaticLayerSnapshot snapshot{};
        const auto* staticLayer = FindStaticLayer(workload);
        if (!staticLayer)
            return snapshot;

        snapshot.Valid = true;
        snapshot.StaticVoxelBudget = workload.StaticTelemetry.RequestedVoxelBudget > 0
                                          ? workload.StaticTelemetry.RequestedVoxelBudget
                                          : workload.StaticVoxelBudget;
        snapshot.StaticBudgetPreset = workload.StaticTelemetry.BudgetPreset;
        snapshot.StaticStorageMode = workload.StaticTelemetry.StorageMode;
        snapshot.StaticGenerationSeed = workload.StaticTelemetry.GenerationSeed;
        snapshot.StaticVoxelSize = staticLayer->RenderSettings.VoxelSize;
        snapshot.ActualStaticVoxelCount = staticLayer->LogicalVoxelCount();
        snapshot.GridOrigin = staticLayer->GridOrigin;
        snapshot.BoundsMin = staticLayer->BoundsMin;
        snapshot.BoundsMax = staticLayer->BoundsMax;
        return snapshot;
    }

    bool StaticGenerationInputsMatch(
        const StaticLayerSnapshot& snapshot,
        const VoxelSceneWorkload& workload)
    {
        return snapshot.Valid &&
            snapshot.StaticVoxelBudget == workload.StaticVoxelBudget &&
            snapshot.StaticBudgetPreset == workload.StaticBudgetPreset &&
            snapshot.StaticStorageMode == workload.StaticStorageMode &&
            snapshot.StaticGenerationSeed == workload.StaticGenerationSeed &&
            snapshot.StaticVoxelSize == workload.StaticVoxelSize;
    }

    bool SameGridOrigin(const VoxelGridCoordinate& lhs, const VoxelGridCoordinate& rhs)
    {
        return lhs.X == rhs.X && lhs.Y == rhs.Y && lhs.Z == rhs.Z;
    }

    bool SameVector(const DirectX::SimpleMath::Vector3& lhs, const DirectX::SimpleMath::Vector3& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }

    void AssertStaticLayerShapePreserved(
        const StaticLayerSnapshot& before,
        const VoxelSceneWorkload& after)
    {
        if (!StaticGenerationInputsMatch(before, after))
            return;

        const auto* staticLayer = FindStaticLayer(after);
        assert(staticLayer && "Static layer must survive settings changes when static generation inputs are unchanged");
        assert(staticLayer->RenderSettings.VoxelSize == before.StaticVoxelSize);
        assert(staticLayer->LogicalVoxelCount() == before.ActualStaticVoxelCount);
        assert(SameGridOrigin(staticLayer->GridOrigin, before.GridOrigin));
        assert(SameVector(staticLayer->BoundsMin, before.BoundsMin));
        assert(SameVector(staticLayer->BoundsMax, before.BoundsMax));
    }

    std::filesystem::path ResolveVoxelWaterfallAssetPath(const std::filesystem::path& relativePath)
    {
        const std::array<std::filesystem::path, 4> candidates =
        {
            relativePath,
            std::filesystem::path(L"MGPU-VoxelWaterfall") / relativePath,
            GetExecutableDirectory() / relativePath,
            GetExecutableDirectory() / L"..\\.." / L"MGPU-VoxelWaterfall" / relativePath
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        return relativePath;
    }

    bool UsesMultiGpuExecution(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::MultiGpuFull ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    bool UsesTemporalExecution(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    const char* ProfileName(const VoxelResearchWorkloadProfile profile)
    {
        switch (profile)
        {
        case VoxelResearchWorkloadProfile::StaticRenderOnly:
            return "StaticRenderOnly";
        case VoxelResearchWorkloadProfile::DynamicSimulationAndRender:
            return "DynamicSimulationAndRender";
        case VoxelResearchWorkloadProfile::MixedStaticAndDynamic:
            return "MixedStaticAndDynamic";
        case VoxelResearchWorkloadProfile::OcclusionValidation:
            return "OcclusionValidation";
        case VoxelResearchWorkloadProfile::SpatialLodDemonstration:
            return "SpatialLodDemonstration";
        default:
            return "Unknown";
        }
    }

    const char* PartitionStrategyName(const VoxelPartitionStrategy strategy)
    {
        switch (strategy)
        {
        case VoxelPartitionStrategy::HashedChunks:
            return "HashedChunks";
        case VoxelPartitionStrategy::SpatialPlane:
            return "SpatialPlane";
        default:
            return "Unknown";
        }
    }

    const char* LoadBalanceScenarioName(const VoxelLoadBalanceScenario scenario)
    {
        switch (scenario)
        {
        case VoxelLoadBalanceScenario::Balanced:
            return "Balanced";
        case VoxelLoadBalanceScenario::PrimaryHeavy:
            return "PrimaryHeavy";
        case VoxelLoadBalanceScenario::SecondaryHeavy:
            return "SecondaryHeavy";
        default:
            return "Unknown";
        }
    }

    const char* TemporalPolicyName(const VoxelTemporalPolicy policy)
    {
        return policy == VoxelTemporalPolicy::Decimated ? "Decimated" : "Full";
    }

    const char* CameraModeName(const VoxelResearchCameraMode mode)
    {
        switch (mode)
        {
        case VoxelResearchCameraMode::Interactive:
            return "Interactive";
        case VoxelResearchCameraMode::FixedOverview:
            return "FixedOverview";
        case VoxelResearchCameraMode::FixedOcclusion:
            return "FixedOcclusion";
        case VoxelResearchCameraMode::WaterfallCloseup:
            return "WaterfallCloseup";
        case VoxelResearchCameraMode::LodSweepRoute:
            return "LodSweepRoute";
        case VoxelResearchCameraMode::BenchmarkRoute:
            return "BenchmarkRoute";
        default:
            return "Unknown";
        }
    }

    const char* LightingPresetName(const VoxelResearchLightingPreset preset)
    {
        switch (preset)
        {
        case VoxelResearchLightingPreset::BenchmarkNeutral:
            return "BenchmarkNeutral";
        case VoxelResearchLightingPreset::DemoStaticSky:
            return "DemoStaticSky";
        case VoxelResearchLightingPreset::OcclusionValidationLighting:
            return "OcclusionValidationLighting";
        default:
            return "Unknown";
        }
    }

    std::pair<uint32_t, uint32_t> ResolutionForPreset(const VoxelRenderResolutionPreset preset)
    {
        switch (preset)
        {
        case VoxelRenderResolutionPreset::R1280x720:
            return {1280, 720};
        case VoxelRenderResolutionPreset::R2560x1440:
            return {2560, 1440};
        case VoxelRenderResolutionPreset::R3840x2160:
            return {3840, 2160};
        case VoxelRenderResolutionPreset::R1920x1080:
        default:
            return {1920, 1080};
        }
    }

    const char* ResolutionPresetName(const VoxelRenderResolutionPreset preset)
    {
        switch (preset)
        {
        case VoxelRenderResolutionPreset::R1280x720:
            return "1280x720";
        case VoxelRenderResolutionPreset::R1920x1080:
            return "1920x1080";
        case VoxelRenderResolutionPreset::R2560x1440:
            return "2560x1440";
        case VoxelRenderResolutionPreset::R3840x2160:
            return "3840x2160";
        default:
            return "Unknown";
        }
    }

    const char* BenchmarkConfigClassName(const VoxelBenchmarkConfigClass configClass)
    {
        switch (configClass)
        {
        case VoxelBenchmarkConfigClass::ValidMatchingBenchmark:
            return "Valid matching benchmark configuration";
        case VoxelBenchmarkConfigClass::Diagnostic:
            return "Diagnostic configuration";
        case VoxelBenchmarkConfigClass::InvalidMixedQuality:
            return "Invalid mixed-quality configuration";
        default:
            return "Invalid mixed-quality configuration";
        }
    }

    std::pair<VoxelBenchmarkConfigClass, std::string> ClassifyBenchmarkConfig(
        const VoxelSceneWorkload& workload,
        const VoxelExecutionMode requestedMode)
    {
        const bool requestedTemporal = UsesTemporalExecution(requestedMode);
        const bool workloadTemporal = workload.TemporalPolicy == VoxelTemporalPolicy::Decimated;
        if (requestedTemporal != workloadTemporal)
            return {
                VoxelBenchmarkConfigClass::InvalidMixedQuality,
                "Requested execution mode temporal policy does not match workload temporal policy"
            };

        if (workload.BenchmarkConfigClass == VoxelBenchmarkConfigClass::Diagnostic)
            return {VoxelBenchmarkConfigClass::Diagnostic, workload.BenchmarkConfigReason};

        if (workload.Profile == VoxelResearchWorkloadProfile::OcclusionValidation ||
            workload.CameraMode == VoxelResearchCameraMode::Interactive ||
            workload.PartitionStrategy == VoxelPartitionStrategy::SpatialPlane ||
            workload.SpatialLod.FreezeCamera ||
            workload.SecondaryShare <= 0.0f ||
            workload.SecondaryShare >= 1.0f ||
            workload.StaticStorageMode == StaticVoxelStorageMode::DenseSolidStress)
        {
            return {
                VoxelBenchmarkConfigClass::Diagnostic,
                "Diagnostic partition, edge-share, dense-solid, or occlusion validation configuration"
            };
        }

        return {VoxelBenchmarkConfigClass::ValidMatchingBenchmark, ""};
    }

    VoxelAdapterOwner AdapterOwnerForPartition(const VoxelExecutionMode mode,
                                               const bool multiGpuAvailable,
                                               const VoxelAdapterPartition& partition)
    {
        return UsesMultiGpuExecution(mode) &&
                   multiGpuAvailable &&
                   partition.PartitionId == VoxelAdapterPartitionId::SecondaryPartition
                   ? VoxelAdapterOwner::Secondary
                   : VoxelAdapterOwner::Primary;
    }

    std::shared_ptr<PEPEngine::Graphics::GDevice> DeviceForAdapterOwner(
        const VoxelAdapterOwner owner,
        const std::shared_ptr<PEPEngine::Graphics::GDevice>& primaryDevice,
        const std::shared_ptr<PEPEngine::Graphics::GDevice>& secondaryDevice)
    {
        return owner == VoxelAdapterOwner::Secondary ? secondaryDevice : primaryDevice;
    }

    bool SameAdapterLuid(const LUID& left, const LUID& right)
    {
        return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    std::wstring ResolveVoxelWaterfallAssetPathW(const wchar_t* relativePath)
    {
        return ResolveVoxelWaterfallAssetPath(relativePath).wstring();
    }

    std::string ResolveVoxelWaterfallAssetPathA(const char* relativePath)
    {
        return ResolveVoxelWaterfallAssetPath(relativePath).string();
    }

}

VoxelWaterfallApp::VoxelWaterfallApp(const HINSTANCE hInstance) : D3DApp(hInstance)
{
    mSceneBounds.Center = Vector3(0.0f, 0.0f, 0.0f);
    mSceneBounds.Radius = 200;
}

VoxelWaterfallApp::~VoxelWaterfallApp()
{
    if (imguiInitialized)
    {
        Flush();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
}

void VoxelWaterfallApp::Update(const GameTimer& gt)
{
    cpuFrameStart = std::chrono::steady_clock::now();
    currentPrimaryWaitMs = 0.0;
    currentFrameResourceReady = true;

    const auto commandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);

    currentFrameResource = frameResources[currentFrameResourceIndex];

    if (currentFrameResource->PrimeRenderFenceValue != 0 && !commandQueue->IsFinish(
        currentFrameResource->PrimeRenderFenceValue))
    {
        currentFrameResourceReady = false;
        return;
    }

    if (researchCameraController)
    {
        researchCameraController->SetMode(voxelWorkload.CameraMode);
        researchCameraController->SetInputBlocked(voxelWorkload.CameraMode != VoxelResearchCameraMode::Interactive ||
                                                  benchmarkProfiler.IsActive() ||
                                                  benchmarkController.IsAutomaticActive());
        researchCameraController->SetDeterministicTime(static_cast<double>(simulationFrameIndex) / 60.0);
    }

    for (auto& e : gameObjects)
    {
        e->Update();
    }

    UpdateMaterials();

    UpdateShadowTransform(gt);
    UpdateMainPassCB(gt);
    UpdateShadowPassCB(gt);
    UpdateSsaoCB(gt);
}

void VoxelWaterfallApp::Draw(const GameTimer& gt)
{
    if (isResizing) return;
    if (!currentFrameResourceReady) return;

    ApplyPendingVoxelSettings();
    auto benchmarkContext = BuildBenchmarkControllerContext();
    benchmarkController.UpdateAutomatic(benchmarkContext);
    const bool benchmarkWasActive = benchmarkProfiler.IsActive();
    benchmarkProfiler.BeginFrame(BuildBenchmarkMetadata());

    const UINT timestampHeapIndex = 2 * currentFrameResourceIndex;

    const auto primaryComputeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto secondaryComputeQueue = secondDevice
                                           ? secondDevice->GetCommandQueue(GQueueType::Compute)
                                           : primaryComputeQueue;
    const auto primaryCopyQueue = primeDevice->GetCommandQueue(GQueueType::Copy);
    const auto secondaryCopyQueue = secondDevice
                                        ? secondDevice->GetCommandQueue(GQueueType::Copy)
                                        : primaryCopyQueue;
    const auto secondaryGraphicsQueue = secondDevice
                                            ? secondDevice->GetCommandQueue(GQueueType::Graphics)
                                            : primeDevice->GetCommandQueue(GQueueType::Graphics);
    auto renderQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);

    if (lastPrimaryPartitionGraphicsFenceValue != 0)
    {
        primaryComputeQueue->Wait(renderQueue->GetFence(), lastPrimaryPartitionGraphicsFenceValue);
    }
    if (lastSecondaryPartitionGraphicsFenceValue != 0)
    {
        const auto secondaryFenceQueue =
            lastSecondaryPartitionGraphicsFenceOwner == VoxelAdapterOwner::Secondary
                ? secondaryGraphicsQueue
                : renderQueue;
        const auto secondaryComputeWaitQueue =
            lastSecondaryPartitionGraphicsFenceOwner == VoxelAdapterOwner::Secondary
                ? secondaryComputeQueue
                : primaryComputeQueue;
        secondaryComputeWaitQueue->Wait(secondaryFenceQueue->GetFence(), lastSecondaryPartitionGraphicsFenceValue);
    }

    VoxelSimulationSchedulerContext schedulerContext{
        voxelWorkload,
        executionMode,
        multiGpuAvailable,
        simulationFrameIndex,
        timestampHeapIndex,
        gt.DeltaTime(),
        voxelSimulationAccumulator,
        voxelSimulationTime,
        voxelSimulationStepsThisFrame,
        voxelInterpolationAlpha,
        voxelRecycledCount,
        voxelAliveCount,
        voxelExpectedCount,
        primaryComputeQueue,
        secondaryComputeQueue,
        primaryComputeQueueFenceValue,
        secondaryComputeQueueFenceValue,
        currentFrameResource->ComputeFenceValue,
        benchmarkProfiler
    };
    const auto simulationResult = voxelScheduler.DispatchFrame(schedulerContext);
    currentFrameResource->PrimaryComputeFenceValue = simulationResult.PrimaryComputeFenceValue;
    currentFrameResource->SecondaryComputeFenceValue = simulationResult.SecondaryComputeFenceValue;

    frameGraphTelemetry = {};
    frameGraphTelemetry.FrameResourceIndex = currentFrameResourceIndex;
    frameGraphTelemetry.RequestedMode = requestedExecutionMode;
    frameGraphTelemetry.ActualMode = simulationResult.UsedMultiGpuMode
                                          ? executionMode
                                          : (executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation
                                                 ? VoxelExecutionMode::SingleGpuTemporalDecimation
                                                 : VoxelExecutionMode::SingleGpuFull);
    frameGraphTelemetry.PrimaryComputeSubmitted = simulationResult.PrimaryComputeSubmitted;
    frameGraphTelemetry.SecondaryComputeSubmitted = simulationResult.SecondaryComputeSubmitted;
    frameGraphTelemetry.SecondaryConfiguredUpdateInterval =
        simulationResult.SecondaryConfiguredUpdateInterval;
    frameGraphTelemetry.SecondaryEffectiveUpdateInterval =
        simulationResult.SecondaryEffectiveUpdateInterval;
    frameGraphTelemetry.FixedSimulationStepIndex = simulationResult.FixedSimulationStepIndex;
    frameGraphTelemetry.SecondarySimulationDispatchedThisFrame =
        simulationResult.SecondaryWorkThisFrame;
    frameGraphTelemetry.SecondaryStepsSinceLastUpdate =
        simulationResult.SecondaryStepsSinceLastUpdate;
    frameGraphTelemetry.SecondaryInterpolationPhase =
        simulationResult.SecondaryInterpolationPhase;
    frameGraphTelemetry.SecondaryCoarseDeltaTime =
        simulationResult.SecondaryCoarseDeltaTime;
    frameGraphTelemetry.PrimaryComputeFenceValue = simulationResult.PrimaryComputeFenceValue;
    frameGraphTelemetry.SecondaryComputeFenceValue = simulationResult.SecondaryComputeFenceValue;
    frameGraphTelemetry.ParticleTransferBytes = 0;

    benchmarkProfiler.UpdateCurrentFrameMetadata(BuildBenchmarkMetadata());

    if (!voxelWorkload.SpatialLod.FreezeCamera || !spatialLodCameraInitialized)
    {
        spatialLodCameraPosition = mainPassCB.EyePosW;
        spatialLodCameraInitialized = true;
    }
    if (voxelCompositeDebugView == VoxelCompositeDebugView::SpatialLodColors)
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::LodLevel;
    else if (voxelCompositeDebugView == VoxelCompositeDebugView::PartitionOwnershipColors)
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::AdapterOwnership;
    for (auto& partition : voxelWorkload.Partitions)
    {
        if (partition.GpuPartition)
            partition.GpuPartition->ConfigureSpatialLod(voxelWorkload.SpatialLod, spatialLodCameraPosition);
    }

    const auto voxelRenderWorkload = BuildVoxelRenderWorkload();
    ValidateVoxelRenderWorkload(voxelRenderWorkload);
    std::vector<VoxelPartitionRenderResult> primaryVoxelRenderResults;
    std::vector<VoxelPartitionRenderResult> secondaryVoxelRenderResults;
    const bool hasSecondaryVoxelDrawWork = std::any_of(
        voxelRenderWorkload.SecondaryOwnedPartitions.begin(),
        voxelRenderWorkload.SecondaryOwnedPartitions.end(),
        [](const VoxelAdapterPartition* partition)
        {
            return partition && partition->GpuPartition && partition->VoxelCount() > 0;
        });

    PrimaryBasePassContext primaryBaseContext{
        renderQueue,
        primaryComputeQueue->GetFence(),
        simulationResult.PrimaryComputeFenceValue,
        timestampHeapIndex,
        *currentFrameResource,
        benchmarkProfiler,
        graphicsPassFenceValue,
        &frameGraphTelemetry,
        &voxelRenderWorkload,
        &primaryVoxelRenderResults,
        [this, &voxelRenderWorkload, &primaryVoxelRenderResults](const std::shared_ptr<GCommandList>& cmdList)
        {
            VoxelRenderPassContext basePassContext{
                primeDeviceSignature,
                ssaoPrimeRootSignature,
                srvTexturesMemory,
                *currentFrameResource,
                fullViewport,
                fullRect,
                *shadowPath,
                *ambientPrimePath,
                *antiAliasingPrimePath,
                defaultPrimePipelineResources,
                typedRenderer,
                &voxelRenderWorkload.PrimaryOwnedPartitions,
                &primaryVoxelRenderResults,
                &benchmarkProfiler,
                MainWindow->GetCurrentBackBuffer()
            };
            basePassContext.DynamicShadowsEnabled = voxelWorkload.DynamicShadowsEnabled;
            voxelRenderPasses.RecordPrimaryBase(cmdList, basePassContext);
        }
    };
    renderPipeline.SubmitPrimaryBasePass(primaryBaseContext);
    for (const auto& result : primaryVoxelRenderResults)
    {
        frameGraphTelemetry.PrimarySpatialLodStats.Lod0Rendered += result.LodStats.Lod0Rendered;
        frameGraphTelemetry.PrimarySpatialLodStats.Lod1Rendered += result.LodStats.Lod1Rendered;
        frameGraphTelemetry.PrimarySpatialLodStats.Lod2Rendered += result.LodStats.Lod2Rendered;
        frameGraphTelemetry.PrimarySpatialLodStats.Aggregated += result.LodStats.Aggregated;
        if (result.UsedIndirectDraw)
            frameGraphTelemetry.PrimaryIndirectDrawCalls += result.DrawCallCount;
    }
    lastPrimaryPartitionGraphicsFenceValue = currentFrameResource->PrimaryBaseRenderFenceValue;
    if (!hasSecondaryVoxelDrawWork)
    {
        lastSecondaryPartitionGraphicsFenceValue = currentFrameResource->PrimaryBaseRenderFenceValue;
        lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Primary;
    }
    const bool runSecondaryGraphics =
        simulationResult.UsedMultiGpuMode &&
        multiGpuVoxelRenderTargets.IsInitialized() &&
        hasSecondaryVoxelDrawWork;
    bool secondaryImageReadyThisFrame = false;

    if (runSecondaryGraphics)
    {
        auto& secondaryFrameTargets = multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex];
        const auto secondaryDesc = secondaryFrameTargets.SecondaryLocalColor.GetD3D12ResourceDesc();
        const D3D12_VIEWPORT secondaryViewport{
            0.0f,
            0.0f,
            static_cast<float>(secondaryDesc.Width),
            static_cast<float>(secondaryDesc.Height),
            0.0f,
            1.0f
        };
        const D3D12_RECT secondaryScissor{
            0,
            0,
            static_cast<LONG>(secondaryDesc.Width),
            static_cast<LONG>(secondaryDesc.Height)
        };

        SecondaryVoxelGraphicsPassContext secondaryGraphicsContext{
            secondaryGraphicsQueue,
            secondaryComputeQueue->GetFence(),
            simulationResult.SecondaryComputeFenceValue,
            timestampHeapIndex,
            *currentFrameResource,
            voxelRenderWorkload.SecondaryOwnedPartitions,
            secondaryFrameTargets,
            secondaryViewport,
            secondaryScissor,
            benchmarkProfiler,
            &frameGraphTelemetry,
            &secondaryVoxelRenderResults
        };
        renderPipeline.SubmitSecondaryVoxelPass(secondaryGraphicsContext);
        lastSecondaryPartitionGraphicsFenceValue = currentFrameResource->SecondaryRenderFenceValue;
        lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Secondary;

        SecondaryLocalToSharedCopyPassContext secondaryCopyContext{
            secondaryCopyQueue,
            secondaryGraphicsQueue->GetFence(),
            currentFrameResource->SecondaryRenderFenceValue,
            secondCrossAdapterRenderReadyFence,
            crossAdapterRenderReadyFenceValue,
            timestampHeapIndex,
            *currentFrameResource,
            secondaryFrameTargets,
            benchmarkProfiler,
            &frameGraphTelemetry
        };
        renderPipeline.SubmitSecondaryLocalToSharedCopyPass(secondaryCopyContext);

        PrimarySharedToLocalCopyPassContext primaryCopyContext{
            primaryCopyQueue,
            primeCrossAdapterRenderReadyFence,
            currentFrameResource->CrossAdapterRenderReadyFenceValue,
            timestampHeapIndex,
            *currentFrameResource,
            secondaryFrameTargets,
            benchmarkProfiler,
            &frameGraphTelemetry
        };
        renderPipeline.SubmitPrimarySharedToLocalCopyPass(primaryCopyContext);
        secondaryFrameTargets.HasReceivedImage = true;
        secondaryImageReadyThisFrame = true;
    }

    const bool shouldComposeSecondaryImage =
        runSecondaryGraphics &&
        multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex].HasReceivedImage &&
        voxelCompositePass.IsInitialized();
    frameGraphTelemetry.CompositeSubmitted = shouldComposeSecondaryImage;
    frameGraphTelemetry.CompositeUsedSecondaryImage = shouldComposeSecondaryImage;
    frameGraphTelemetry.CompositeDebugView = voxelCompositeDebugView;

    FinalCompositeAndPresentPassContext finalPassContext{
        renderQueue,
        renderQueue->GetFence(),
        currentFrameResource->PrimaryBaseRenderFenceValue,
        primaryCopyQueue->GetFence(),
        currentFrameResource->PrimarySecondaryImageReadyFenceValue,
        secondaryImageReadyThisFrame,
        timestampHeapIndex,
        *currentFrameResource,
        graphicsPassFenceValue,
        benchmarkProfiler,
        &frameGraphTelemetry,
        [this, shouldComposeSecondaryImage](const std::shared_ptr<GCommandList>& cmdList)
        {
            VoxelRenderPassContext finalPassContext{
                primeDeviceSignature,
                ssaoPrimeRootSignature,
                srvTexturesMemory,
                *currentFrameResource,
                fullViewport,
                fullRect,
                *shadowPath,
                *ambientPrimePath,
                *antiAliasingPrimePath,
                defaultPrimePipelineResources,
                typedRenderer,
                nullptr,
                nullptr,
                nullptr,
                MainWindow->GetCurrentBackBuffer()
            };
            if (shouldComposeSecondaryImage)
            {
                auto& targets = multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex];
                benchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                             VoxelBenchmarkProfiler::RangeId::Composite);
                VoxelCompositePassContext compositeContext{
                    antiAliasingPrimePath->GetRenderTarget(),
                    antiAliasingPrimePath->GetDepthMap(),
                    targets,
                    antiAliasingPrimePath->GetViewPort(),
                    antiAliasingPrimePath->GetRect(),
                    mainPassCB.NearZ,
                    mainPassCB.FarZ,
                    voxelCompositeDebugView
                };
                voxelCompositePass.Record(cmdList, compositeContext);
                benchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                           VoxelBenchmarkProfiler::RangeId::Composite);
                benchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                               VoxelBenchmarkProfiler::RangeId::Composite);
                finalPassContext.ResolveSourceSrv = &targets.PrimaryCompositeDescriptors;
                finalPassContext.ResolveSourceSrvOffset = 4;
            }
            benchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                         VoxelBenchmarkProfiler::RangeId::FinalResolveUi);
            voxelRenderPasses.RecordFinalPresent(cmdList, finalPassContext);
            if (!benchmarkController.IsAutomaticActive())
                DrawUserInterface(cmdList);
            cmdList->TransitionBarrier(MainWindow->GetCurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT);
            cmdList->FlushResourceBarriers();
            benchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                       VoxelBenchmarkProfiler::RangeId::FinalResolveUi);
            benchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                           VoxelBenchmarkProfiler::RangeId::FinalResolveUi);
        }
    };
    renderPipeline.SubmitFinalCompositeAndPresentPass(finalPassContext);
    ValidateVoxelFrameDrawResults(
        voxelRenderWorkload,
        primaryVoxelRenderResults,
        secondaryVoxelRenderResults,
        frameGraphTelemetry.SecondaryGraphicsSubmitted);
    frameGraphTelemetry.VisualValidationHasResult = visualValidationMetrics.HasResult;
    frameGraphTelemetry.VisualValidationPassed =
        visualValidationMetrics.HasResult && visualValidationMetrics.Passed;
    frameGraphTelemetry.VisualValidationColorMAE = visualValidationMetrics.ColorMAE;
    frameGraphTelemetry.VisualValidationColorRMSE = visualValidationMetrics.ColorRMSE;
    frameGraphTelemetry.VisualValidationPSNR = visualValidationMetrics.ColorPSNR;
    frameGraphTelemetry.VisualValidationMaxError = visualValidationMetrics.MaxColorError;
    frameGraphTelemetry.VisualValidationMismatchedPixelPercent =
        visualValidationMetrics.ColorMismatchPercent;
    frameGraphTelemetry.VisualValidationDepthRMSE = visualValidationMetrics.DepthRMSE;
    frameGraphTelemetry.VisualValidationDepthMismatchPercent =
        visualValidationMetrics.DepthMismatchPercent;
    frameGraphTelemetry.VisualValidationPipelinePrimitiveCount =
        visualValidationMetrics.PipelinePrimitiveCount;
    frameGraphTelemetry.VisualValidationFailReason = visualValidationMetrics.FailReason;

    currentFrameResourceIndex = MainWindow->Present();
    benchmarkProfiler.UpdateCurrentFrameMetadata(BuildBenchmarkMetadata());
    benchmarkProfiler.EndFrameCpu();
    benchmarkProfiler.ProcessCompletedFrames();
    benchmarkController.RestoreVSyncAfterManualCompletion(benchmarkContext, benchmarkWasActive);
}

bool VoxelWaterfallApp::Initialize()
{
    InitDevices();
    const auto primaryComputeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto primaryGraphicsQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);
    const auto primaryCopyQueue = primeDevice->GetCommandQueue(GQueueType::Copy);
    const auto secondaryComputeQueue = secondDevice
                                           ? secondDevice->GetCommandQueue(GQueueType::Compute)
                                           : primaryComputeQueue;
    const auto secondaryGraphicsQueue = secondDevice
                                            ? secondDevice->GetCommandQueue(GQueueType::Graphics)
                                            : primaryGraphicsQueue;
    const auto secondaryCopyQueue = secondDevice
                                        ? secondDevice->GetCommandQueue(GQueueType::Copy)
                                        : primaryCopyQueue;
    benchmarkProfiler.Initialize(primeDevice, secondDevice,
                                 primaryComputeQueue,
                                 primaryGraphicsQueue,
                                 secondaryComputeQueue,
                                 secondaryGraphicsQueue,
                                 secondaryCopyQueue,
                                 primaryCopyQueue);
    InitMainWindow();
    Flush();

    LoadStudyTexture();
    Flush();
    LoadModels();
    Flush();
    CreateMaterials();
    Flush();
    GenerateMipMaps();
    Flush();

    InitRenderPaths();
    Flush();
    InitSRVMemoryAndMaterials();
    Flush();
    InitRootSignature();
    Flush();
    InitPipeLineResource();
    Flush();
    CreateGO();
    Flush();
    SortGO();
    Flush();
    InitFrameResource();
    Flush();

    OnResize();
    InitUserInterface();

    Flush();

    return true;
}

void VoxelWaterfallApp::InitDevices()
{
    auto allDevices = GDeviceFactory::GetAllDevices(false);
    const auto selectedDevices = DeviceSelectionPolicy::Select(allDevices);
    adapterReportLines.clear();
    for (const auto& adapter : selectedDevices.Adapters)
    {
        adapterReportLines.push_back(
            adapter.Name +
            L" | hardware=" + std::to_wstring(adapter.Hardware) +
            L" graphics=" + std::to_wstring(adapter.GraphicsQueue) +
            L" compute=" + std::to_wstring(adapter.ComputeQueue) +
            L" copy=" + std::to_wstring(adapter.CopyQueue) +
            L" crossAdapterTexture=" + std::to_wstring(adapter.CrossAdapterTexture) +
            L" | " + adapter.Status);
    }

    primeDevice = selectedDevices.Primary ? selectedDevices.Primary :
                  (!allDevices.empty() ? allDevices.front() : nullptr);
    secondDevice = selectedDevices.Secondary;

    if (!primeDevice)
        throw std::runtime_error("No Direct3D 12 hardware adapter is available for MGPU-VoxelWaterfall");

    assets = std::make_shared<AssetsLoader>(primeDevice);


    for (int i = 0; i < static_cast<uint8_t>(RenderMode::Count); ++i)
    {
        typedRenderer.push_back(
            MemoryAllocator::CreateVector<std::shared_ptr<Renderer>>());
    }

    multiGpuAvailable = secondDevice != nullptr && secondDevice != primeDevice;
    if (multiGpuAvailable && (!primeDevice->IsCrossAdapterTextureSupported() || !secondDevice->IsCrossAdapterTextureSupported()))
    {
        DisableMultiGpu(L"MultiGpu unavailable: cross-adapter row-major texture path is not supported");
    }
    else if (multiGpuAvailable && !secondDevice->GetCommandQueue(GQueueType::Graphics))
    {
        DisableMultiGpu(L"MultiGpu unavailable: secondary adapter graphics queue is not available");
    }

    if (multiGpuAvailable)
    {
        const bool renderReadyFenceReady = primeDevice->TrySharedFence(
            primeCrossAdapterRenderReadyFence,
            secondDevice,
            secondCrossAdapterRenderReadyFence,
            crossAdapterRenderReadyFenceValue);
        if (!renderReadyFenceReady)
        {
            DisableMultiGpu(L"MultiGpu unavailable: shared cross-adapter fence creation/open failed");
        }
        else
        {
            multiGpuStatus = L"MultiGpu hardware available; render target validation pending";
        }
    }
    else
    {
        DisableMultiGpu(selectedDevices.MultiGpuUnavailableReason.empty()
                            ? L"MultiGpu unavailable: secondary hardware adapter was not found"
                            : selectedDevices.MultiGpuUnavailableReason);
    }

    logQueue.Push(L"\nPrime Device: " + (primeDevice->GetName()));
    logQueue.Push(
        L"\t\n Cross Adapter Texture Support: " + std::to_wstring(
            primeDevice->IsCrossAdapterTextureSupported()));
    if (multiGpuAvailable && secondDevice)
    {
        logQueue.Push(L"\nSecond Device: " + (secondDevice->GetName()));
        logQueue.Push(
            L"\t\n Cross Adapter Texture Support: " + std::to_wstring(
                secondDevice->IsCrossAdapterTextureSupported()));
    }
    else
    {
        logQueue.Push(L"\nSecond Device: unavailable; MultiGpu modes are disabled");
    }
}

void VoxelWaterfallApp::InitUserInterface()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.FontGlobalScale = DebugUiScale;
    ImGui::GetStyle().ScaleAllSizes(DebugUiScale);

    imguiSrvMemory = primeDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
    ImGui_ImplWin32_Init(MainWindow->GetWindowHandle());
    ImGui_ImplDX12_InitInfo dx12InitInfo{};
    dx12InitInfo.Device = primeDevice->GetDXDevice().Get();
    dx12InitInfo.CommandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics)->GetD3D12CommandQueue().Get();
    dx12InitInfo.NumFramesInFlight = globalCountFrameResources;
    dx12InitInfo.RTVFormat = GetSRGBFormat(BackBufferFormat);
    dx12InitInfo.DSVFormat = DepthStencilFormat;
    dx12InitInfo.SrvDescriptorHeap = imguiSrvMemory.GetDescriptorHeap()->GetDirectxHeap();
    dx12InitInfo.UserData = &imguiSrvMemory;
    dx12InitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                           D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        const auto descriptor = static_cast<GDescriptor*>(info->UserData);
        *outCpuHandle = descriptor->GetCPUHandle();
        *outGpuHandle = descriptor->GetGPUHandle();
    };
    dx12InitInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                          D3D12_CPU_DESCRIPTOR_HANDLE,
                                          D3D12_GPU_DESCRIPTOR_HANDLE)
    {
    };
    ImGui_ImplDX12_Init(&dx12InitInfo);
    imguiInitialized = true;
}

void VoxelWaterfallApp::DrawUserInterface(const std::shared_ptr<GCommandList>& cmdList)
{
    VoxelWaterfallDebugPanelContext context{
        imguiInitialized,
        cmdList,
        &imguiSrvMemory,
        voxelWorkload,
        requestedExecutionMode,
        executionMode,
        multiGpuAvailable,
        multiGpuStatus,
        primeDevice ? primeDevice->GetName() : L"unavailable",
        multiGpuAvailable && secondDevice ? secondDevice->GetName() : L"unavailable",
        &adapterReportLines,
        simulationFrameIndex,
        voxelSimulationAccumulator,
        voxelSimulationStepsThisFrame,
        voxelInterpolationAlpha,
        voxelRecycledCount,
        voxelAliveCount,
        voxelExpectedCount,
        &frameGraphTelemetry,
        voxelCompositeDebugView,
        benchmarkProfiler,
        benchmarkController.WasVSyncEnabled(),
        benchmarkController.IsAutomaticActive(),
        benchmarkController.HasAutomaticConfigs(),
        benchmarkController.GetAutomaticIndex(),
        benchmarkController.GetAutomaticCount(),
        benchmarkController.GetAutomaticSummaryPath(),
        [this](const VoxelResearchWorkloadProfile profile) { ApplyResearchWorkloadProfile(profile); },
        [this](const VoxelResearchCameraMode mode) { ApplyResearchCameraMode(mode); },
        [this](const VoxelResearchLightingPreset preset) { ApplyResearchLightingPreset(preset); },
        [this](const VoxelRenderResolutionPreset preset) { ApplyRenderResolutionPreset(preset); },
        [this](const VoxelExecutionMode mode) { ApplyExecutionMode(mode); },
        [this] { StartManualBenchmark(); },
        [this] { StopManualBenchmark(); },
        [this] { RunVisualValidation(); },
        [this] { StartAutomaticBenchmark(); },
        [this] { StopAutomaticBenchmark(); },
        [this] { RequestApplyVoxelWorkloadSettings(); }
    };
    debugPanel.Draw(context);
}

void VoxelWaterfallApp::StartManualBenchmark()
{
    benchmarkController.StartManual(BuildBenchmarkControllerContext());
}

void VoxelWaterfallApp::StopManualBenchmark()
{
    benchmarkController.StopManual(BuildBenchmarkControllerContext());
}

void VoxelWaterfallApp::RunVisualValidation()
{
    VoxelVisualValidationConfig config{};
    config.Snapshot.Seed = voxelWorkload.Parameters.Seed;
    config.Snapshot.TotalVoxelCount = voxelWorkload.TotalVoxelCount;
    config.Snapshot.SecondaryShare = voxelWorkload.SecondaryShare;
    config.Snapshot.FixedDeltaTime = 1.0 / 60.0;
    config.Snapshot.FixedStepCount = simulationFrameIndex;
    config.Snapshot.SpatialLod = voxelWorkload.SpatialLod;
    config.Snapshot.TemporalInterval = voxelWorkload.TemporalDecimationInterval;
    config.Cases = VoxelVisualValidationConfig::DefaultCases();
    if (antiAliasingPrimePath)
    {
        const auto desc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        config.Snapshot.RenderWidth = static_cast<uint32_t>(desc.Width);
        config.Snapshot.RenderHeight = desc.Height;
    }

    const auto outputDirectory = GetExecutableDirectory() / "VoxelValidation";
    try
    {
        visualValidationMetrics =
            visualValidationRunner.RunDeterministicSuite(config, outputDirectory);
        logQueue.Push(L"Visual validation exported to " +
                      visualValidationMetrics.CsvPath.wstring());
    }
    catch (const std::exception& ex)
    {
        visualValidationMetrics = {};
        visualValidationMetrics.HasResult = true;
        visualValidationMetrics.Passed = false;
        visualValidationMetrics.FailReason = ex.what();
        logQueue.Push(L"Visual validation failed to export results");
    }
}

void VoxelWaterfallApp::RequestApplyVoxelWorkloadSettings()
{
    voxelWorkloadSettingsPending = true;
}

void VoxelWaterfallApp::ApplyResearchWorkloadProfile(const VoxelResearchWorkloadProfile profile)
{
    VoxelResearchScenePreset preset = VoxelResearchScenePreset::MixedVoxelEnvironment;
    switch (profile)
    {
    case VoxelResearchWorkloadProfile::StaticRenderOnly:
        preset = VoxelResearchScenePreset::StaticVoxelEnvironment;
        break;
    case VoxelResearchWorkloadProfile::DynamicSimulationAndRender:
        preset = VoxelResearchScenePreset::DynamicWaterfall;
        break;
    case VoxelResearchWorkloadProfile::MixedStaticAndDynamic:
        preset = VoxelResearchScenePreset::MixedVoxelEnvironment;
        break;
    case VoxelResearchWorkloadProfile::OcclusionValidation:
        preset = VoxelResearchScenePreset::OcclusionValidation;
        break;
    case VoxelResearchWorkloadProfile::SpatialLodDemonstration:
        preset = VoxelResearchScenePreset::SpatialLodDemonstration;
        break;
    default:
        break;
    }

    if (!voxelResearchSceneManager.RequestPreset(preset))
        return;

    Flush();
    lights.clear();
    camera.reset();
    researchCameraController.reset();
    VoxelResearchSceneContext sceneContext{
        primeDevice,
        secondDevice,
        *assets,
        models,
        srvTexturesMemory,
        gameObjects,
        typedRenderer,
        voxelWorkload,
        AspectRatio()
    };
    voxelResearchSceneManager.RebuildScene(sceneContext);
    SortGO();
    RebuildGpuPartitionsForMode();
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    voxelExpectedCount = voxelWorkload.TotalVoxelCount;
    simulationFrameIndex = 0;
    spatialLodCameraInitialized = false;
    voxelWorkloadSettingsPending = false;
}

void VoxelWaterfallApp::ApplyResearchCameraMode(const VoxelResearchCameraMode mode)
{
    voxelWorkload.CameraMode = mode;
    voxelWorkload.CameraPath = CameraModeName(mode);
    if (researchCameraController)
    {
        researchCameraController->SetMode(mode);
        researchCameraController->SetInputBlocked(mode != VoxelResearchCameraMode::Interactive ||
                                                  benchmarkProfiler.IsActive() ||
                                                  benchmarkController.IsAutomaticActive());
    }
    spatialLodCameraInitialized = false;
}

void VoxelWaterfallApp::ApplyResearchLightingPreset(const VoxelResearchLightingPreset preset)
{
    voxelWorkload.LightingMode = preset;
    voxelWorkload.LightingPreset = LightingPresetName(preset);
    voxelWorkload.DynamicShadowsEnabled = false;
}

void VoxelWaterfallApp::ApplyRenderResolutionPreset(const VoxelRenderResolutionPreset preset)
{
    const auto [width, height] = ResolutionForPreset(preset);
    if (MainWindow->GetClientWidth() == static_cast<int>(width) &&
        MainWindow->GetClientHeight() == static_cast<int>(height))
    {
        voxelWorkload.ResolutionPreset = preset;
        voxelWorkload.RenderResolutionWidth = width;
        voxelWorkload.RenderResolutionHeight = height;
        return;
    }

    Flush();
    voxelWorkload.ResolutionPreset = preset;
    voxelWorkload.RenderResolutionWidth = width;
    voxelWorkload.RenderResolutionHeight = height;
    MainWindow->SetWidth(static_cast<int>(width));
    MainWindow->SetHeight(static_cast<int>(height));
    OnResize();
}

bool VoxelWaterfallApp::HandleDemoPresetHotkey(const WPARAM key)
{
    if (benchmarkController.IsAutomaticActive())
        return false;

    switch (key)
    {
    case VK_F1:
        ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::StaticRenderOnly);
        return true;
    case VK_F2:
        ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::DynamicSimulationAndRender);
        return true;
    case VK_F3:
        ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::MixedStaticAndDynamic);
        return true;
    case VK_F4:
        ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::OcclusionValidation);
        return true;
    case VK_F5:
        ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::SpatialLodDemonstration);
        return true;
    case VK_F6:
        voxelCompositeDebugView = VoxelCompositeDebugView::PartitionOwnershipColors;
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::AdapterOwnership;
        return true;
    case VK_F7:
        voxelCompositeDebugView = VoxelCompositeDebugView::SecondaryColorOnly;
        return true;
    case VK_F8:
        voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
        return true;
    default:
        return false;
    }
}

void VoxelWaterfallApp::ResetBenchmarkDeterministicState()
{
    if (voxelWorkload.CameraMode == VoxelResearchCameraMode::Interactive)
    {
        voxelWorkload.CameraMode = VoxelResearchCameraMode::FixedOverview;
        voxelWorkload.CameraPath = CameraModeName(voxelWorkload.CameraMode);
    }
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    simulationFrameIndex = 0;
    voxelWorkload.SpatialLod.FreezeCamera = false;
    spatialLodCameraInitialized = false;
    if (researchCameraController)
    {
        researchCameraController->SetMode(voxelWorkload.CameraMode);
        researchCameraController->SetInputBlocked(true);
        researchCameraController->SetDeterministicTime(0.0);
        researchCameraController->ResetRoute();
    }
}

std::string VoxelWaterfallApp::GetExecutionModeName() const
{
    return GetExecutionModeName(executionMode);
}

std::string VoxelWaterfallApp::GetExecutionModeName(const VoxelExecutionMode mode) const
{
    switch (mode)
    {
    case VoxelExecutionMode::SingleGpuFull:
        return "SingleGpuFull";
    case VoxelExecutionMode::MultiGpuFull:
        return "MultiGpuFull";
    case VoxelExecutionMode::SingleGpuTemporalDecimation:
        return "SingleGpuTemporalDecimation";
    case VoxelExecutionMode::MultiGpuTemporalDecimation:
        return "MultiGpuTemporalDecimation";
    }
    return "SingleGpuFull";
}

VoxelBenchmarkProfiler::FrameMetadata VoxelWaterfallApp::BuildBenchmarkMetadata() const
{
    VoxelBenchmarkProfiler::FrameMetadata metadata{};
    metadata.FrameIndex = simulationFrameIndex;
    metadata.ProfileName = ProfileName(voxelWorkload.Profile);
    metadata.ScenePreset = voxelWorkload.ScenePreset;
    metadata.RequestedMode = GetExecutionModeName(requestedExecutionMode);
    metadata.ActualMode = GetExecutionModeName(frameGraphTelemetry.ActualMode);
    metadata.TemporalPolicy = TemporalPolicyName(voxelWorkload.TemporalPolicy);
    metadata.SpatialLodPolicy =
        voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::ThreeLevel
            ? "ThreeLevel"
            : "Off";
    metadata.PartitionStrategy = PartitionStrategyName(voxelWorkload.PartitionStrategy);
    metadata.LoadBalanceScenario = LoadBalanceScenarioName(voxelWorkload.LoadBalanceScenario);
    metadata.CameraPath =
        researchCameraController ? researchCameraController->GetCameraPathName() : voxelWorkload.CameraPath;
    metadata.LightingPreset = LightingPresetName(voxelWorkload.LightingMode);
    metadata.RenderResolutionPreset = ResolutionPresetName(voxelWorkload.ResolutionPreset);
    metadata.CameraFovDegrees = camera ? camera->GetFov() : 0.0f;
    metadata.CameraNearPlane = camera ? camera->GetNearZ() : 0.0f;
    metadata.CameraFarPlane = camera ? camera->GetFarZ() : 0.0f;
    metadata.DynamicShadowsEnabled = voxelWorkload.DynamicShadowsEnabled;
    const auto [configClass, configReason] = ClassifyBenchmarkConfig(voxelWorkload, requestedExecutionMode);
    metadata.BenchmarkConfigClass = BenchmarkConfigClassName(configClass);
    metadata.BenchmarkConfigReason = configReason;
    if (metadata.RequestedMode != metadata.ActualMode)
        metadata.FallbackReason = "requested mode unavailable; actual mode selected by runtime capability checks";
    uint32_t primaryVoxelCount = 0;
    uint32_t secondaryVoxelCount = 0;
    uint32_t updatedVoxelCount = 0;
    uint32_t simulationDispatchCount = 0;
    for (const auto& partition : voxelWorkload.Partitions)
    {
        if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
            secondaryVoxelCount += partition.VoxelCount();
        else
            primaryVoxelCount += partition.VoxelCount();
        updatedVoxelCount += partition.UpdatedVoxelCount;
        if (partition.SimulationDispatchedThisFrame)
            ++simulationDispatchCount;
    }
    metadata.PrimaryPartitionVoxelCount = primaryVoxelCount;
    metadata.SecondaryPartitionVoxelCount = secondaryVoxelCount;
    metadata.TotalVoxelCount = voxelWorkload.TotalVoxelCount;
    metadata.ActualStaticVoxelCount = voxelWorkload.ActualStaticVoxelCount;
    metadata.ActualDynamicVoxelCount = voxelWorkload.ActualDynamicVoxelCount;
    metadata.StaticVoxelBudget = voxelWorkload.StaticVoxelBudget;
    metadata.DynamicVoxelBudget = voxelWorkload.DynamicVoxelBudget;
    metadata.VoxelSize = voxelWorkload.Parameters.VoxelSize;
    metadata.ChunkSizeX = voxelWorkload.ChunkSize.Width;
    metadata.ChunkSizeY = voxelWorkload.ChunkSize.Height;
    metadata.ChunkSizeZ = voxelWorkload.ChunkSize.Depth;
    metadata.UpdatedVoxelCount = updatedVoxelCount;
    metadata.SimulationStepsThisFrame = voxelSimulationStepsThisFrame;
    metadata.SimulationDispatchCount = simulationDispatchCount;
    metadata.Seed =
        voxelWorkload.ActualStaticVoxelCount > 0
            ? voxelWorkload.StaticGenerationSeed
            : voxelWorkload.Parameters.Seed;
    metadata.SecondaryShare = voxelWorkload.SecondaryShare;
    metadata.TemporalDecimationInterval = voxelWorkload.TemporalDecimationInterval;
    if (antiAliasingPrimePath)
    {
        const auto desc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        metadata.RenderWidth = static_cast<uint32_t>(desc.Width);
        metadata.RenderHeight = desc.Height;
    }
    metadata.PrimaryAdapterName = primeDevice ? primeDevice->GetName() : L"unavailable";
    metadata.SecondaryAdapterName = multiGpuAvailable && secondDevice
                                        ? secondDevice->GetName()
                                        : L"unavailable";
    FillAdapterMetadata(primeDevice,
                        metadata.PrimaryVendorId,
                        metadata.PrimaryDeviceId,
                        metadata.PrimaryDedicatedVideoMemory,
                        metadata.PrimaryAdapterLuid);
    FillAdapterMetadata(multiGpuAvailable ? secondDevice : nullptr,
                        metadata.SecondaryVendorId,
                        metadata.SecondaryDeviceId,
                        metadata.SecondaryDedicatedVideoMemory,
                        metadata.SecondaryAdapterLuid);
    metadata.OperatingSystem = "Windows";
#ifdef _DEBUG
    metadata.BuildConfiguration = "Debug";
#else
    metadata.BuildConfiguration = "Release";
#endif
    metadata.GitCommit = "unknown";
    metadata.D3D12DebugLayerEnabled = false;
    metadata.CpuWaitMs = currentPrimaryWaitMs;
    metadata.TotalCrossAdapterBytes = frameGraphTelemetry.TotalCrossAdapterBytes;
    metadata.ColorTransferBytes = frameGraphTelemetry.ColorBytesTransferred;
    metadata.DepthTransferBytes = frameGraphTelemetry.DepthBytesTransferred;
    metadata.ParticleTransferBytes = frameGraphTelemetry.ParticleTransferBytes;
    metadata.RenderOutputTransferBytes = frameGraphTelemetry.RenderOutputTransferBytes;
    metadata.SecondaryDrawCalls = frameGraphTelemetry.SecondaryDrawCalls;
    metadata.PrimaryRenderedVoxelCount = frameGraphTelemetry.PrimarySpatialLodStats.TotalRendered();
    metadata.SecondaryRenderedVoxelCount = frameGraphTelemetry.SecondarySpatialLodStats.TotalRendered();
    metadata.PrimarySubmittedVoxelCount = metadata.PrimaryRenderedVoxelCount;
    metadata.SecondarySubmittedVoxelCount = metadata.SecondaryRenderedVoxelCount;
    metadata.PrimaryLod0Count = frameGraphTelemetry.PrimarySpatialLodStats.Lod0Rendered;
    metadata.PrimaryLod1Count = frameGraphTelemetry.PrimarySpatialLodStats.Lod1Rendered;
    metadata.PrimaryLod2Count = frameGraphTelemetry.PrimarySpatialLodStats.Lod2Rendered;
    metadata.SecondaryLod0Count = frameGraphTelemetry.SecondarySpatialLodStats.Lod0Rendered;
    metadata.SecondaryLod1Count = frameGraphTelemetry.SecondarySpatialLodStats.Lod1Rendered;
    metadata.SecondaryLod2Count = frameGraphTelemetry.SecondarySpatialLodStats.Lod2Rendered;
    metadata.VisualValidationHasResult = frameGraphTelemetry.VisualValidationHasResult;
    metadata.VisualValidationPassed = frameGraphTelemetry.VisualValidationPassed;
    metadata.VisualValidationColorMAE = frameGraphTelemetry.VisualValidationColorMAE;
    metadata.VisualValidationColorRMSE = frameGraphTelemetry.VisualValidationColorRMSE;
    metadata.VisualValidationPSNR = frameGraphTelemetry.VisualValidationPSNR;
    metadata.VisualValidationMaxError = frameGraphTelemetry.VisualValidationMaxError;
    metadata.VisualValidationMismatchedPixelPercent =
        frameGraphTelemetry.VisualValidationMismatchedPixelPercent;
    metadata.VisualValidationDepthRMSE = frameGraphTelemetry.VisualValidationDepthRMSE;
    metadata.VisualValidationDepthMismatchPercent =
        frameGraphTelemetry.VisualValidationDepthMismatchPercent;
    metadata.VisualValidationPipelinePrimitiveCount =
        frameGraphTelemetry.VisualValidationPipelinePrimitiveCount;
    metadata.VisualValidationFailReason = frameGraphTelemetry.VisualValidationFailReason;
    metadata.CpuFrameStart = cpuFrameStart;
    return metadata;
}

BenchmarkControllerContext VoxelWaterfallApp::BuildBenchmarkControllerContext()
{
    return BenchmarkControllerContext{
        benchmarkProfiler,
        [this] { return BuildBenchmarkMetadata(); },
        [this](const std::wstring& message) { logQueue.Push(message); },
        [this] { return MainWindow->IsVSync(); },
        [this](const bool enabled) { MainWindow->SetVSync(enabled); },
        [this] { Flush(); },
        [this](const VoxelExecutionMode mode) { ApplyExecutionMode(mode); },
        [this](const int totalCount) { ApplyBenchmarkVoxelCount(totalCount); },
        [this](const float secondaryShare) { ApplyBenchmarkSecondaryShare(secondaryShare); },
        [this](const bool enabled) { ApplyBenchmarkSpatialLodEnabled(enabled); },
        [this](const uint32_t interval) { ApplyBenchmarkTemporalInterval(interval); },
        [this] { ResetBenchmarkDeterministicState(); },
        multiGpuAvailable
    };
}

void VoxelWaterfallApp::StartAutomaticBenchmark()
{
    benchmarkController.StartAutomatic(BuildBenchmarkControllerContext());
}

void VoxelWaterfallApp::StopAutomaticBenchmark()
{
    benchmarkController.StopAutomatic(BuildBenchmarkControllerContext());
}

void VoxelWaterfallApp::ApplyBenchmarkVoxelCount(const int totalCount)
{
    const auto activePreset = voxelResearchSceneManager.GetActivePreset();
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment)
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, totalCount));
    else if (activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment)
    {
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, totalCount));
        if (totalCount <= 100000)
            voxelWorkload.DynamicVoxelBudget = 25000;
        else if (totalCount <= 250000)
            voxelWorkload.DynamicVoxelBudget = 100000;
        else if (totalCount <= 500000)
            voxelWorkload.DynamicVoxelBudget = 250000;
        else
            voxelWorkload.DynamicVoxelBudget = 500000;
    }
    else if (activePreset == VoxelResearchScenePreset::DynamicWaterfall)
        voxelWorkload.DynamicVoxelBudget = static_cast<uint32_t>(std::max(0, totalCount));
    else if (activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, totalCount));
    else
        voxelWorkload.TotalVoxelCount = static_cast<uint32_t>(std::max(0, totalCount));
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();
}

void VoxelWaterfallApp::ApplyBenchmarkSecondaryShare(const float secondaryShare)
{
    voxelWorkload.SecondaryShare = std::clamp(secondaryShare, 0.0f, 1.0f);
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();
}

void VoxelWaterfallApp::ApplyBenchmarkSpatialLodEnabled(const bool enabled)
{
    voxelWorkload.SpatialLod.Mode = enabled
                                        ? VoxelSpatialLodMode::ThreeLevel
                                        : VoxelSpatialLodMode::Off;
    voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
    voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
}

void VoxelWaterfallApp::ApplyBenchmarkTemporalInterval(const uint32_t interval)
{
    voxelWorkload.TemporalPolicy = interval <= 1 ? VoxelTemporalPolicy::Full : VoxelTemporalPolicy::Decimated;
    voxelWorkload.TemporalDecimationInterval =
        voxelWorkload.TemporalPolicy == VoxelTemporalPolicy::Decimated
            ? std::clamp<uint32_t>(interval, 2, 16)
            : 1;
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();
}

void VoxelWaterfallApp::ApplyExecutionMode(const VoxelExecutionMode requestedMode)
{
    requestedExecutionMode = requestedMode;
    VoxelExecutionMode targetMode = requestedMode;
    const bool requestsMultiGpu = targetMode == VoxelExecutionMode::MultiGpuFull ||
        targetMode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    if (requestsMultiGpu && !multiGpuAvailable)
    {
        targetMode = targetMode == VoxelExecutionMode::MultiGpuFull
                         ? VoxelExecutionMode::SingleGpuFull
                         : VoxelExecutionMode::SingleGpuTemporalDecimation;
        if (multiGpuStatus.empty())
            multiGpuStatus = L"MultiGpu unavailable: secondary render target capability validation failed";
    }

    if (requestsMultiGpu && multiGpuAvailable && !multiGpuVoxelRenderTargets.IsInitialized())
    {
        RebuildMultiGpuVoxelRenderTargets();
        if (!multiGpuVoxelRenderTargets.IsInitialized())
        {
            targetMode = targetMode == VoxelExecutionMode::MultiGpuFull
                             ? VoxelExecutionMode::SingleGpuFull
                             : VoxelExecutionMode::SingleGpuTemporalDecimation;
        }
    }

    if (executionMode == targetMode)
        return;

    executionMode = targetMode;
    if (executionMode == VoxelExecutionMode::MultiGpuFull)
    {
        multiGpuStatus = L"MultiGpuFull adapter-local partitions active; secondary render-output transfer enabled";
    }
    else if (executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
    {
        multiGpuStatus = L"MultiGpuTemporalDecimation adapter-local partitions active; secondary render-output transfer enabled";
    }
    else if (executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
    {
        multiGpuStatus = L"SingleGpuTemporalDecimation active";
    }
    else
    {
        multiGpuStatus = L"SingleGpuFull active";
    }
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();
    logQueue.Push(L"\nVoxel execution mode changed: " + multiGpuStatus);
}

void VoxelWaterfallApp::ApplyPendingVoxelSettings()
{
    if (!voxelWorkloadSettingsPending)
        return;

    const StaticLayerSnapshot staticLayerBefore = CaptureStaticLayerSnapshot(voxelWorkload);

    Flush();
    const auto activePreset = voxelResearchSceneManager.GetActivePreset();
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment ||
        activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment ||
        activePreset == VoxelResearchScenePreset::OcclusionValidation ||
        activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
    {
        VoxelResearchEnvironmentGenerator::Settings settings{};
        settings.Seed = voxelWorkload.StaticGenerationSeed;
        settings.StaticVoxelBudget = voxelWorkload.StaticVoxelBudget;
        settings.BudgetPreset = voxelWorkload.StaticBudgetPreset;
        settings.StorageMode = voxelWorkload.StaticStorageMode;
        settings.VoxelSize = voxelWorkload.StaticVoxelSize;
        settings.SecondaryShare = voxelWorkload.SecondaryShare;
        settings.PartitionStrategy = voxelWorkload.PartitionStrategy;
        settings.LoadBalanceScenario = voxelWorkload.LoadBalanceScenario;
        settings.ChunkSize = voxelWorkload.ChunkSize;
        settings.SpatialLod = voxelWorkload.SpatialLod;
        auto generated = VoxelResearchEnvironmentGenerator::Generate(settings);
        voxelWorkload.Layers.clear();
        voxelWorkload.Layers.push_back(std::move(generated.Layer));
        voxelWorkload.StaticTelemetry = generated.Telemetry;
    }
    else
    {
        voxelWorkload.Layers.clear();
    }
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment ||
        activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        voxelWorkload.DynamicVoxelBudget = 0;
    voxelWorkload = VoxelSceneWorkloadBuilder::Build(voxelWorkload);
    AssertStaticLayerShapePreserved(staticLayerBefore, voxelWorkload);
    RebuildGpuPartitionsForMode();
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    voxelExpectedCount = voxelWorkload.TotalVoxelCount;
    simulationFrameIndex = 0;
    spatialLodCameraInitialized = false;
    voxelWorkloadSettingsPending = false;
}

void VoxelWaterfallApp::RebuildGpuPartitionsForMode()
{
    auto voxelObjectIt = std::find_if(
        gameObjects.begin(),
        gameObjects.end(),
        [](const std::shared_ptr<GameObject>& object)
        {
            return object && object->GetName() == VoxelSceneGpuObjectName;
        });

    if (voxelObjectIt == gameObjects.end())
    {
        assert(voxelWorkload.TotalVoxelCount == 0 &&
               "Dynamic voxel object must exist before rebuilding non-empty GPU partitions");
        for (auto& partition : voxelWorkload.Partitions)
        {
            partition.AdapterOwner = AdapterOwnerForPartition(executionMode, multiGpuAvailable, partition);
            partition.GpuPartition.reset();
        }
        return;
    }

    (*voxelObjectIt)->RemoveComponentsOfType<VoxelGpuPartition>();
    for (auto& partition : voxelWorkload.Partitions)
        partition.GpuPartition.reset();

    for (auto& partition : voxelWorkload.Partitions)
    {
        if (partition.VoxelCount() == 0)
            continue;

        partition.AdapterOwner = AdapterOwnerForPartition(executionMode, multiGpuAvailable, partition);
        const auto ownerDevice = DeviceForAdapterOwner(partition.AdapterOwner, primeDevice, secondDevice);
        assert(ownerDevice && "VoxelGpuPartition owner device must be valid");

        auto gpuPartition = std::make_shared<VoxelGpuPartition>(
            ownerDevice,
            partition.DrawStreams,
            partition.AdapterOwner);
        gpuPartition->SetSimulationEnabled(partition.HasDynamicVoxels() && partition.SimulationPolicy.Enabled);
        gpuPartition->SetRenderEnabled(partition.VoxelCount() > 0);
        (*voxelObjectIt)->AddComponent(gpuPartition);

        partition.GpuPartition = std::move(gpuPartition);
    }

    const auto modeNameUtf8 = GetExecutionModeName(executionMode);
    logQueue.Push(L"\nVoxel GPU partitions rebuilt for " +
        std::wstring(modeNameUtf8.begin(), modeNameUtf8.end()));
}

VoxelRenderWorkload VoxelWaterfallApp::BuildVoxelRenderWorkload() const
{
    VoxelRenderWorkload renderWorkload{};
    for (const auto& partition : voxelWorkload.Partitions)
    {
        if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
            renderWorkload.SecondaryOwnedPartitions.push_back(&partition);
        else
            renderWorkload.PrimaryOwnedPartitions.push_back(&partition);
    }
    return renderWorkload;
}

void VoxelWaterfallApp::ValidateVoxelRenderWorkload(const VoxelRenderWorkload& renderWorkload) const
{
    if (typedRenderer.size() > static_cast<size_t>(RenderMode::Particle))
    {
        for (const auto& renderer : typedRenderer[static_cast<int>(RenderMode::Particle)])
            assert(dynamic_cast<VoxelGpuPartition*>(renderer.get()) == nullptr &&
                   "VoxelGpuPartition must not be registered in the generic renderer registry");
    }

    std::unordered_set<const VoxelAdapterPartition*> primaryPartitions;
    for (const auto* partition : renderWorkload.PrimaryOwnedPartitions)
    {
        assert(partition != nullptr);
        assert(primaryPartitions.insert(partition).second && "Duplicate primary voxel partition draw entry");
        assert(partition->AdapterOwner == VoxelAdapterOwner::Primary);
    }

    for (const auto* partition : renderWorkload.SecondaryOwnedPartitions)
    {
        assert(partition != nullptr);
        assert(primaryPartitions.find(partition) == primaryPartitions.end() &&
               "Voxel partition is present in both primary and secondary draw lists");
        assert(partition->AdapterOwner == VoxelAdapterOwner::Secondary);
    }

    for (const auto& partition : voxelWorkload.Partitions)
    {
        const auto expectedOwner = AdapterOwnerForPartition(executionMode, multiGpuAvailable, partition);
        assert(partition.AdapterOwner == expectedOwner &&
               "Voxel partition adapter owner does not match current execution mode");
        assert(partition.VoxelCount() == 0 || partition.GpuPartition &&
               "Non-empty voxel partition must have adapter-local GPU object");
        if (!partition.GpuPartition)
            continue;
        assert(partition.GpuPartition->GetAdapterOwner() == partition.AdapterOwner);
        assert(partition.HasDynamicVoxels() ||
               !partition.GpuPartition->IsSimulationEnabled());

        const auto expectedDevice = DeviceForAdapterOwner(partition.AdapterOwner, primeDevice, secondDevice);
        const auto actualDevice = partition.GpuPartition->GetOwningDevice();
        assert(expectedDevice && actualDevice);
        assert(SameAdapterLuid(expectedDevice->GetDesc().AdapterLuid, actualDevice->GetDesc().AdapterLuid) &&
               "Voxel partition GPU object owner device does not match adapter owner");
    }

    if (executionMode == VoxelExecutionMode::MultiGpuFull ||
        executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
    {
        for (const auto* partition : renderWorkload.PrimaryOwnedPartitions)
            assert(partition->PartitionId != VoxelAdapterPartitionId::SecondaryPartition &&
                   "Multi-GPU primary draw list must not contain SecondaryPartition");
        for (const auto* partition : renderWorkload.SecondaryOwnedPartitions)
            assert(partition->PartitionId != VoxelAdapterPartitionId::PrimaryPartition &&
                   "Multi-GPU secondary draw list must not contain PrimaryPartition");
    }

    if ((executionMode == VoxelExecutionMode::SingleGpuFull ||
        executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
        && voxelWorkload.TotalVoxelCount > 0)
    {
        assert(renderWorkload.SecondaryOwnedPartitions.empty() &&
               "Single-GPU mode must not submit secondary graphics draw list");
        assert(renderWorkload.PrimaryOwnedPartitions.size() == voxelWorkload.Partitions.size() &&
               "Single-GPU primary draw list must contain both logical voxel partitions");
    }
}

void VoxelWaterfallApp::ValidateVoxelFrameDrawResults(
    const VoxelRenderWorkload& renderWorkload,
    const std::vector<VoxelPartitionRenderResult>& primaryResults,
    const std::vector<VoxelPartitionRenderResult>& secondaryResults,
    const bool secondaryGraphicsSubmitted) const
{
    std::unordered_set<VoxelGlobalId> drawListGlobalIds;
    std::unordered_set<const VoxelAdapterPartition*> partitionPointers;

    auto addDrawList = [&](const std::vector<const VoxelAdapterPartition*>& partitions)
    {
        for (const auto* partition : partitions)
        {
            assert(partition != nullptr);
            assert(partitionPointers.insert(partition).second &&
                   "A voxel partition was assigned to more than one frame draw list");
            for (const VoxelGlobalId globalVoxelId : partition->GlobalVoxelIds)
            {
                assert(drawListGlobalIds.insert(globalVoxelId).second &&
                       "GlobalVoxelId appears in more than one frame draw list");
            }
        }
    };

    addDrawList(renderWorkload.PrimaryOwnedPartitions);
    addDrawList(renderWorkload.SecondaryOwnedPartitions);
    assert(drawListGlobalIds.size() == voxelWorkload.TotalVoxelCount &&
           "Frame voxel draw lists do not cover the logical workload exactly once");

    uint32_t submittedVoxelCount = 0;
    uint32_t secondaryDrawCallCount = 0;
    std::unordered_set<uint32_t> primaryRecordedPartitions;
    std::unordered_set<uint32_t> secondaryRecordedPartitions;

    for (const auto& result : primaryResults)
    {
        submittedVoxelCount += result.SubmittedVoxelCount;
        primaryRecordedPartitions.insert(static_cast<uint32_t>(result.PartitionId));
    }

    for (const auto& result : secondaryResults)
    {
        submittedVoxelCount += result.SubmittedVoxelCount;
        secondaryDrawCallCount += result.DrawCallCount;
        secondaryRecordedPartitions.insert(static_cast<uint32_t>(result.PartitionId));
    }

    assert(submittedVoxelCount <= voxelWorkload.TotalVoxelCount &&
           "Submitted voxel draw count exceeds logical workload count");

    for (const auto partitionId : primaryRecordedPartitions)
    {
        assert(secondaryRecordedPartitions.find(partitionId) == secondaryRecordedPartitions.end() &&
               "A voxel partition was recorded in both primary and secondary command lists");
    }

    if (executionMode == VoxelExecutionMode::MultiGpuFull ||
        executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
    {
        for (const auto& result : primaryResults)
            assert(result.PartitionId != VoxelAdapterPartitionId::SecondaryPartition &&
                   "Multi-GPU primary command list recorded SecondaryPartition");
        for (const auto& result : secondaryResults)
            assert(result.PartitionId != VoxelAdapterPartitionId::PrimaryPartition &&
                   "Multi-GPU secondary command list recorded PrimaryPartition");

        const bool hasSecondaryLogicalVoxels = std::any_of(
            renderWorkload.SecondaryOwnedPartitions.begin(),
            renderWorkload.SecondaryOwnedPartitions.end(),
            [](const VoxelAdapterPartition* partition)
            {
                return partition && partition->VoxelCount() > 0;
            });
        if (hasSecondaryLogicalVoxels && simulationFrameIndex > 0)
        {
            assert(secondaryGraphicsSubmitted &&
                   "Multi-GPU secondary graphics queue did not receive a command list");
            assert(secondaryDrawCallCount > 0 &&
                   "Multi-GPU secondary graphics command list did not issue a draw command");
        }
    }

    if (executionMode == VoxelExecutionMode::SingleGpuFull ||
        executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
    {
        assert(!secondaryGraphicsSubmitted &&
               "Single-GPU mode must not submit secondary graphics queue");
        assert(secondaryResults.empty() &&
               "Single-GPU mode must not record secondary voxel draw results");
    }
}

void VoxelWaterfallApp::InitFrameResource()
{
    for (int i = 0; i < globalCountFrameResources; ++i)
    {
        frameResources.push_back(std::make_unique<FrameResource>(primeDevice,
                                                                  secondDevice ? secondDevice : primeDevice, 2,
                                                                  static_cast<UINT>(assets->GetMaterials().size())));
    }
    logQueue.Push(std::wstring(L"\nInit FrameResource "));
}

void VoxelWaterfallApp::InitRootSignature()
{
    auto rootSignature = std::make_shared<GRootSignature>();
    CD3DX12_DESCRIPTOR_RANGE texParam[4];
    texParam[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, StandardShaderSlot::SkyMap - 3, 0); //SkyMap
    texParam[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, StandardShaderSlot::ShadowMap - 3, 0); //ShadowMap
    texParam[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, StandardShaderSlot::AmbientMap - 3, 0); //SsaoMap
    texParam[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                     static_cast<UINT>(assets->GetLoadTexturesCount() > 0 ? assets->GetLoadTexturesCount() : 1),
                     StandardShaderSlot::TexturesMap - 3, 0);


    rootSignature->AddConstantBufferParameter(0);
    rootSignature->AddConstantBufferParameter(1);
    rootSignature->AddShaderResourceView(0, 1);
    rootSignature->AddDescriptorParameter(&texParam[0], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->AddDescriptorParameter(&texParam[1], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->AddDescriptorParameter(&texParam[2], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->AddDescriptorParameter(&texParam[3], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->Initialize(primeDevice);

    primeDeviceSignature = rootSignature;


    logQueue.Push(std::wstring(L"\nInit RootSignature for " + primeDevice->GetName()));

    ssaoPrimeRootSignature = std::make_shared<GRootSignature>();

    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0);

    ssaoPrimeRootSignature->AddConstantBufferParameter(0);
    ssaoPrimeRootSignature->AddConstantParameter(1, 1);
    ssaoPrimeRootSignature->AddDescriptorParameter(&texTable0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    ssaoPrimeRootSignature->AddDescriptorParameter(&texTable1, 1, D3D12_SHADER_VISIBILITY_PIXEL);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressW
        0.0f,
        0,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
    {
        pointClamp, linearClamp, depthMapSam, linearWrap
    };

    for (auto&& sampler : staticSamplers)
    {
        ssaoPrimeRootSignature->AddStaticSampler(sampler);
    }

    ssaoPrimeRootSignature->Initialize(primeDevice);
}

void VoxelWaterfallApp::InitPipeLineResource()
{
    defaultInputLayout =
    {
        {
            "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
    };

    const D3D12_INPUT_LAYOUT_DESC desc = {
        defaultInputLayout.data(), static_cast<UINT>(defaultInputLayout.size())
    };

    defaultPrimePipelineResources = RenderModeFactory();
    defaultPrimePipelineResources.LoadDefaultShaders();
    defaultPrimePipelineResources.LoadDefaultPSO(primeDevice, primeDeviceSignature, desc,
                                                 BackBufferFormat, DepthStencilFormat, ssaoPrimeRootSignature,
                                                 NormalMapFormat, AmbientMapFormat);

    ambientPrimePath->SetPipelineData(*defaultPrimePipelineResources.GetPSO(RenderMode::Ssao),
                                      *defaultPrimePipelineResources.GetPSO(RenderMode::SsaoBlur));

    voxelCompositePass.Initialize(primeDevice, GetSRGBFormat(BackBufferFormat));

    logQueue.Push(std::wstring(L"\nInit PSO for " + primeDevice->GetName()));

    const auto primeDeviceShadowMapPso = defaultPrimePipelineResources.GetPSO(RenderMode::ShadowMapOpaque);
}

void VoxelWaterfallApp::CreateMaterials()
{
    auto seamless = std::make_shared<Material>(L"seamless", RenderMode::Opaque);
    seamless->FresnelR0 = Vector3(0.02f, 0.02f, 0.02f);
    seamless->Roughness = 0.1f;

    auto tex = assets->GetTextureIndex(L"seamless");
    seamless->SetDiffuseTexture(assets->GetTexture(tex), tex);

    tex = assets->GetTextureIndex(L"defaultNormalMap");

    seamless->SetNormalMap(assets->GetTexture(tex), tex);
    assets->AddMaterial(seamless);

    auto matteOccluder = std::make_shared<Material>(L"matteOccluder", RenderMode::Opaque);
    matteOccluder->DiffuseAlbedo = Vector4(0.36f, 0.35f, 0.33f, 1.0f);
    matteOccluder->FresnelR0 = Vector3(0.01f, 0.01f, 0.01f);
    matteOccluder->Roughness = 0.9f;
    tex = assets->GetTextureIndex(L"seamless");
    matteOccluder->SetDiffuseTexture(assets->GetTexture(tex), tex);
    tex = assets->GetTextureIndex(L"defaultNormalMap");
    matteOccluder->SetNormalMap(assets->GetTexture(tex), tex);
    assets->AddMaterial(matteOccluder);

    models[L"quad"]->SetMeshMaterial(0, assets->GetMaterial(assets->GetMaterialIndex(L"seamless")));
    if (models.find(L"box") != models.end())
        models[L"box"]->SetMeshMaterial(0, assets->GetMaterial(assets->GetMaterialIndex(L"matteOccluder")));

    logQueue.Push(std::wstring(L"\nCreate Materials"));
}

void VoxelWaterfallApp::InitSRVMemoryAndMaterials()
{
    srvTexturesMemory =
        primeDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         static_cast<uint32_t>(assets->GetTextures().size()));

    auto materials = assets->GetMaterials();

    for (size_t j = 0; j < materials.size(); ++j)
    {
        auto material = materials[j];

        material->InitMaterial(&srvTexturesMemory);
    }

    logQueue.Push(std::wstring(L"\nInit Views for " + primeDevice->GetName()));
    ambientPrimePath->BuildDescriptors();
}

void VoxelWaterfallApp::InitRenderPaths()
{
    auto commandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);
    auto cmdList = commandQueue->GetCommandList();

    ambientPrimePath = (std::make_shared<SSAO>(
        primeDevice,
        cmdList,
        MainWindow->GetClientWidth(), MainWindow->GetClientHeight()));

    antiAliasingPrimePath = (std::make_shared<SSAA>(primeDevice, 4, MainWindow->GetClientWidth(),
                                                    MainWindow->GetClientHeight()));
    antiAliasingPrimePath->OnResize(MainWindow->GetClientWidth(), MainWindow->GetClientHeight());

    commandQueue->WaitForFenceValue(commandQueue->ExecuteCommandList(cmdList));

    logQueue.Push(std::wstring(L"\nInit Render path data for " + primeDevice->GetName()));

    shadowPath = (std::make_shared<ShadowMap>(primeDevice, 2048, 2048));
    RebuildMultiGpuVoxelRenderTargets();
}

MultiGpuVoxelRenderTargetDesc VoxelWaterfallApp::BuildMultiGpuVoxelRenderTargetDesc() const
{
    MultiGpuVoxelRenderTargetDesc desc{};
    desc.FrameCount = globalCountFrameResources;
    desc.ColorFormat = GetSRGBFormat(BackBufferFormat);
    desc.LinearDepthFormat = DXGI_FORMAT_R32_FLOAT;
    desc.DepthStencilFormat = DepthStencilFormat;

    if (antiAliasingPrimePath)
    {
        const auto renderTargetDesc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        desc.Width = static_cast<UINT>(renderTargetDesc.Width);
        desc.Height = renderTargetDesc.Height;
        desc.ColorFormat = renderTargetDesc.Format;
    }
    else if (MainWindow)
    {
        desc.Width = MainWindow->GetClientWidth();
        desc.Height = MainWindow->GetClientHeight();
    }

    return desc;
}

void VoxelWaterfallApp::RebuildMultiGpuVoxelRenderTargets()
{
    multiGpuVoxelRenderTargets.Reset();

    if (!multiGpuAvailable)
        return;

    const auto desc = BuildMultiGpuVoxelRenderTargetDesc();
    if (!multiGpuVoxelRenderTargets.Initialize(primeDevice, secondDevice, desc))
    {
        DisableMultiGpu(multiGpuVoxelRenderTargets.GetFailureMessage());
        return;
    }

    multiGpuStatus = L"MultiGpu hardware available; secondary voxel render targets allocated";
    logQueue.Push(L"\n" + multiGpuStatus);
    logQueue.Push(L"\nMultiGpu voxel render target frame sets: " +
        std::to_wstring(multiGpuVoxelRenderTargets.GetFrameCount()));
}

void VoxelWaterfallApp::DisableMultiGpu(const std::wstring& reason)
{
    multiGpuAvailable = false;
    executionMode = VoxelExecutionMode::SingleGpuFull;
    multiGpuStatus = reason.empty() ? L"MultiGpu unavailable: unknown capability failure" : reason;
    multiGpuVoxelRenderTargets.Reset();
    logQueue.Push(L"\n" + multiGpuStatus);
}

void VoxelWaterfallApp::LoadStudyTexture()
{
    auto queue = primeDevice->GetCommandQueue(GQueueType::Compute);

    const auto cmdList = queue->GetCommandList();

    for (const auto& entry : SampleAssetManifest::Textures())
    {
        auto texture = entry.IsNormalMap
                           ? GTexture::LoadTextureFromFile(ResolveVoxelWaterfallAssetPathW(entry.RelativePath.c_str()),
                                                           cmdList, TextureUsage::Normalmap)
                           : GTexture::LoadTextureFromFile(ResolveVoxelWaterfallAssetPathW(entry.RelativePath.c_str()),
                                                           cmdList);
        texture->SetName(entry.Name);
        assets->AddTexture(texture);
    }

    queue->WaitForFenceValue(queue->ExecuteCommandList(cmdList));

    logQueue.Push(std::wstring(L"\nLoad DDS Texture"));
}

void VoxelWaterfallApp::LoadModels()
{
    auto queue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto cmdList = queue->GetCommandList();

    for (const auto& entry : SampleAssetManifest::Models())
    {
        auto model = assets->CreateModelFromFile(cmdList, ResolveVoxelWaterfallAssetPathA(entry.RelativePath.c_str()));
        model->scaleMatrix = entry.Scale;
        models[entry.Name] = std::move(model);
    }

    auto quad = assets->GenerateQuad(cmdList, -15.0f, -15.0f, 30.0f, 30.0f, 0.0f);
    models[L"quad"] = std::move(quad);
    auto box = assets->GenerateBox(cmdList, 1.0f, 1.0f, 1.0f);
    models[L"box"] = std::move(box);

    queue->WaitForFenceValue(queue->ExecuteCommandList(cmdList));
    queue->Flush();

    logQueue.Push(std::wstring(L"\nLoad Models Data"));
}

void VoxelWaterfallApp::GenerateMipMaps()
{
    try
    {
        std::vector<GTexture*> generatedMipTextures;

        auto textures = assets->GetTextures();

        for (auto&& texture : textures)
        {
            texture->ClearTrack();

            if (texture->GetD3D12Resource()->GetDesc().Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                continue;

            if (!texture->HasMipMap)
            {
                generatedMipTextures.push_back(texture.get());
            }
        }

        const auto computeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
        auto computeList = computeQueue->GetCommandList();
        GTexture::GenerateMipMaps(computeList, generatedMipTextures.data(), generatedMipTextures.size());
        computeQueue->WaitForFenceValue(computeQueue->ExecuteCommandList(computeList));
        logQueue.Push(std::wstring(L"\nMip Map Generation for " + primeDevice->GetName()));

        computeList = computeQueue->GetCommandList();
        for (auto&& texture : generatedMipTextures)
            computeList->TransitionBarrier(texture->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
        computeList->FlushResourceBarriers();
        logQueue.Push(std::wstring(L"\nTexture Barrier Generation for " + primeDevice->GetName()));
        computeQueue->WaitForFenceValue(computeQueue->ExecuteCommandList(computeList));

        logQueue.Push(std::wstring(L"\nMipMap Generation cmd list executing " + primeDevice->GetName()));
        for (auto&& pair : textures)
            pair->ClearTrack();
        logQueue.Push(std::wstring(L"\nFinish Mip Map Generation for " + primeDevice->GetName()));
    }
    catch (DxException& e)
    {
        logQueue.Push(L"\n" + e.Filename + L" " + e.FunctionName + L" " + std::to_wstring(e.LineNumber));
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
    }
    catch (...)
    {
        logQueue.Push(L"\nUnexpected error during mip-map generation");
    }
}

void VoxelWaterfallApp::SortGO()
{
    researchCameraController.reset();
    for (auto&& item : gameObjects)
    {
        auto light = item->GetComponent<Light>();
        if (light != nullptr)
        {
            lights.push_back(light.get());
        }

        auto cam = item->GetComponent<Camera>();
        if (cam != nullptr)
        {
            camera = (cam);
        }
        auto researchController = item->GetComponent<VoxelResearchCameraController>();
        if (researchController != nullptr)
        {
            researchCameraController = researchController;
        }
    }
}

void VoxelWaterfallApp::CreateGO()
{
    logQueue.Push(std::wstring(L"\nStart Create GO"));

    lights.clear();
    camera.reset();
    researchCameraController.reset();
    voxelResearchSceneManager.RequestPreset(VoxelResearchScenePreset::MixedVoxelEnvironment);
    VoxelResearchSceneContext sceneContext{
        primeDevice,
        secondDevice,
        *assets,
        models,
        srvTexturesMemory,
        gameObjects,
        typedRenderer,
        voxelWorkload,
        AspectRatio()
    };
    voxelResearchSceneManager.RebuildScene(sceneContext);
    RebuildGpuPartitionsForMode();

#if defined(DEBUG) || defined(_DEBUG)
    RunSceneOwnershipSettingsSelfTest();
#endif

    logQueue.Push(std::wstring(L"\nFinish create GO"));
}

#if defined(DEBUG) || defined(_DEBUG)
void VoxelWaterfallApp::RunSceneOwnershipSettingsSelfTest()
{
    if (voxelResearchSceneManager.GetActivePreset() != VoxelResearchScenePreset::MixedVoxelEnvironment)
        return;

    const VoxelExecutionMode originalRequestedMode = requestedExecutionMode;
    const float originalSecondaryShare = voxelWorkload.SecondaryShare;
    const VoxelSpatialLodSettings originalSpatialLod = voxelWorkload.SpatialLod;
    const VoxelTemporalPolicy originalTemporalPolicy = voxelWorkload.TemporalPolicy;
    const uint32_t originalTemporalInterval = voxelWorkload.TemporalDecimationInterval;
    const VoxelCompositeDebugView originalCompositeDebugView = voxelCompositeDebugView;
    const StaticLayerSnapshot before = CaptureStaticLayerSnapshot(voxelWorkload);

    assert(before.Valid);
    assert(voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::Off);

    ApplyExecutionMode(VoxelExecutionMode::SingleGpuTemporalDecimation);
    ApplyBenchmarkSecondaryShare(0.25f);
    ApplyBenchmarkSpatialLodEnabled(true);
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();
    ApplyBenchmarkTemporalInterval(4);
    AssertStaticLayerShapePreserved(before, voxelWorkload);

    ApplyExecutionMode(originalRequestedMode);
    voxelWorkload.SecondaryShare = originalSecondaryShare;
    voxelWorkload.SpatialLod = originalSpatialLod;
    voxelWorkload.TemporalPolicy = originalTemporalPolicy;
    voxelWorkload.TemporalDecimationInterval = originalTemporalInterval;
    voxelCompositeDebugView = originalCompositeDebugView;
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();

    AssertStaticLayerShapePreserved(before, voxelWorkload);
    assert(voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::Off);
}
#endif

void VoxelWaterfallApp::CalculateFrameStats()
{
    static float minFps = std::numeric_limits<float>::max();
    static float minMspf = std::numeric_limits<float>::max();
    static float maxFps = std::numeric_limits<float>::min();
    static float maxMspf = std::numeric_limits<float>::min();
    static UINT writeStatisticCount = 0;
    frameCount++;

    if ((timer.TotalTime() - timeElapsed) >= 1.0f)
    {
        const float fps = static_cast<float>(frameCount); // fps = frameCnt / 1
        const float mspf = 1000.0f / fps;

        minFps = std::min(fps, minFps);
        minMspf = std::min(mspf, minMspf);
        maxFps = std::max(fps, maxFps);
        maxMspf = std::max(mspf, maxMspf);

        frameCount = 0;
        timeElapsed += 1.0f;


        const std::string modeNameUtf8 = GetExecutionModeName();
        const std::wstring modeName(modeNameUtf8.begin(), modeNameUtf8.end());
        const std::wstring title = L"FPS " + std::to_wstring(fps) + L" Mode:" + modeName + L" Progress: " + std::to_wstring(
                (static_cast<float>(writeStatisticCount) / StatisticsStepSecondsCount) * 100.0f) + L"/" +
            std::to_wstring(100);

        if (writeStatisticCount >= static_cast<UINT>(StatisticsStepSecondsCount))
        {
            const std::wstring statisticsText =
                L"\nMode: " + modeName
                + L"\nMultiGpu status: " + multiGpuStatus
                + L"\n\tMin FPS:" + std::to_wstring(minFps)
                + L"\n\tMin MSPF:" + std::to_wstring(minMspf)
                + L"\n\tMax FPS:" + std::to_wstring(maxFps)
                + L"\n\tMax MSPF:" + std::to_wstring(maxMspf);

            logQueue.Push(statisticsText);


            writeStatisticCount = 0;
            minFps = std::numeric_limits<float>::max();
            minMspf = std::numeric_limits<float>::max();
            maxFps = std::numeric_limits<float>::min();
            maxMspf = std::numeric_limits<float>::min();

        }
        else
        {
            const std::string actualModeNameUtf8 = GetExecutionModeName(frameGraphTelemetry.ActualMode);
            const std::wstring actualModeName(actualModeNameUtf8.begin(), actualModeNameUtf8.end());
            const std::wstring statisticsText =
                L"\n\tFPS:" + std::to_wstring(fps)
                + L"\n\tMSPF:" + std::to_wstring(mspf)
                + L"\n\tActual mode:" + actualModeName;

            logQueue.Push(statisticsText);

            writeStatisticCount++;
        }


        MainWindow->SetWindowTitle(title);
    }
}

void VoxelWaterfallApp::LogWriting()
{
    const std::filesystem::path filePath(
        L"VoxelWaterfall " + primeDevice->GetName() + L"+" +
        (secondDevice ? secondDevice->GetName() : L"no-secondary") + L".txt");

    const auto path = std::filesystem::current_path().wstring() + L"\\" + filePath.wstring();

    OutputDebugStringW(path.c_str());

    std::wofstream fileSteam;
    fileSteam.open(path.c_str(), std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);
    if (fileSteam.is_open())
    {
        fileSteam << L"Information" << std::endl << L"Statistic step seconds:" << std::to_wstring(
            StatisticsStepSecondsCount) << std::endl;
    }

    std::wstring line;

    while (logQueue.Size() > 0)
    {
        while (logQueue.TryPop(line))
        {
            fileSteam << line;
        }
    }

    fileSteam << L"\nFinish Logs" << std::endl;

    fileSteam.flush();
    fileSteam.close();
}

int VoxelWaterfallApp::Run()
{
    MSG msg{};

    timer.Reset();

    while (msg.message != WM_QUIT)
    {
        // If there are Window messages then process them.
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        // Otherwise, do animation/game stuff.
        else
        {
            if (isStopRequested)
            {
                MainWindow->SetWindowTitle(MainWindow->GetWindowName() + L" Finished. Wait...");
                LogWriting();
                Quit();
                break;
            }

            timer.Tick();

            CalculateFrameStats();
            Update(timer);
            Draw(timer);

            if (primeDevice)
            {
                primeDevice->ResetAllocators(frameCount);
            }
            if (secondDevice && secondDevice != primeDevice)
            {
                secondDevice->ResetAllocators(frameCount);
            }
        }
    }

    benchmarkController.Shutdown(BuildBenchmarkControllerContext());
    return 0;
}

void VoxelWaterfallApp::UpdateMaterials()
{
    {
        auto currentMaterialBuffer = currentFrameResource->MaterialBuffer;

        for (auto&& material : assets->GetMaterials())
        {
            material->Update();
            auto constantData = material->GetMaterialConstantData();
            currentMaterialBuffer->CopyData(material->GetIndex(), constantData);
        }
    }
}

void VoxelWaterfallApp::UpdateShadowTransform(const GameTimer& gt)
{
    // Only the first "main" light casts a shadow.
    Vector3 lightDir = mBaseLightDirections[0];
    Vector3 lightPos = -2.0f * mSceneBounds.Radius * lightDir;
    Vector3 targetPos = mSceneBounds.Center;
    Vector3 lightUp = Vector3::Up;
    Matrix lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

    mLightPosW = lightPos;


    // Transform bounding sphere to light space.
    Vector3 sphereCenterLS = Vector3::Transform(targetPos, lightView);


    // Ortho frustum in light space encloses scene.
    float l = sphereCenterLS.x - mSceneBounds.Radius;
    float b = sphereCenterLS.y - mSceneBounds.Radius;
    float n = sphereCenterLS.z - mSceneBounds.Radius;
    float r = sphereCenterLS.x + mSceneBounds.Radius;
    float t = sphereCenterLS.y + mSceneBounds.Radius;
    float f = sphereCenterLS.z + mSceneBounds.Radius;

    mLightNearZ = n;
    mLightFarZ = f;
    Matrix lightProj = DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    Matrix T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    Matrix S = lightView * lightProj * T;
    mLightView = lightView;
    mLightProj = lightProj;
    mShadowTransform = S;
}

void VoxelWaterfallApp::UpdateShadowPassCB(const GameTimer& gt)
{
    auto view = mLightView;
    auto proj = mLightProj;

    auto viewProj = (view * proj);
    auto invView = view.Invert();
    auto invProj = proj.Invert();
    auto invViewProj = viewProj.Invert();

    shadowPassCB.View = view.Transpose();
    shadowPassCB.InvView = invView.Transpose();
    shadowPassCB.Proj = proj.Transpose();
    shadowPassCB.InvProj = invProj.Transpose();
    shadowPassCB.ViewProj = viewProj.Transpose();
    shadowPassCB.InvViewProj = invViewProj.Transpose();
    shadowPassCB.EyePosW = mLightPosW;
    shadowPassCB.NearZ = mLightNearZ;
    shadowPassCB.FarZ = mLightFarZ;

    UINT w = shadowPath->Width();
    UINT h = shadowPath->Height();
    shadowPassCB.RenderTargetSize = Vector2(static_cast<float>(w), static_cast<float>(h));
    shadowPassCB.InvRenderTargetSize = Vector2(1.0f / w, 1.0f / h);

    auto currPassCB = currentFrameResource->PrimePassConstantUploadBuffer;
    currPassCB->CopyData(1, shadowPassCB);
}

void VoxelWaterfallApp::UpdateMainPassCB(const GameTimer& gt)
{
    auto view = camera->GetViewMatrix();
    auto proj = camera->GetProjectionMatrix();

    auto viewProj = (view * proj);
    auto invView = view.Invert();
    auto invProj = proj.Invert();
    auto invViewProj = viewProj.Invert();
    auto shadowTransform = mShadowTransform;

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    Matrix T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);
    Matrix viewProjTex = XMMatrixMultiply(viewProj, T);
    mainPassCB.View = view.Transpose();
    mainPassCB.InvView = invView.Transpose();
    mainPassCB.Proj = proj.Transpose();
    mainPassCB.InvProj = invProj.Transpose();
    mainPassCB.ViewProj = viewProj.Transpose();
    mainPassCB.InvViewProj = invViewProj.Transpose();
    mainPassCB.ViewProjTex = viewProjTex.Transpose();
    mainPassCB.ShadowTransform = shadowTransform.Transpose();
    mainPassCB.EyePosW = camera->gameObject->GetTransform()->GetWorldPosition();
    mainPassCB.debugMap = voxelWorkload.DynamicShadowsEnabled ? 1.0f : 0.0f;
    mainPassCB.RenderTargetSize = Vector2(static_cast<float>(MainWindow->GetClientWidth()),
                                          static_cast<float>(MainWindow->GetClientHeight()));
    mainPassCB.InvRenderTargetSize = Vector2(1.0f / mainPassCB.RenderTargetSize.x,
                                             1.0f / mainPassCB.RenderTargetSize.y);
    mainPassCB.NearZ = camera->GetNearZ();
    mainPassCB.FarZ = camera->GetFarZ();
    mainPassCB.TotalTime = gt.TotalTime();
    mainPassCB.DeltaTime = gt.DeltaTime();
    mainPassCB.AmbientLight = Vector4{0.20f, 0.22f, 0.24f, 1.0f};

    for (int i = 0; i < MaxLights; ++i)
    {
        if (i < lights.size())
        {
            mainPassCB.Lights[i] = lights[i]->GetData();
        }
        else
        {
            break;
        }
    }

    mainPassCB.Lights[0].Direction = mBaseLightDirections[0];
    mainPassCB.Lights[1].Direction = mBaseLightDirections[1];
    mainPassCB.Lights[2].Direction = mBaseLightDirections[2];
    switch (voxelWorkload.LightingMode)
    {
    case VoxelResearchLightingPreset::OcclusionValidationLighting:
        mainPassCB.AmbientLight = Vector4{0.10f, 0.10f, 0.12f, 1.0f};
        mainPassCB.Lights[0].Strength = Vector3{1.05f, 1.0f, 0.9f};
        mainPassCB.Lights[1].Strength = Vector3{0.12f, 0.16f, 0.22f};
        mainPassCB.Lights[2].Strength = Vector3{0.0f, 0.0f, 0.0f};
        break;
    case VoxelResearchLightingPreset::DemoStaticSky:
        mainPassCB.AmbientLight = Vector4{0.24f, 0.27f, 0.30f, 1.0f};
        mainPassCB.Lights[0].Strength = Vector3{0.82f, 0.82f, 0.78f};
        mainPassCB.Lights[1].Strength = Vector3{0.18f, 0.20f, 0.24f};
        mainPassCB.Lights[2].Strength = Vector3{0.08f, 0.08f, 0.08f};
        break;
    case VoxelResearchLightingPreset::BenchmarkNeutral:
    default:
        mainPassCB.AmbientLight = Vector4{0.20f, 0.22f, 0.24f, 1.0f};
        mainPassCB.Lights[0].Strength = Vector3{0.78f, 0.78f, 0.74f};
        mainPassCB.Lights[1].Strength = Vector3{0.12f, 0.14f, 0.16f};
        mainPassCB.Lights[2].Strength = Vector3{0.0f, 0.0f, 0.0f};
        break;
    }

    auto currentPassCB = currentFrameResource->PrimePassConstantUploadBuffer;
    currentPassCB->CopyData(0, mainPassCB);
    if (currentFrameResource->SecondaryPassConstantUploadBuffer)
        currentFrameResource->SecondaryPassConstantUploadBuffer->CopyData(0, mainPassCB);
}

void VoxelWaterfallApp::UpdateSsaoCB(const GameTimer& gt)
{
    SsaoConstants ssaoCB;

    auto P = camera->GetProjectionMatrix();

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    Matrix T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    ssaoCB.Proj = mainPassCB.Proj;
    ssaoCB.InvProj = mainPassCB.InvProj;
    XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

    {
        ambientPrimePath->GetOffsetVectors(ssaoCB.OffsetVectors);

        auto blurWeights = ambientPrimePath->CalcGaussWeights(2.5f);
        ssaoCB.BlurWeights[0] = Vector4(&blurWeights[0]);
        ssaoCB.BlurWeights[1] = Vector4(&blurWeights[4]);
        ssaoCB.BlurWeights[2] = Vector4(&blurWeights[8]);

        ssaoCB.InvRenderTargetSize = Vector2(1.0f / ambientPrimePath->SsaoMapWidth(),
                                             1.0f / ambientPrimePath->SsaoMapHeight());

        // Coordinates given in view space.
        ssaoCB.OcclusionRadius = 0.5f;
        ssaoCB.OcclusionFadeStart = 0.2f;
        ssaoCB.OcclusionFadeEnd = 1.0f;
        ssaoCB.SurfaceEpsilon = 0.05f;

        auto currSsaoCB = currentFrameResource->SsaoConstantUploadBuffer;
        currSsaoCB->CopyData(0, ssaoCB);
    }
}

bool VoxelWaterfallApp::InitMainWindow()
{
    MainWindow = CreateRenderWindow(primeDevice, mainWindowCaption, 1920, 1080, false);

    logQueue.Push(std::wstring(L"\nInit Window"));
    return true;
}

void VoxelWaterfallApp::OnResize()
{
    D3DApp::OnResize();
    voxelWorkload.RenderResolutionWidth = static_cast<uint32_t>(MainWindow->GetClientWidth());
    voxelWorkload.RenderResolutionHeight = static_cast<uint32_t>(MainWindow->GetClientHeight());

    fullViewport.Height = static_cast<float>(MainWindow->GetClientHeight());
    fullViewport.Width = static_cast<float>(MainWindow->GetClientWidth());
    fullViewport.MinDepth = 0.0f;
    fullViewport.MaxDepth = 1.0f;
    fullViewport.TopLeftX = 0;
    fullViewport.TopLeftY = 0;
    fullRect = D3D12_RECT{0, 0, MainWindow->GetClientWidth(), MainWindow->GetClientHeight()};


    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = GetSRGBFormat(BackBufferFormat);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    for (int i = 0; i < globalCountFrameResources; ++i)
    {
        MainWindow->GetBackBuffer(i).CreateRenderTargetView(&rtvDesc, &frameResources[i]->BackBufferRTVMemory);
    }


    if (camera != nullptr)
    {
        camera->SetAspectRatio(AspectRatio());
    }

    if (ambientPrimePath != nullptr)
    {
        ambientPrimePath->OnResize(MainWindow->GetClientWidth(), MainWindow->GetClientHeight());
        ambientPrimePath->RebuildDescriptors();
    }

    if (antiAliasingPrimePath != nullptr)
    {
        antiAliasingPrimePath->OnResize(MainWindow->GetClientWidth(), MainWindow->GetClientHeight());
    }

    if (multiGpuAvailable)
    {
        Flush();
        RebuildMultiGpuVoxelRenderTargets();
    }

    currentFrameResourceIndex = MainWindow->GetCurrentBackBufferIndex();
}

void VoxelWaterfallApp::Flush()
{
    if (primeDevice)
        primeDevice->Flush();
    if (secondDevice && secondDevice != primeDevice)
        secondDevice->Flush();
    lastPrimaryPartitionGraphicsFenceValue = 0;
    lastSecondaryPartitionGraphicsFenceValue = 0;
    lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Primary;
}

LRESULT VoxelWaterfallApp::MsgProc(const HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    if (benchmarkController.IsAutomaticActive())
        return D3DApp::MsgProc(hwnd, msg, wParam, lParam);

    if (msg == WM_KEYDOWN && HandleDemoPresetHotkey(wParam))
        return 0;

    if (imguiInitialized && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    bool inputHandled = false;
    const LRESULT inputResult = inputRouter.Route(hwnd, msg, wParam, lParam, keyboard, mouse, inputHandled);
    if (inputHandled)
        return inputResult;

    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}
