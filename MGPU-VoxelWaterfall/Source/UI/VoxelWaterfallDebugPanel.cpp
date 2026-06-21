#include "Source/UI/VoxelWaterfallDebugPanel.h"

#include "GCommandList.h"
#include "GDescriptor.h"
#include "Source/Rendering/RenderPipeline.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cstdint>

namespace
{
    const char* ExecutionModeName(const VoxelExecutionMode mode)
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
        default:
            return "Unknown";
        }
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
        return strategy == VoxelPartitionStrategy::SpatialPlane ? "SpatialPlane" : "HashedChunks";
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

    const char* CompositeViewName(const VoxelCompositeDebugView view)
    {
        switch (view)
        {
        case VoxelCompositeDebugView::FinalComposite:
            return "Final Composite";
        case VoxelCompositeDebugView::PrimaryOnly:
            return "Primary Only";
        case VoxelCompositeDebugView::SecondaryColorOnly:
            return "Secondary Color Only";
        case VoxelCompositeDebugView::SecondaryLinearDepth:
            return "Secondary Linear Depth";
        case VoxelCompositeDebugView::PrimaryLinearDepth:
            return "Primary Linear Depth";
        case VoxelCompositeDebugView::PartitionOwnershipColors:
            return "Partition Ownership";
        case VoxelCompositeDebugView::SpatialLodColors:
            return "Spatial LOD";
        case VoxelCompositeDebugView::DepthDifference:
            return "Depth Difference";
        default:
            return "Unknown";
        }
    }

    const char* SpatialLodStatusName(const VoxelSceneWorkload& workload)
    {
        if (workload.SpatialLod.Mode == VoxelSpatialLodMode::Off)
            return "Off";
        if (workload.SpatialLod.FreezeCamera)
            return "ThreeLevel frozen";
        return "ThreeLevel";
    }

    bool UsesTemporalExecution(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    const char* BenchmarkClassName(const VoxelSceneWorkload& workload, const VoxelExecutionMode requestedMode)
    {
        const bool requestedTemporal = UsesTemporalExecution(requestedMode);
        const bool workloadTemporal = workload.TemporalPolicy == VoxelTemporalPolicy::Decimated;
        if (requestedTemporal != workloadTemporal)
            return "Invalid mixed-quality configuration";
        if (workload.BenchmarkConfigClass == VoxelBenchmarkConfigClass::Diagnostic ||
            workload.Profile == VoxelResearchWorkloadProfile::OcclusionValidation ||
            workload.CameraMode == VoxelResearchCameraMode::Interactive ||
            workload.PartitionStrategy == VoxelPartitionStrategy::SpatialPlane ||
            workload.SpatialLod.FreezeCamera ||
            workload.SecondaryShare <= 0.0f ||
            workload.SecondaryShare >= 1.0f ||
            workload.StaticStorageMode == StaticVoxelStorageMode::DenseSolidStress)
            return "Diagnostic configuration";
        return "Valid matching benchmark configuration";
    }

    uint32_t CountOwnedVoxels(const VoxelSceneWorkload& workload, const VoxelAdapterOwner owner)
    {
        uint32_t count = 0;
        for (const auto& partition : workload.Partitions)
        {
            if (partition.AdapterOwner == owner)
                count += partition.VoxelCount();
        }
        return count;
    }

    uint32_t TotalChunkCount(const VoxelSceneWorkload& workload)
    {
        uint32_t count = 0;
        for (const auto& layer : workload.Layers)
            count += static_cast<uint32_t>(layer.ChunkIds.size());
        return count;
    }

    uint32_t SeedForScene(const VoxelSceneWorkload& workload)
    {
        return workload.ActualStaticVoxelCount > 0
            ? workload.StaticGenerationSeed
            : workload.Parameters.Seed;
    }

    void DrawMetric(const char* label, const char* value)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::TextUnformatted(value);
    }

    void DrawMetricU32(const char* label, const uint32_t value)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::Text("%u", value);
    }

    void DrawMetricU64(const char* label, const uint64_t value)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::Text("%llu", static_cast<unsigned long long>(value));
    }

    void DrawMetricF32(const char* label, const float value, const char* format = "%.2f")
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::Text(format, value);
    }

    void DrawMetricVec3(const char* label, const DirectX::SimpleMath::Vector3& value)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::Text("%.2f, %.2f, %.2f", value.x, value.y, value.z);
    }

    void DrawMetricF64(const char* label, const double value, const char* format = "%.6f")
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::Text(format, value);
    }
}

void VoxelWaterfallDebugPanel::Draw(const VoxelWaterfallDebugPanelContext& context) const
{
    if (!context.ImGuiInitialized)
        return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    const auto* telemetry = context.FrameGraphTelemetry;
    const VoxelExecutionMode actualMode = telemetry ? telemetry->ActualMode : context.ExecutionMode;
    const uint32_t primaryOwnedVoxels = CountOwnedVoxels(context.Workload, VoxelAdapterOwner::Primary);
    const uint32_t secondaryOwnedVoxels = CountOwnedVoxels(context.Workload, VoxelAdapterOwner::Secondary);
    const uint32_t primaryRenderedCount =
        telemetry ? telemetry->PrimarySpatialLodStats.TotalRendered() : 0;
    const uint32_t secondaryRenderedCount =
        telemetry ? telemetry->SecondarySpatialLodStats.TotalRendered() : 0;
    const uint32_t lod0Count =
        telemetry ? telemetry->PrimarySpatialLodStats.Lod0Rendered + telemetry->SecondarySpatialLodStats.Lod0Rendered : 0;
    const uint32_t lod1Count =
        telemetry ? telemetry->PrimarySpatialLodStats.Lod1Rendered + telemetry->SecondarySpatialLodStats.Lod1Rendered : 0;
    const uint32_t lod2Count =
        telemetry ? telemetry->PrimarySpatialLodStats.Lod2Rendered + telemetry->SecondarySpatialLodStats.Lod2Rendered : 0;
    const bool inputLocked = context.Workload.CameraMode != VoxelResearchCameraMode::Interactive ||
        context.BenchmarkProfiler.IsActive() ||
        context.AutomaticBenchmarkActive;
    const double routeTime = static_cast<double>(context.SimulationFrameIndex) / 60.0;

    ImGui::SetNextWindowPos(ImVec2(10.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.58f);
    const ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("VoxelResearchOverlay", nullptr, overlayFlags))
    {
        ImGui::Text("%s | voxels %u | %s | P/S %u/%u | LOD %s",
                    context.Workload.ScenePreset.c_str(),
                    context.Workload.TotalVoxelCount,
                    ExecutionModeName(actualMode),
                    primaryOwnedVoxels,
                    secondaryOwnedVoxels,
                    SpatialLodStatusName(context.Workload));
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10.0f, 42.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("MGPU Voxel Research", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    const char* executionModes[] = {
        "SingleGpuFull",
        "MultiGpuFull",
        "SingleGpuTemporalDecimation",
        "MultiGpuTemporalDecimation"
    };
    int selectedMode = 0;
    if (context.RequestedExecutionMode == VoxelExecutionMode::MultiGpuFull)
        selectedMode = 1;
    else if (context.RequestedExecutionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
        selectedMode = 2;
    else if (context.RequestedExecutionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
        selectedMode = 3;
    if (ImGui::Combo("Mode", &selectedMode, executionModes, IM_ARRAYSIZE(executionModes)) &&
        context.ApplyExecutionMode)
    {
        VoxelExecutionMode requestedMode = VoxelExecutionMode::SingleGpuFull;
        if (selectedMode == 1)
            requestedMode = VoxelExecutionMode::MultiGpuFull;
        else if (selectedMode == 2)
            requestedMode = VoxelExecutionMode::SingleGpuTemporalDecimation;
        else if (selectedMode == 3)
            requestedMode = VoxelExecutionMode::MultiGpuTemporalDecimation;
        context.ApplyExecutionMode(requestedMode);
    }

    const char* profiles[] = {
        "StaticRenderOnly",
        "DynamicSimulationAndRender",
        "MixedStaticAndDynamic",
        "OcclusionValidation",
        "SpatialLodDemonstration"
    };
    int selectedProfile = static_cast<int>(context.Workload.Profile);
    if (ImGui::Combo("Profile", &selectedProfile, profiles, IM_ARRAYSIZE(profiles)) &&
        context.ApplyWorkloadProfile)
    {
        context.ApplyWorkloadProfile(
            static_cast<VoxelResearchWorkloadProfile>(
                std::clamp(selectedProfile, 0,
                           static_cast<int>(VoxelResearchWorkloadProfile::SpatialLodDemonstration))));
    }

    const char* compositeDebugViews[] = {
        "Final Composite",
        "Primary Only",
        "Secondary Color Only",
        "Secondary Linear Depth",
        "Primary Linear Depth",
        "Partition Ownership Colors",
        "Spatial LOD Colors",
        "Depth Difference"
    };
    int selectedCompositeView = static_cast<int>(context.CompositeDebugView);
    if (ImGui::Combo("View", &selectedCompositeView, compositeDebugViews, IM_ARRAYSIZE(compositeDebugViews)))
    {
        context.CompositeDebugView =
            static_cast<VoxelCompositeDebugView>(
                std::clamp(selectedCompositeView, 0,
                           static_cast<int>(VoxelCompositeDebugView::DepthDifference)));
    }

    ImGui::TextDisabled("Hotkeys: F1 static, F2 dynamic, F3 mixed, F4 occlusion, F5 LOD, F6 ownership, F7 secondary, F8 composite");

    if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawMetric("active preset", context.Workload.ScenePreset.c_str());
        DrawMetric("workload profile", ProfileName(context.Workload.Profile));
        DrawMetricU32("static voxel count", context.Workload.ActualStaticVoxelCount);
        DrawMetricU32("dynamic voxel count", context.Workload.ActualDynamicVoxelCount);
        DrawMetricU32("total voxel count", context.Workload.TotalVoxelCount);
        DrawMetricF32("static voxel size", context.Workload.StaticVoxelSize);
        DrawMetricVec3("static bounds min", context.Workload.StaticTelemetry.BoundsMin);
        DrawMetricVec3("static bounds max", context.Workload.StaticTelemetry.BoundsMax);
        DrawMetricU32("chunk count", TotalChunkCount(context.Workload));
        DrawMetricU32("seed", SeedForScene(context.Workload));
    }

    if (ImGui::CollapsingHeader("Multi-GPU", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawMetric("requested mode", ExecutionModeName(context.RequestedExecutionMode));
        DrawMetric("actual mode", ExecutionModeName(actualMode));
        ImGui::TextUnformatted("primary adapter");
        ImGui::SameLine(170.0f);
        ImGui::Text("%S", context.PrimaryAdapterName.c_str());
        ImGui::TextUnformatted("secondary adapter");
        ImGui::SameLine(170.0f);
        ImGui::Text("%S", context.SecondaryAdapterName.c_str());
        DrawMetricU32("primary-owned voxels", primaryOwnedVoxels);
        DrawMetricU32("secondary-owned voxels", secondaryOwnedVoxels);
        DrawMetricF32("secondary share", context.Workload.SecondaryShare);
        DrawMetric("partition strategy", PartitionStrategyName(context.Workload.PartitionStrategy));
        ImGui::TextWrapped("Status: %S", context.MultiGpuStatus.c_str());
    }

    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawMetricU32("primary rendered count", primaryRenderedCount);
        DrawMetricU32("secondary rendered count", secondaryRenderedCount);
        DrawMetricU32("actual secondary draw calls", telemetry ? telemetry->SecondaryDrawCalls : 0);
        DrawMetric("Spatial LOD status", SpatialLodStatusName(context.Workload));
        DrawMetricU32("LOD0 count", lod0Count);
        DrawMetricU32("LOD1 count", lod1Count);
        DrawMetricU32("LOD2 count", lod2Count);
        DrawMetric("Temporal policy", TemporalPolicyName(context.Workload.TemporalPolicy));
        DrawMetricU32("simulation update interval", telemetry ? telemetry->SecondaryEffectiveUpdateInterval : context.Workload.TemporalDecimationInterval);
        DrawMetric("debug view", CompositeViewName(context.CompositeDebugView));
    }

    if (ImGui::CollapsingHeader("Transfer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawMetricU64("color bytes", telemetry ? telemetry->ColorBytesTransferred : 0);
        DrawMetricU64("depth bytes", telemetry ? telemetry->DepthBytesTransferred : 0);
        DrawMetricU64("render-output bytes", telemetry ? telemetry->RenderOutputTransferBytes : 0);
        DrawMetricU64("particle-transfer bytes", telemetry ? telemetry->ParticleTransferBytes : 0);
    }

    if (ImGui::CollapsingHeader("Validation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const bool hasValidation = telemetry && telemetry->VisualValidationHasResult;
        DrawMetricF64("color MAE", telemetry ? telemetry->VisualValidationColorMAE : 0.0);
        DrawMetricF64("color RMSE", telemetry ? telemetry->VisualValidationColorRMSE : 0.0);
        DrawMetricF64("PSNR", telemetry ? telemetry->VisualValidationPSNR : 0.0, "%.2f");
        DrawMetricF64("depth RMSE", telemetry ? telemetry->VisualValidationDepthRMSE : 0.0);
        DrawMetricF64("mismatched pixels", telemetry ? telemetry->VisualValidationMismatchedPixelPercent : 0.0, "%.3f%%");
        DrawMetric("validation", hasValidation ? (telemetry->VisualValidationPassed ? "pass" : "fail") : "not run");
        DrawMetric("benchmark config", BenchmarkClassName(context.Workload, context.RequestedExecutionMode));
        if (telemetry && !telemetry->VisualValidationFailReason.empty())
            ImGui::TextWrapped("Reason: %s", telemetry->VisualValidationFailReason.c_str());
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawMetric("camera mode", CameraModeName(context.Workload.CameraMode));
        DrawMetric("camera route", context.Workload.CameraPath.c_str());
        DrawMetricF64("route time", routeTime, "%.3f s");
        DrawMetric("input locked", inputLocked ? "yes" : "no");
        ImGui::TextUnformatted("resolution");
        ImGui::SameLine(170.0f);
        ImGui::Text("%u x %u", context.Workload.RenderResolutionWidth, context.Workload.RenderResolutionHeight);
    }

    if (ImGui::CollapsingHeader("Controls"))
    {
        if (!context.BenchmarkProfiler.IsActive() && !context.AutomaticBenchmarkActive)
        {
            if (ImGui::Button("Start benchmark") && context.StartBenchmark)
                context.StartBenchmark();
        }
        else if (!context.AutomaticBenchmarkActive)
        {
            if (ImGui::Button("Stop benchmark") && context.StopBenchmark)
                context.StopBenchmark();
        }
        ImGui::SameLine();
        if (ImGui::Button("Run validation") && context.RunVisualValidation)
            context.RunVisualValidation();

        if (!context.AutomaticBenchmarkActive)
        {
            if (ImGui::Button("Start auto benchmark") && context.StartAutomaticBenchmark)
                context.StartAutomaticBenchmark();
        }
        else if (ImGui::Button("Stop auto benchmark") && context.StopAutomaticBenchmark)
        {
            context.StopAutomaticBenchmark();
        }

        const char* cameraModes[] = {
            "Interactive",
            "FixedOverview",
            "FixedOcclusion",
            "WaterfallCloseup",
            "LodSweepRoute",
            "BenchmarkRoute"
        };
        int cameraMode = static_cast<int>(context.Workload.CameraMode);
        if (ImGui::Combo("Camera", &cameraMode, cameraModes, IM_ARRAYSIZE(cameraModes)) &&
            context.ApplyCameraMode)
        {
            context.ApplyCameraMode(
                static_cast<VoxelResearchCameraMode>(
                    std::clamp(cameraMode, 0, static_cast<int>(VoxelResearchCameraMode::BenchmarkRoute))));
        }

        const char* resolutionPresets[] = {"1280x720", "1920x1080", "2560x1440", "3840x2160"};
        int resolutionPreset = static_cast<int>(context.Workload.ResolutionPreset);
        if (ImGui::Combo("Resolution", &resolutionPreset, resolutionPresets, IM_ARRAYSIZE(resolutionPresets)) &&
            context.ApplyRenderResolutionPreset)
        {
            context.ApplyRenderResolutionPreset(
                static_cast<VoxelRenderResolutionPreset>(
                    std::clamp(resolutionPreset, 0,
                               static_cast<int>(VoxelRenderResolutionPreset::R3840x2160))));
        }

        ImGui::Checkbox("Freeze LOD camera", &context.Workload.SpatialLod.FreezeCamera);
        if (ImGui::Button("Apply workload") && context.RequestApplyWorkloadSettings)
            context.RequestApplyWorkloadSettings();

        ImGui::Text("Benchmark progress: %.1f%%", context.BenchmarkProfiler.GetProgress() * 100.0f);
        ImGui::Text("Rows: %u/%u",
                    context.BenchmarkProfiler.GetRowsWritten(),
                    VoxelBenchmarkProfiler::RecordedFrameCount);
    }

    ImGui::End();

    ImGui::Render();
    context.CommandList->SetDescriptorsHeap(context.ImGuiSrvMemory);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), context.CommandList->GetGraphicsCommandList().Get());
}
