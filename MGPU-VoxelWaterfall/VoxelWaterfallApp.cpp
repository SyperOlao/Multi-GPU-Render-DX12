#include "VoxelWaterfallApp.h"
#include "GDescriptorHeap.h"
#include "Source/Assets/SampleAssetManifest.h"
#include "Source/Devices/DeviceSelectionPolicy.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <utility>

#include "CameraController.h"
#include "GameObject.h"
#include "GDeviceFactory.h"
#include "GModel.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MathHelper.h"
#include "ModelRenderer.h"
#include "Source/Voxels/VoxelWaterfallEmitter.h"
#include "Rotater.h"
#include "SkyBox.h"
#include "Transform.h"
#include "Window.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr float DebugUiScale = 2.25f;
    constexpr float SceneLabelUiScale = 2.0f;

    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
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

        const std::array<std::filesystem::path, 4> sharedAssetCandidates =
        {
            std::filesystem::path(L"..\\MGPU-Particles") / relativePath,
            std::filesystem::path(L"MGPU-Particles") / relativePath,
            GetExecutableDirectory() / L"..\\.." / L"MGPU-Particles" / relativePath,
            GetExecutableDirectory() / L"..\\.." / L"MGPU-VoxelWaterfall" / L"..\\MGPU-Particles" / relativePath
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        for (const auto& candidate : sharedAssetCandidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        return relativePath;
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
    currentSecondaryWaitMs = 0.0;

    const UINT olderIndex = currentFrameResourceIndex - 1 > globalCountFrameResources
                                ? 0
                                : static_cast<UINT>(currentFrameResourceIndex);
    primeGPURenderingTime = primeDevice->GetCommandQueue()->GetTimestamp(olderIndex);
    secondGPURenderingTime = secondDevice->GetCommandQueue()->GetTimestamp(olderIndex);

    primeGPUComputingTime = primeDevice->GetCommandQueue(GQueueType::Compute)->GetTimestamp(olderIndex);
    secondGPUComputingTime = secondDevice->GetCommandQueue(GQueueType::Compute)->GetTimestamp(olderIndex);

    const auto commandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);

    currentFrameResource = frameResources[currentFrameResourceIndex];

    if (currentFrameResource->PrimeRenderFenceValue != 0 && !commandQueue->IsFinish(
        currentFrameResource->PrimeRenderFenceValue))
    {
        const auto waitStart = std::chrono::steady_clock::now();
        commandQueue->WaitForFenceValue(currentFrameResource->PrimeRenderFenceValue);
        currentPrimaryWaitMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
    }

    mLightRotationAngle += 0.1f * gt.DeltaTime();

    const Matrix R = Matrix::CreateRotationY(mLightRotationAngle);
    for (int i = 0; i < 3; ++i)
    {
        auto lightDir = mBaseLightDirections[i];
        lightDir = Vector3::TransformNormal(lightDir, R);
        mRotatedLightDirections[i] = lightDir;
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

    ApplyPendingVoxelSettings();
    auto benchmarkContext = BuildBenchmarkControllerContext();
    benchmarkController.UpdateAutomatic(benchmarkContext);
    const bool benchmarkWasActive = benchmarkProfiler.IsActive();
    benchmarkProfiler.BeginFrame(BuildBenchmarkMetadata());

    const UINT timestampHeapIndex = 2 * currentFrameResourceIndex;

    const auto primaryComputeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto secondaryComputeQueue = secondDevice->GetCommandQueue(GQueueType::Compute);
    const auto crossAdapterCopyQueue = primeDevice->GetCommandQueue(GQueueType::Copy);
    auto renderQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);

    VoxelSimulationSchedulerContext schedulerContext{
        voxelLods,
        executionMode,
        splitMultiGpuAvailable,
        simulationFrameIndex,
        timestampHeapIndex,
        primaryComputeQueue,
        secondaryComputeQueue,
        crossAdapterCopyQueue,
        renderQueue,
        primeComputeFence,
        secondComputeFence,
        secondRenderFence,
        sharedRenderFenceValue,
        primaryComputeQueueFenceValue,
        secondaryComputeQueueFenceValue,
        sharedComputeFenceValue,
        crossAdapterDataReadyFenceValue,
        currentFrameResource->ComputeFenceValue,
        benchmarkProfiler
    };
    const auto simulationResult = voxelScheduler.DispatchFrame(schedulerContext);

    benchmarkProfiler.UpdateCurrentFrameMetadata(BuildBenchmarkMetadata());

    RenderPipelineContext renderContext{
        renderQueue,
        primaryComputeQueue,
        crossAdapterCopyQueue,
        simulationResult.SecondaryWorkThisFrame,
        simulationResult.UsedSplitMultiGpu,
        timestampHeapIndex,
        *currentFrameResource,
        benchmarkProfiler,
        primeRenderFence,
        sharedRenderFenceValue,
        graphicsPassFenceValue,
        [this](const std::shared_ptr<GCommandList>& cmdList)
        {
            VoxelRenderPassContext passContext{
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
                MainWindow->GetCurrentBackBuffer()
            };
            voxelRenderPasses.RecordFrame(cmdList, passContext);
            DrawUserInterface(cmdList);
            cmdList->TransitionBarrier(MainWindow->GetCurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT);
            cmdList->FlushResourceBarriers();
        }
    };
    renderPipeline.RenderFrame(renderContext);

    currentFrameResourceIndex = MainWindow->Present();
    benchmarkProfiler.EndFrameCpu();
    benchmarkProfiler.ProcessCompletedFrames();
    benchmarkController.RestoreVSyncAfterManualCompletion(benchmarkContext, benchmarkWasActive);
    ++simulationFrameIndex;
}

bool VoxelWaterfallApp::Initialize()
{
    InitDevices();
    benchmarkProfiler.Initialize(primeDevice, secondDevice,
                                 primeDevice->GetCommandQueue(GQueueType::Compute),
                                 secondDevice->GetCommandQueue(GQueueType::Compute),
                                 primeDevice->GetCommandQueue(GQueueType::Copy),
                                 primeDevice->GetCommandQueue(GQueueType::Graphics));
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
    sceneTransformController.Load();
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
    primeDevice = selectedDevices.Primary;
    secondDevice = selectedDevices.Secondary;


    assets = std::make_shared<AssetsLoader>(primeDevice);


    for (int i = 0; i < static_cast<uint8_t>(RenderMode::Count); ++i)
    {
        typedRenderer.push_back(
            MemoryAllocator::CreateVector<std::shared_ptr<Renderer>>());
    }

    splitMultiGpuAvailable = allDevices.size() > 1 && secondDevice != primeDevice;
    if (splitMultiGpuAvailable)
    {
        primeDevice->SharedFence(primeComputeFence, secondDevice, secondComputeFence, sharedComputeFenceValue);
        primeDevice->SharedFence(primeRenderFence, secondDevice, secondRenderFence, sharedRenderFenceValue);
        splitMultiGpuStatus = L"SplitMultiGpu available";
    }
    else
    {
        executionMode = VoxelExecutionMode::PrimaryOnly;
        splitMultiGpuStatus = L"SplitMultiGpu disabled: secondary hardware adapter was not found";
        logQueue.Push(L"\n" + splitMultiGpuStatus);
    }

    logQueue.Push(L"\nPrime Device: " + (primeDevice->GetName()));
    logQueue.Push(
        L"\t\n Cross Adapter Texture Support: " + std::to_wstring(
            primeDevice->IsCrossAdapterTextureSupported()));
    if (splitMultiGpuAvailable)
    {
        logQueue.Push(L"\nSecond Device: " + (secondDevice->GetName()));
        logQueue.Push(
            L"\t\n Cross Adapter Texture Support: " + std::to_wstring(
                secondDevice->IsCrossAdapterTextureSupported()));
    }
    else
    {
        logQueue.Push(L"\nSecond Device: unavailable; SplitMultiGpu is disabled");
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
    sceneTransformController.Refresh();

    VoxelWaterfallDebugPanelContext context{
        imguiInitialized,
        cmdList,
        &imguiSrvMemory,
        voxelLods,
        executionMode,
        splitMultiGpuAvailable,
        splitMultiGpuStatus,
        primeDevice ? primeDevice->GetName() : L"unavailable",
        splitMultiGpuAvailable && secondDevice ? secondDevice->GetName() : L"unavailable",
        simulationFrameIndex,
        benchmarkProfiler,
        benchmarkController.WasVSyncEnabled(),
        benchmarkController.IsAutomaticActive(),
        benchmarkController.HasAutomaticConfigs(),
        benchmarkController.GetAutomaticIndex(),
        benchmarkController.GetAutomaticCount(),
        benchmarkController.GetAutomaticSummaryPath(),
        &sceneTransformController,
        [this] { DrawVoxelWaterfallSceneLabels(); },
        [this](const VoxelExecutionMode mode) { ApplyExecutionMode(mode); },
        [this] { StartManualBenchmark(); },
        [this] { StopManualBenchmark(); },
        [this] { StartAutomaticBenchmark(); },
        [this] { StopAutomaticBenchmark(); },
        [this](const size_t lodIndex, const bool enabled) { SetVoxelLodEnabled(lodIndex, enabled); },
        [this](const size_t lodIndex) { RequestApplyVoxelLodSettings(lodIndex); }
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

void VoxelWaterfallApp::SetVoxelLodEnabled(const size_t lodIndex, const bool enabled)
{
    if (lodIndex >= voxelLods.size())
        return;

    auto& lod = voxelLods[lodIndex];
    lod.Enabled = enabled;
    if (lod.CrossEmitter)
        lod.CrossEmitter->SetEnabled(lod.Enabled);
    else if (lod.Emitter)
        lod.Emitter->SetEnabled(lod.Enabled);
}

void VoxelWaterfallApp::RequestApplyVoxelLodSettings(const size_t lodIndex)
{
    if (lodIndex < voxelLods.size())
        voxelLods[lodIndex].SettingsPending = true;
}

bool VoxelWaterfallApp::ProjectWorldToScreen(const Vector3& worldPosition, Vector2& screenPosition) const
{
    if (!camera || !MainWindow)
        return false;

    const Matrix viewProj = camera->GetViewMatrix() * camera->GetProjectionMatrix();
    const Vector4 clip = Vector4::Transform(Vector4(worldPosition.x, worldPosition.y, worldPosition.z, 1.0f), viewProj);
    if (clip.w <= 1e-4f)
        return false;

    const float invW = 1.0f / clip.w;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    if (ndcX < -1.25f || ndcX > 1.25f || ndcY < -1.25f || ndcY > 1.25f)
        return false;

    const float width = static_cast<float>(MainWindow->GetClientWidth());
    const float height = static_cast<float>(MainWindow->GetClientHeight());
    screenPosition.x = (ndcX * 0.5f + 0.5f) * width;
    screenPosition.y = (1.0f - (ndcY * 0.5f + 0.5f)) * height;
    return true;
}

void VoxelWaterfallApp::DrawVoxelWaterfallSceneLabels()
{
    static constexpr ImU32 labelColors[] =
    {
        IM_COL32(80, 220, 255, 255),
        IM_COL32(80, 170, 255, 255),
        IM_COL32(150, 200, 255, 255)
    };

    static constexpr const char* lodTitles[] =
    {
        "Near LOD",
        "Medium LOD",
        "Far LOD"
    };

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    for (size_t i = 0; i < voxelLods.size(); ++i)
    {
        const auto& lod = voxelLods[i];
        if (!lod.Enabled)
            continue;

        Vector2 screenPosition;
        const Vector3 labelWorldPosition = lod.Position +
            Vector3(0.0f, lod.Parameters.SpawnHeight + lod.Parameters.VoxelSize * 4.0f, 0.0f);
        if (!ProjectWorldToScreen(labelWorldPosition, screenPosition))
            continue;

        const bool secondaryGpu =
            (executionMode == VoxelExecutionMode::SplitMultiGpu ||
             executionMode == VoxelExecutionMode::SplitMultiGpuLod) &&
            (i == MediumVoxelWaterfall || i == FarVoxelWaterfall) &&
            splitMultiGpuAvailable;

        std::string label = lodTitles[i];
        label += secondaryGpu ? "\nSecondary GPU" : "\nPrimary GPU";
        label += "\n";
        label += std::to_string(std::max(0, lod.VoxelCount));
        label += " voxels";

        const float fontSize = ImGui::GetFontSize() * SceneLabelUiScale;
        const ImVec2 baseTextSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 textSize(baseTextSize.x * SceneLabelUiScale, baseTextSize.y * SceneLabelUiScale);
        const float sideOffset = ((i == NearVoxelWaterfall) ? -28.0f : (i == FarVoxelWaterfall ? 28.0f : 0.0f)) *
            SceneLabelUiScale;
        const ImVec2 anchor(screenPosition.x, screenPosition.y);
        const ImVec2 textPos(screenPosition.x - textSize.x * 0.5f + sideOffset,
                             screenPosition.y - textSize.y - 18.0f * SceneLabelUiScale);
        const ImVec2 rectMin(textPos.x - 7.0f * SceneLabelUiScale, textPos.y - 5.0f * SceneLabelUiScale);
        const ImVec2 rectMax(textPos.x + textSize.x + 7.0f * SceneLabelUiScale,
                             textPos.y + textSize.y + 5.0f * SceneLabelUiScale);

        const ImU32 color = labelColors[i];
        drawList->AddLine(anchor, ImVec2(textPos.x + textSize.x * 0.5f, rectMax.y), color,
                          2.0f * SceneLabelUiScale);
        drawList->AddCircleFilled(anchor, 4.0f * SceneLabelUiScale, color, 12);
        drawList->AddRectFilled(rectMin, rectMax, IM_COL32(5, 14, 24, 220), 5.0f * SceneLabelUiScale);
        drawList->AddRect(rectMin, rectMax, color, 5.0f * SceneLabelUiScale, 0, 1.5f * SceneLabelUiScale);
        drawList->AddText(ImGui::GetFont(), fontSize, textPos, IM_COL32(235, 250, 255, 255), label.c_str());
    }
}

std::string VoxelWaterfallApp::GetExecutionModeName() const
{
    if (executionMode == VoxelExecutionMode::SplitMultiGpu)
        return "SplitMultiGpu";
    if (executionMode == VoxelExecutionMode::SplitMultiGpuLod)
        return "SplitMultiGpuLod";
    return "PrimaryOnly";
}

VoxelBenchmarkProfiler::FrameMetadata VoxelWaterfallApp::BuildBenchmarkMetadata() const
{
    VoxelBenchmarkProfiler::FrameMetadata metadata{};
    metadata.FrameIndex = simulationFrameIndex;
    metadata.ExecutionMode = GetExecutionModeName();

    metadata.NearVoxelCount = voxelLods[NearVoxelWaterfall].Enabled
                                  ? static_cast<uint32_t>(std::max(0, voxelLods[NearVoxelWaterfall].VoxelCount))
                                  : 0;
    metadata.MediumVoxelCount = voxelLods[MediumVoxelWaterfall].Enabled
                                    ? static_cast<uint32_t>(std::max(0, voxelLods[MediumVoxelWaterfall].VoxelCount))
                                    : 0;
    metadata.FarVoxelCount = voxelLods[FarVoxelWaterfall].Enabled
                                 ? static_cast<uint32_t>(std::max(0, voxelLods[FarVoxelWaterfall].VoxelCount))
                                 : 0;
    metadata.TotalVoxelCount = metadata.NearVoxelCount + metadata.MediumVoxelCount + metadata.FarVoxelCount;
    metadata.UpdatedVoxelCount = voxelLods[NearVoxelWaterfall].UpdatedVoxelCount +
        voxelLods[MediumVoxelWaterfall].UpdatedVoxelCount +
        voxelLods[FarVoxelWaterfall].UpdatedVoxelCount;
    metadata.MediumUpdateInterval = executionMode == VoxelExecutionMode::SplitMultiGpuLod
                                        ? std::max<uint32_t>(1, voxelLods[MediumVoxelWaterfall].UpdateInterval)
                                        : 1;
    metadata.FarUpdateInterval = executionMode == VoxelExecutionMode::SplitMultiGpuLod
                                     ? std::max<uint32_t>(1, voxelLods[FarVoxelWaterfall].UpdateInterval)
                                     : 1;
    metadata.PrimaryAdapterName = primeDevice ? primeDevice->GetName() : L"unavailable";
    metadata.SecondaryAdapterName = splitMultiGpuAvailable && secondDevice
                                        ? secondDevice->GetName()
                                        : L"unavailable";
    metadata.PrimaryWaitMs = currentPrimaryWaitMs;
    metadata.SecondaryWaitMs = currentSecondaryWaitMs;
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
        [this](const int nearCount, const int mediumCount, const int farCount)
        {
            ApplyBenchmarkVoxelCounts(nearCount, mediumCount, farCount);
        },
        splitMultiGpuAvailable
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

void VoxelWaterfallApp::ApplyBenchmarkVoxelCounts(const int nearCount, const int mediumCount, const int farCount)
{
    const int counts[] = {nearCount, mediumCount, farCount};
    for (size_t i = NearVoxelWaterfall; i <= FarVoxelWaterfall; ++i)
    {
        auto& lod = voxelLods[i];
        lod.Enabled = true;
        lod.VoxelCount = std::max(1, counts[i]);
        lod.UpdatedThisFrame = false;
        lod.UpdatedVoxelCount = 0;

        if (lod.CrossEmitter)
        {
            lod.CrossEmitter->ApplySettings(static_cast<UINT>(lod.VoxelCount), lod.Parameters);
            lod.CrossEmitter->SetEnabled(true);
            if (executionMode != VoxelExecutionMode::PrimaryOnly && splitMultiGpuAvailable)
                lod.CrossEmitter->EnableShared();
            else
                lod.CrossEmitter->DisableShared();
        }
        else if (lod.Emitter)
        {
            lod.Emitter->ApplySettings(static_cast<UINT>(lod.VoxelCount), lod.Parameters);
            lod.Emitter->SetEnabled(true);
        }
    }
}

void VoxelWaterfallApp::ApplyExecutionMode(const VoxelExecutionMode requestedMode)
{
    VoxelExecutionMode targetMode = requestedMode;
    if (targetMode != VoxelExecutionMode::PrimaryOnly && !splitMultiGpuAvailable)
    {
        targetMode = VoxelExecutionMode::PrimaryOnly;
        splitMultiGpuStatus = L"SplitMultiGpu disabled: secondary hardware adapter was not found";
    }

    if (executionMode == targetMode)
        return;

    Flush();

    for (size_t i = MediumVoxelWaterfall; i <= FarVoxelWaterfall; ++i)
    {
        if (!voxelLods[i].CrossEmitter)
            continue;

        if (targetMode != VoxelExecutionMode::PrimaryOnly)
            voxelLods[i].CrossEmitter->EnableShared();
        else
            voxelLods[i].CrossEmitter->DisableShared();
    }

    executionMode = targetMode;
    UseCrossAdapter = executionMode != VoxelExecutionMode::PrimaryOnly;
    UseCrossSync = UseCrossAdapter;
    if (executionMode == VoxelExecutionMode::SplitMultiGpu)
    {
        splitMultiGpuStatus = L"SplitMultiGpu active: Medium/Far simulation runs on secondary GPU every frame";
    }
    else if (executionMode == VoxelExecutionMode::SplitMultiGpuLod)
    {
        splitMultiGpuStatus = L"SplitMultiGpuLod active: Medium/Far simulation runs on secondary GPU with update intervals";
    }
    else
    {
        splitMultiGpuStatus = L"PrimaryOnly active: all LOD simulation runs on primary GPU";
    }
    logQueue.Push(L"\nVoxel execution mode changed: " + splitMultiGpuStatus);
}

void VoxelWaterfallApp::ApplyPendingVoxelSettings()
{
    bool hasPendingSettings = false;
    for (const auto& lod : voxelLods)
    {
        if (lod.SettingsPending && (lod.Emitter || lod.CrossEmitter))
        {
            hasPendingSettings = true;
            break;
        }
    }

    if (!hasPendingSettings)
        return;

    Flush();
    for (auto& lod : voxelLods)
    {
        if (!lod.SettingsPending || (!lod.Emitter && !lod.CrossEmitter))
            continue;

        lod.VoxelCount = std::max(1, lod.VoxelCount);
        if (lod.CrossEmitter)
        {
            lod.CrossEmitter->ApplySettings(static_cast<UINT>(lod.VoxelCount), lod.Parameters);
            lod.CrossEmitter->SetEnabled(lod.Enabled);
            if (executionMode != VoxelExecutionMode::PrimaryOnly && splitMultiGpuAvailable)
                lod.CrossEmitter->EnableShared();
        }
        else if (lod.Emitter)
        {
            lod.Emitter->ApplySettings(static_cast<UINT>(lod.VoxelCount), lod.Parameters);
            lod.Emitter->SetEnabled(lod.Enabled);
        }
        lod.SettingsPending = false;
    }
}

void VoxelWaterfallApp::InitFrameResource()
{
    for (int i = 0; i < globalCountFrameResources; ++i)
    {
        frameResources.push_back(std::make_unique<FrameResource>(primeDevice,
                                                                  primeDevice, 2,
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


    models[L"quad"]->SetMeshMaterial(0, assets->GetMaterial(assets->GetMaterialIndex(L"seamless")));

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

    auto sphere = assets->GenerateSphere(cmdList);
    models[L"sphere"] = std::move(sphere);

    auto quad = assets->GenerateQuad(cmdList, -15.0f, -15.0f, 30.0f, 30.0f, 0.0f);
    models[L"quad"] = std::move(quad);

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
    }
}

void VoxelWaterfallApp::CreateGO()
{
    logQueue.Push(std::wstring(L"\nStart Create GO"));

    SceneFactoryContext sceneContext{
        primeDevice,
        secondDevice,
        *assets,
        models,
        srvTexturesMemory,
        gameObjects,
        typedRenderer,
        voxelLods,
        splitMultiGpuAvailable,
        AspectRatio()
    };
    sceneFactory.CreateScene(sceneContext);
    sceneTransformController.Attach(gameObjects);
    sceneTransformController.SetTransformAppliedCallback(
        [this](const GameObject& object, const Vector3& position)
        {
            SyncVoxelLodPosition(object, position);
        });
    sceneTransformController.SetLogCallback(
        [this](const std::wstring& message)
        {
            logQueue.Push(L"\n" + message);
        });

    logQueue.Push(std::wstring(L"\nFinish create GO"));
}

void VoxelWaterfallApp::SyncVoxelLodPosition(const GameObject& object, const Vector3& position)
{
    const auto& name = object.GetName();
    for (auto& lod : voxelLods)
    {
        if (lod.ObjectName && name == lod.ObjectName)
            lod.Position = position;
    }
}

void VoxelWaterfallApp::CalculateFrameStats()
{
    static float minFps = std::numeric_limits<float>::max();
    static float minMspf = std::numeric_limits<float>::max();
    static float maxFps = std::numeric_limits<float>::min();
    static float maxMspf = std::numeric_limits<float>::min();
    static UINT writeStatisticCount = 0;
    static UINT64 primeGPUTimeMax = std::numeric_limits<UINT64>::min();
    static UINT64 primeGPUTimeMin = std::numeric_limits<UINT64>::max();
    static UINT64 secondGPUTimeMax = std::numeric_limits<UINT64>::min();
    static UINT64 secondGPUTimeMin = std::numeric_limits<UINT64>::max();

    static UINT64 primeGPUComputingTimeMax = std::numeric_limits<UINT64>::min();
    static UINT64 primeGPUComputingTimeMin = std::numeric_limits<UINT64>::max();
    static UINT64 secondGPUComputingTimeMax = std::numeric_limits<UINT64>::min();
    static UINT64 secondGPUComputingTimeMin = std::numeric_limits<UINT64>::max();
    frameCount++;

    if ((timer.TotalTime() - timeElapsed) >= 1.0f)
    {
        const float fps = static_cast<float>(frameCount); // fps = frameCnt / 1
        const float mspf = 1000.0f / fps;

        minFps = std::min(fps, minFps);
        minMspf = std::min(mspf, minMspf);
        maxFps = std::max(fps, maxFps);
        maxMspf = std::max(mspf, maxMspf);

        primeGPUTimeMin = std::min(primeGPURenderingTime, primeGPUTimeMin);
        primeGPUTimeMax = std::max(primeGPURenderingTime, primeGPUTimeMax);
        secondGPUTimeMin = std::min(secondGPURenderingTime, secondGPUTimeMin);
        secondGPUTimeMax = std::max(secondGPURenderingTime, secondGPUTimeMax);

        primeGPUComputingTimeMin = std::min(primeGPUComputingTime, primeGPUComputingTimeMin);
        primeGPUComputingTimeMax = std::max(primeGPUComputingTime, primeGPUComputingTimeMax);
        secondGPUComputingTimeMin = std::min(secondGPUComputingTime, secondGPUComputingTimeMin);
        secondGPUComputingTimeMax = std::max(secondGPUComputingTime, secondGPUComputingTimeMax);


        frameCount = 0;
        timeElapsed += 1.0f;


        std::wstring modeName = L"PrimaryOnly";
        if (executionMode == VoxelExecutionMode::SplitMultiGpu)
            modeName = L"SplitMultiGpu";
        else if (executionMode == VoxelExecutionMode::SplitMultiGpuLod)
            modeName = L"SplitMultiGpuLod";
        const std::wstring title = L"FPS " + std::to_wstring(fps) + L" Mode:" + modeName + L" Progress: " + std::to_wstring(
                (static_cast<float>(writeStatisticCount) / StatisticsStepSecondsCount) * 100.0f) + L"/" +
            std::to_wstring(100);

        if (writeStatisticCount >= static_cast<UINT>(StatisticsStepSecondsCount))
        {
            const std::wstring statisticsText =
                L"\nUse Cross Adapter: " + std::to_wstring(UseCrossAdapter) +
                L"\nUse Cross Sync: " + std::to_wstring(UseCrossSync)
                + L"\n\tMin FPS:" + std::to_wstring(minFps)
                + L"\n\tMin MSPF:" + std::to_wstring(minMspf)
                + L"\n\tMax FPS:" + std::to_wstring(maxFps)
                + L"\n\tMax MSPF:" + std::to_wstring(maxMspf)
                + L"\n\tMax Prime GPU Rendering Time:" + std::to_wstring(primeGPUTimeMax) +
                +L"\n\tMin Prime GPU Rendering Time:" + std::to_wstring(primeGPUTimeMin) +
                +L"\n\tMax Second GPU Rendering Time:" + std::to_wstring(secondGPUTimeMax)
                + L"\n\tMin Second GPU Rendering Time:" + std::to_wstring(secondGPUTimeMin)
                + L"\n\tMax Prime GPU Computing Time:" + std::to_wstring(primeGPUComputingTimeMax) +
                +L"\n\tMin Prime GPU Computing Time:" + std::to_wstring(primeGPUComputingTimeMin) +
                +L"\n\tMax Second GPU Computing Time:" + std::to_wstring(secondGPUComputingTimeMax)
                + L"\n\tMin Second GPU Computing Time:" + std::to_wstring(secondGPUComputingTimeMin);

            logQueue.Push(statisticsText);


            writeStatisticCount = 0;
            minFps = std::numeric_limits<float>::max();
            minMspf = std::numeric_limits<float>::max();
            maxFps = std::numeric_limits<float>::min();
            maxMspf = std::numeric_limits<float>::min();
            primeGPUTimeMax = std::numeric_limits<UINT64>::min();
            primeGPUTimeMin = std::numeric_limits<UINT64>::max();
            secondGPUTimeMax = std::numeric_limits<UINT64>::min();
            secondGPUTimeMin = std::numeric_limits<UINT64>::max();
            primeGPUComputingTimeMax = std::numeric_limits<UINT64>::min();
            primeGPUComputingTimeMin = std::numeric_limits<UINT64>::max();
            secondGPUComputingTimeMax = std::numeric_limits<UINT64>::min();
            secondGPUComputingTimeMin = std::numeric_limits<UINT64>::max();

        }
        else
        {
            const std::wstring statisticsText =
                L"\n\tFPS:" + std::to_wstring(fps)
                + L"\n\tMSPF:" + std::to_wstring(mspf)
                + L"\n\tPrime GPU Rendering Time:" + std::to_wstring(primeGPURenderingTime)
                + L"\n\tSecond GPU Rendering Time:" + std::to_wstring(secondGPURenderingTime)
                + L"\n\tPrime GPU Computing Time:" + std::to_wstring(primeGPUComputingTime)
                + L"\n\tSecond GPU Computing Time:" + std::to_wstring(secondGPUComputingTime);

            logQueue.Push(statisticsText);

            writeStatisticCount++;
        }


        MainWindow->SetWindowTitle(title);
    }
}

void VoxelWaterfallApp::LogWriting()
{
    const std::filesystem::path filePath(
        L"SharedParticle " + primeDevice->GetName() + L"+" + secondDevice->GetName() + L".txt");

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
    MSG msg = {nullptr};

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

            //if (!isAppPaused)
            {
                CalculateFrameStats();
                Update(timer);
                Draw(timer);
            }
            //else
            {
                //Sleep(100);
            }

            primeDevice->ResetAllocators(frameCount);
            secondDevice->ResetAllocators(frameCount);
        }
    }

    benchmarkController.Shutdown(BuildBenchmarkControllerContext());
    return static_cast<int>(msg.wParam);
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
    Vector3 lightDir = mRotatedLightDirections[0];
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
    Matrix lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

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
    mainPassCB.RenderTargetSize = Vector2(static_cast<float>(MainWindow->GetClientWidth()),
                                          static_cast<float>(MainWindow->GetClientHeight()));
    mainPassCB.InvRenderTargetSize = Vector2(1.0f / mainPassCB.RenderTargetSize.x,
                                             1.0f / mainPassCB.RenderTargetSize.y);
    mainPassCB.NearZ = 1.0f;
    mainPassCB.FarZ = 1000.0f;
    mainPassCB.TotalTime = gt.TotalTime();
    mainPassCB.DeltaTime = gt.DeltaTime();
    mainPassCB.AmbientLight = Vector4{0.25f, 0.25f, 0.35f, 1.0f};

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

    mainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
    mainPassCB.Lights[0].Strength = Vector3{0.9f, 0.8f, 0.7f};
    mainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
    mainPassCB.Lights[1].Strength = Vector3{0.4f, 0.4f, 0.4f};
    mainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
    mainPassCB.Lights[2].Strength = Vector3{0.2f, 0.2f, 0.2f};

    auto currentPassCB = currentFrameResource->PrimePassConstantUploadBuffer;
    currentPassCB->CopyData(0, mainPassCB);
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

    //for (int i = 0; i < GraphicAdapterCount; ++i)
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

    currentFrameResourceIndex = MainWindow->GetCurrentBackBufferIndex();
}

void VoxelWaterfallApp::Flush()
{
    if (primeDevice)
        primeDevice->Flush();
    if (secondDevice && secondDevice != primeDevice)
        secondDevice->Flush();
}

LRESULT VoxelWaterfallApp::MsgProc(const HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    if (imguiInitialized && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    bool inputHandled = false;
    const LRESULT inputResult = inputRouter.Route(hwnd, msg, wParam, lParam, keyboard, mouse, inputHandled);
    if (inputHandled)
        return inputResult;

    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}
