#include "Source/UI/VoxelWaterfallDebugPanel.h"

#include "GCommandList.h"
#include "GDescriptor.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <algorithm>

void VoxelWaterfallDebugPanel::Draw(const VoxelWaterfallDebugPanelContext& context) const
{
    if (!context.ImGuiInitialized)
        return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (context.DrawSceneLabels)
        context.DrawSceneLabels();

    ImGui::SetNextWindowSize(ImVec2(390.0f, 430.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Voxel Waterfall");

    UINT totalVoxelCount = 0;
    for (const auto& lod : context.Lods)
    {
        if (lod.Enabled)
            totalVoxelCount += static_cast<UINT>(std::max(0, lod.VoxelCount));
    }

    UINT updatedVoxelCount = 0;
    for (const auto& lod : context.Lods)
        updatedVoxelCount += lod.UpdatedVoxelCount;

    const char* executionModes[] = {"PrimaryOnly", "SplitMultiGpu", "SplitMultiGpuLod"};
    int selectedMode = 0;
    if (context.ExecutionMode == VoxelExecutionMode::SplitMultiGpu)
        selectedMode = 1;
    else if (context.ExecutionMode == VoxelExecutionMode::SplitMultiGpuLod)
        selectedMode = 2;

    if (!context.SplitMultiGpuAvailable && selectedMode != 0)
        selectedMode = 0;

    if (!context.SplitMultiGpuAvailable)
        ImGui::BeginDisabled();
    if (ImGui::Combo("Execution mode", &selectedMode, executionModes, IM_ARRAYSIZE(executionModes)) &&
        context.ApplyExecutionMode)
    {
        VoxelExecutionMode requestedMode = VoxelExecutionMode::PrimaryOnly;
        if (selectedMode == 1)
            requestedMode = VoxelExecutionMode::SplitMultiGpu;
        else if (selectedMode == 2)
            requestedMode = VoxelExecutionMode::SplitMultiGpuLod;
        context.ApplyExecutionMode(requestedMode);
    }
    if (!context.SplitMultiGpuAvailable)
        ImGui::EndDisabled();

    ImGui::Text("Primary adapter: %S", context.PrimaryAdapterName.c_str());
    if (context.SplitMultiGpuAvailable)
        ImGui::Text("Secondary adapter: %S", context.SecondaryAdapterName.c_str());
    else
        ImGui::Text("Secondary adapter: unavailable");
    ImGui::Text("Split status: %S", context.SplitMultiGpuStatus.c_str());
    ImGui::Text("Total enabled elements: %u", totalVoxelCount);
    ImGui::Text("Updated elements this frame: %u", updatedVoxelCount);
    ImGui::Text("Simulation frame: %llu", context.SimulationFrameIndex);
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

    for (size_t i = 0; i < context.Lods.size(); ++i)
    {
        auto& lod = context.Lods[i];
        if (ImGui::TreeNodeEx(lod.DisplayName, ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool enabled = lod.Enabled;
            if (ImGui::Checkbox("Enabled", &enabled) && context.SetLodEnabled)
                context.SetLodEnabled(i, enabled);

            ImGui::Text("Elements: %d", lod.Enabled ? lod.VoxelCount : 0);
            ImGui::SliderInt("Element count", &lod.VoxelCount, 128, 262144);
            ImGui::SliderFloat("Voxel size", &lod.Parameters.VoxelSize, 0.1f, 4.0f, "%.2f");
            ImGui::SliderFloat("Gravity", &lod.Parameters.Gravity, 1.0f, 40.0f, "%.1f");
            ImGui::SliderFloat("Waterfall height", &lod.Parameters.SpawnHeight, 5.0f, 80.0f, "%.1f");
            ImGui::SliderFloat("Waterfall width", &lod.Parameters.WaterfallWidth, 1.0f, 40.0f, "%.1f");
            ImGui::SliderFloat("Waterfall depth", &lod.Parameters.WaterfallDepth, 0.5f, 12.0f, "%.1f");
            if (i == MediumVoxelWaterfall || i == FarVoxelWaterfall)
            {
                int interval = static_cast<int>(lod.UpdateInterval);
                if (ImGui::SliderInt("Update interval", &interval, 1, 16))
                    lod.UpdateInterval = static_cast<uint32_t>(std::max(1, interval));
            }
            ImGui::Text("Updated this frame: %s", lod.UpdatedThisFrame ? "yes" : "no");
            ImGui::Text("Last simulation frame: %llu", lod.LastSimulationFrame);
            ImGui::Text("Updated elements: %u", lod.UpdatedVoxelCount);
            ImGui::Text("Transform: %.1f, %.1f, %.1f", lod.Position.x, lod.Position.y, lod.Position.z);
            ImGui::Text("Seed: %u", lod.Parameters.Seed);

            std::string buttonLabel = "Apply and reset##";
            buttonLabel += lod.DisplayName;
            if (ImGui::Button(buttonLabel.c_str()) && context.RequestApplyLodSettings)
                context.RequestApplyLodSettings(i);

            ImGui::TreePop();
        }
    }

    ImGui::End();

    ImGui::Render();
    context.CommandList->SetDescriptorsHeap(context.ImGuiSrvMemory);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), context.CommandList->GetGraphicsCommandList().Get());
}

