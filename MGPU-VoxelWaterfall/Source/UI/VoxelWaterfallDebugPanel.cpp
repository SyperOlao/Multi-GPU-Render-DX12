#include "Source/UI/VoxelWaterfallDebugPanel.h"

#include "GCommandList.h"
#include "GDescriptor.h"
#include "Source/Rendering/RenderPipeline.h"
#include "Source/Scene/SceneTransformController.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <algorithm>

namespace
{
    const char* AdapterOwnerName(const VoxelAdapterOwner owner)
    {
        return owner == VoxelAdapterOwner::Secondary ? "Secondary adapter" : "Primary adapter";
    }

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

    void DrawSceneObjectEditor(const VoxelWaterfallDebugPanelContext& context)
    {
        if (!context.SceneEditor)
            return;

        auto& editor = *context.SceneEditor;
        auto& objects = editor.GetEditorItems();
        auto& selectedIndex = editor.GetSelectedIndex();
        if (objects.empty())
        {
            ImGui::TextDisabled("No editable scene objects");
            return;
        }

        selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(objects.size()) - 1);

        const char* previewName = objects[selectedIndex].Name.c_str();
        if (ImGui::BeginCombo("Object", previewName))
        {
            for (int i = 0; i < static_cast<int>(objects.size()); ++i)
            {
                std::string label = std::to_string(objects[i].ObjectIndex) + ": " + objects[i].Name;
                const bool selected = selectedIndex == i;
                if (ImGui::Selectable(label.c_str(), selected))
                    selectedIndex = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        auto& item = objects[selectedIndex];
        bool changed = false;
        changed |= ImGui::DragFloat3("Position", &item.Position.x, 0.1f);
        changed |= ImGui::DragFloat3("Rotation", &item.Rotation.x, 0.25f);
        changed |= ImGui::DragFloat3("Scale", &item.Scale.x, 0.01f, 0.001f, 1000.0f);
        if (changed)
            editor.ApplyEditorItem(static_cast<size_t>(selectedIndex), item);

        if (ImGui::Button("Save scene"))
            editor.Save();
        ImGui::SameLine();
        if (ImGui::Button("Reload scene"))
            editor.Load();

        ImGui::Text("Scene file: %S", editor.GetPath().wstring().c_str());
        ImGui::Text("State: %s", editor.IsDirty() ? "modified" : "saved");
    }
}

void VoxelWaterfallDebugPanel::Draw(const VoxelWaterfallDebugPanelContext& context) const
{
    if (!context.ImGuiInitialized)
        return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (context.DrawSceneLabels)
        context.DrawSceneLabels();

    ImGui::SetNextWindowSize(ImVec2(880.0f, 970.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Voxel Waterfall");

    UINT updatedVoxelCount = 0;
    for (const auto& partition : context.Workload.Partitions)
        updatedVoxelCount += partition.UpdatedVoxelCount;

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

    if (ImGui::Combo("Execution mode", &selectedMode, executionModes, IM_ARRAYSIZE(executionModes)) &&
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

    ImGui::Text("Requested mode: %s", ExecutionModeName(context.RequestedExecutionMode));
    if (context.FrameGraphTelemetry)
        ImGui::Text("Actual mode: %s", ExecutionModeName(context.FrameGraphTelemetry->ActualMode));
    else
        ImGui::Text("Actual mode: %s", ExecutionModeName(context.ExecutionMode));
    ImGui::Text("Primary adapter: %S", context.PrimaryAdapterName.c_str());
    if (context.MultiGpuAvailable)
        ImGui::Text("Secondary adapter: %S", context.SecondaryAdapterName.c_str());
    else
        ImGui::Text("Secondary adapter: unavailable");
    ImGui::Text("MultiGpu status: %S", context.MultiGpuStatus.c_str());
    if (context.AdapterReportLines && ImGui::TreeNode("Adapters"))
    {
        for (const auto& line : *context.AdapterReportLines)
            ImGui::TextWrapped("%S", line.c_str());
        ImGui::TreePop();
    }
    ImGui::Text("Total voxels: %u", context.Workload.TotalVoxelCount);
    ImGui::Text("Primary partition voxels: %u",
                context.Workload.Partitions[static_cast<size_t>(VoxelPartitionId::PrimaryPartition)].VoxelCount());
    ImGui::Text("Secondary partition voxels: %u",
                context.Workload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)].VoxelCount());
    ImGui::Text("Secondary share: %.2f", context.Workload.SecondaryShare);
    for (const auto& partition : context.Workload.Partitions)
        ImGui::Text("%s owner: %s", partition.DisplayName, AdapterOwnerName(partition.AdapterOwner));
    ImGui::Text("Updated elements this frame: %u", updatedVoxelCount);
    ImGui::Text("Simulation frame: %llu", context.SimulationFrameIndex);
    ImGui::Text("Accumulated simulation time: %.4f s", context.AccumulatedSimulationTime);
    ImGui::Text("Simulation steps this frame: %u", context.SimulationStepsThisFrame);
    ImGui::Text("Interpolation alpha: %.3f", context.InterpolationAlpha);
    ImGui::Text("Recycled voxels: %u", context.RecycledVoxelCount);
    ImGui::Text("Alive voxels: %u", context.AliveVoxelCount);
    ImGui::Text("Expected voxels: %u", context.ExpectedVoxelCount);
    const char* compositeDebugViews[] = {
        "Final Composite",
        "Primary Only",
        "Secondary Color Only",
        "Secondary Linear Depth",
        "Primary Linear Depth",
        "Partition Ownership Colors",
        "Depth Difference"
    };
    int selectedCompositeView = static_cast<int>(context.CompositeDebugView);
    if (ImGui::Combo("Composite debug view", &selectedCompositeView,
                     compositeDebugViews, IM_ARRAYSIZE(compositeDebugViews)))
    {
        context.CompositeDebugView =
            static_cast<VoxelCompositeDebugView>(
                std::clamp(selectedCompositeView, 0,
                           static_cast<int>(VoxelCompositeDebugView::DepthDifference)));
    }
    if (context.FrameGraphTelemetry)
    {
        const auto& telemetry = *context.FrameGraphTelemetry;
        ImGui::Separator();
        ImGui::Text("Frame resource index: %u", telemetry.FrameResourceIndex);
        ImGui::Text("Primary compute submitted: %s", telemetry.PrimaryComputeSubmitted ? "yes" : "no");
        ImGui::Text("Primary base graphics submitted: %s",
                    telemetry.PrimaryBaseGraphicsSubmitted ? "yes" : "no");
        ImGui::Text("Secondary compute submitted: %s", telemetry.SecondaryComputeSubmitted ? "yes" : "no");
        ImGui::Text("Secondary graphics submitted: %s",
                    telemetry.SecondaryGraphicsSubmitted ? "yes" : "no");
        ImGui::Text("Secondary draw calls: %u", telemetry.SecondaryDrawCalls);
        ImGui::Text("Secondary rendered voxels: %u", telemetry.SecondaryRenderedVoxelCount);
        ImGui::Text("Secondary image reused: %s", telemetry.SecondaryImageReused ? "yes" : "no");
        ImGui::Text("Depth composite submitted: %s", telemetry.CompositeSubmitted ? "yes" : "no");
        ImGui::Text("Composite uses secondary image: %s",
                    telemetry.CompositeUsedSecondaryImage ? "yes" : "no");
        ImGui::Text("Visual validation: %s", telemetry.VisualValidationPassed ? "passed" : "not passed");
        ImGui::Text("Particle transfer bytes: %llu", telemetry.ParticleTransferBytes);
        ImGui::Text("Render-output transfer bytes: %llu", telemetry.RenderOutputTransferBytes);
        ImGui::Text("Color/depth bytes: %llu / %llu",
                    telemetry.ColorBytesTransferred,
                    telemetry.DepthBytesTransferred);
        ImGui::Text("Fences PC/PB/SC/SG/L2S/XR/S2L/IMG/FP: %llu / %llu / %llu / %llu / %llu / %llu / %llu / %llu / %llu",
                    telemetry.PrimaryComputeFenceValue,
                    telemetry.PrimaryBaseGraphicsFenceValue,
                    telemetry.SecondaryComputeFenceValue,
                    telemetry.SecondaryGraphicsFenceValue,
                    telemetry.SecondaryLocalToSharedCopyFenceValue,
                    telemetry.CrossAdapterRenderReadyFenceValue,
                    telemetry.PrimarySharedToLocalCopyFenceValue,
                    telemetry.PrimarySecondaryImageReadyFenceValue,
                    telemetry.FinalPresentFenceValue);
    }
    const auto& timing = context.BenchmarkProfiler.GetLatestTimingSnapshot();
    ImGui::Separator();
    ImGui::Text("Profiler timing sample: %s", timing.Valid ? "ready" : "waiting for measured frame");
    ImGui::Text("Primary graphics ms: %.3f", timing.PrimaryGraphicsMs);
    ImGui::Text("Secondary graphics ms: %.3f", timing.SecondaryGraphicsMs);
    ImGui::Text("Transfer ms / bytes: %.3f / %llu", timing.TransferMs, timing.TransferBytes);
    ImGui::Text("Composite ms: %.3f", timing.CompositeMs);
    ImGui::Text("Visual validation: %s", timing.VisualValidationPassed ? "passed" : "not passed");
    ImGui::Separator();

    if (!context.BenchmarkProfiler.IsActive() && !context.AutomaticBenchmarkActive)
    {
        if (ImGui::Button("Start Benchmark") && context.StartBenchmark)
            context.StartBenchmark();
    }
    else if (!context.AutomaticBenchmarkActive)
    {
        if (ImGui::Button("Stop Benchmark") && context.StopBenchmark)
            context.StopBenchmark();
    }

    if (!context.AutomaticBenchmarkActive)
    {
        if (ImGui::Button("Start Auto Benchmark") && context.StartAutomaticBenchmark)
            context.StartAutomaticBenchmark();
    }
    else
    {
        if (ImGui::Button("Stop Auto Benchmark") && context.StopAutomaticBenchmark)
            context.StopAutomaticBenchmark();
    }

    ImGui::Text("Auto benchmark: %s", context.AutomaticBenchmarkActive ? "running" : "idle");
    if (context.AutomaticBenchmarkHasConfigs)
    {
        ImGui::Text("Auto test: %u/%u",
                    static_cast<unsigned>(std::min(context.AutomaticBenchmarkIndex + 1,
                                                   context.AutomaticBenchmarkCount)),
                    static_cast<unsigned>(context.AutomaticBenchmarkCount));
    }
    ImGui::TextWrapped("Summary CSV: %S", context.AutomaticBenchmarkSummaryPath.wstring().c_str());
    ImGui::Text("Benchmark progress: %.1f%%", context.BenchmarkProfiler.GetProgress() * 100.0f);
    ImGui::Text("Warm-up: %u/%u", context.BenchmarkProfiler.GetWarmupFramesSeen(),
                VoxelBenchmarkProfiler::WarmupFrameCount);
    ImGui::Text("Recorded rows: %u/%u", context.BenchmarkProfiler.GetRowsWritten(),
                VoxelBenchmarkProfiler::RecordedFrameCount);
    ImGui::TextWrapped("CSV: %S", context.BenchmarkProfiler.GetCsvPath().wstring().c_str());
    if (context.BenchmarkProfiler.IsActive() && !context.BenchmarkVSyncWasEnabled)
        ImGui::Text("VSync was already disabled");
    else if (context.BenchmarkProfiler.IsActive())
        ImGui::Text("VSync disabled during benchmark");
    ImGui::Separator();

    if (ImGui::TreeNodeEx("Workload", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int totalCount = static_cast<int>(context.Workload.TotalVoxelCount);
        if (ImGui::SliderInt("Total voxel count", &totalCount, 128, 262144))
            context.Workload.TotalVoxelCount = static_cast<uint32_t>(std::max(1, totalCount));
        ImGui::SliderFloat("Secondary share", &context.Workload.SecondaryShare, 0.0f, 1.0f, "%.2f");
        int decimation = static_cast<int>(context.Workload.TemporalDecimationInterval);
        if (ImGui::SliderInt("Temporal decimation", &decimation, 1, 16))
            context.Workload.TemporalDecimationInterval = static_cast<uint32_t>(std::max(1, decimation));
        ImGui::SliderFloat("Voxel size", &context.Workload.Parameters.VoxelSize, 0.1f, 4.0f, "%.2f");
        ImGui::SliderFloat("Gravity", &context.Workload.Parameters.Gravity, 1.0f, 40.0f, "%.1f");
        ImGui::SliderFloat("Waterfall height", &context.Workload.Parameters.SpawnHeight, 5.0f, 80.0f, "%.1f");
        ImGui::SliderFloat("Waterfall width", &context.Workload.Parameters.WaterfallWidth, 1.0f, 40.0f, "%.1f");
        ImGui::SliderFloat("Waterfall depth", &context.Workload.Parameters.WaterfallDepth, 0.5f, 12.0f, "%.1f");
        ImGui::Text("Transform: %.1f, %.1f, %.1f",
                    context.Workload.Position.x, context.Workload.Position.y, context.Workload.Position.z);
        ImGui::Text("Seed: %u", context.Workload.Parameters.Seed);

        if (ImGui::Button("Apply and reset") && context.RequestApplyWorkloadSettings)
            context.RequestApplyWorkloadSettings();

        for (const auto& partition : context.Workload.Partitions)
        {
            ImGui::Separator();
            ImGui::Text("%s", partition.DisplayName);
            ImGui::Text("Voxels: %u", partition.VoxelCount());
            ImGui::Text("Owner: %s", AdapterOwnerName(partition.AdapterOwner));
            ImGui::Text("Updated this frame: %s", partition.UpdatedThisFrame ? "yes" : "no");
            ImGui::Text("Last simulation frame: %llu", partition.LastSimulationFrame);
            ImGui::Text("Updated elements: %u", partition.UpdatedVoxelCount);
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    if (ImGui::TreeNodeEx("Scene objects", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSceneObjectEditor(context);
        ImGui::TreePop();
    }

    ImGui::End();

    ImGui::Render();
    context.CommandList->SetDescriptorsHeap(context.ImGuiSrvMemory);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), context.CommandList->GetGraphicsCommandList().Get());
}
