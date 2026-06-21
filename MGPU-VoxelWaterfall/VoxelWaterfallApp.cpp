#include "VoxelWaterfallApp.h"
#include "GDescriptorHeap.h"
#include "Source/Assets/SampleAssetManifest.h"
#include "Source/Devices/DeviceSelectionPolicy.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
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
#include "Source/Voxels/VoxelGpuPartition.h"
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

    bool UsesMultiGpuExecution(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::MultiGpuFull ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    VoxelAdapterOwner AdapterOwnerForPartition(const VoxelExecutionMode mode,
                                               const bool multiGpuAvailable,
                                               const VoxelPartitionState& partition)
    {
        return UsesMultiGpuExecution(mode) &&
                   multiGpuAvailable &&
                   partition.PartitionId == VoxelPartitionId::SecondaryPartition
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
    frameGraphTelemetry.PrimaryComputeFenceValue = simulationResult.PrimaryComputeFenceValue;
    frameGraphTelemetry.SecondaryComputeFenceValue = simulationResult.SecondaryComputeFenceValue;
    frameGraphTelemetry.ParticleTransferBytes = 0;

    benchmarkProfiler.UpdateCurrentFrameMetadata(BuildBenchmarkMetadata());

    PrimaryBasePassContext primaryBaseContext{
        renderQueue,
        primaryComputeQueue->GetFence(),
        simulationResult.PrimaryComputeFenceValue,
        timestampHeapIndex,
        *currentFrameResource,
        benchmarkProfiler,
        graphicsPassFenceValue,
        &frameGraphTelemetry,
        [this](const std::shared_ptr<GCommandList>& cmdList)
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
                MainWindow->GetCurrentBackBuffer()
            };
            voxelRenderPasses.RecordPrimaryBase(cmdList, basePassContext);
        }
    };
    renderPipeline.SubmitPrimaryBasePass(primaryBaseContext);
    lastPrimaryPartitionGraphicsFenceValue = currentFrameResource->PrimaryBaseRenderFenceValue;
    auto& primaryRenderedSecondaryPartition =
        voxelWorkload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)];
    if (primaryRenderedSecondaryPartition.AdapterOwner == VoxelAdapterOwner::Primary)
    {
        lastSecondaryPartitionGraphicsFenceValue = currentFrameResource->PrimaryBaseRenderFenceValue;
        lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Primary;
    }

    auto& secondaryPartition =
        voxelWorkload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)];
    const bool runSecondaryGraphics =
        simulationResult.UsedMultiGpuMode &&
        multiGpuVoxelRenderTargets.IsInitialized() &&
        secondaryPartition.AdapterOwner == VoxelAdapterOwner::Secondary &&
        secondaryPartition.GpuPartition &&
        secondaryPartition.VoxelCount() > 0;
    const bool shouldRenderSecondaryImage =
        runSecondaryGraphics &&
        (executionMode == VoxelExecutionMode::MultiGpuFull ||
         executionMode == VoxelExecutionMode::SingleGpuFull ||
         simulationResult.SecondaryWorkThisFrame ||
         !multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex].HasReceivedImage);
    bool secondaryImageReadyThisFrame = false;

    if (shouldRenderSecondaryImage)
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
            *secondaryPartition.GpuPartition,
            secondaryFrameTargets,
            secondaryViewport,
            secondaryScissor,
            benchmarkProfiler,
            &frameGraphTelemetry
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
    else if (simulationResult.UsedMultiGpuMode && secondaryPartition.VoxelCount() > 0)
    {
        frameGraphTelemetry.SecondaryImageReused = true;
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

    const bool multiGpuFullValidationFrame =
        executionMode == VoxelExecutionMode::MultiGpuFull &&
        simulationFrameIndex > VoxelBenchmarkProfiler::WarmupFrameCount &&
        secondaryPartition.VoxelCount() > 0;
    frameGraphTelemetry.VisualValidationPassed =
        !multiGpuFullValidationFrame ||
        (frameGraphTelemetry.RequestedMode == VoxelExecutionMode::MultiGpuFull &&
         frameGraphTelemetry.ActualMode == VoxelExecutionMode::MultiGpuFull &&
         frameGraphTelemetry.SecondaryGraphicsSubmitted &&
         frameGraphTelemetry.SecondaryDrawCalls > 0 &&
         frameGraphTelemetry.RenderOutputTransferBytes > 0 &&
         frameGraphTelemetry.ParticleTransferBytes == 0 &&
         frameGraphTelemetry.CompositeSubmitted &&
         frameGraphTelemetry.CompositeUsedSecondaryImage);

    if (multiGpuFullValidationFrame)
    {
        assert(frameGraphTelemetry.SecondaryGraphicsSubmitted);
        assert(frameGraphTelemetry.SecondaryDrawCalls > 0);
        assert(frameGraphTelemetry.ParticleTransferBytes == 0);
        if (!frameGraphTelemetry.SecondaryImageReused)
            assert(frameGraphTelemetry.RenderOutputTransferBytes > 0);
        assert(!secondaryPartition.GpuPartition ||
               frameGraphTelemetry.SecondaryRenderedVoxelCount ==
               secondaryPartition.GpuPartition->GetStatistics().LastAliveVoxelCount);
        assert(frameGraphTelemetry.RequestedMode == frameGraphTelemetry.ActualMode);
        assert(frameGraphTelemetry.VisualValidationPassed);
    }

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
    sceneTransformController.Refresh();

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
        &sceneTransformController,
        [this] { DrawVoxelWaterfallSceneLabels(); },
        [this](const VoxelExecutionMode mode) { ApplyExecutionMode(mode); },
        [this] { StartManualBenchmark(); },
        [this] { StopManualBenchmark(); },
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

void VoxelWaterfallApp::RequestApplyVoxelWorkloadSettings()
{
    voxelWorkloadSettingsPending = true;
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
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    Vector2 screenPosition;
    const Vector3 labelWorldPosition = voxelWorkload.Position +
        Vector3(0.0f, voxelWorkload.Parameters.SpawnHeight + voxelWorkload.Parameters.VoxelSize * 4.0f, 0.0f);
    if (!ProjectWorldToScreen(labelWorldPosition, screenPosition))
        return;

    std::string label = "VoxelWaterfall\n";
    label += std::to_string(voxelWorkload.TotalVoxelCount);
    label += " total voxels\n";
    label += "Primary: ";
    label += std::to_string(voxelWorkload.Partitions[static_cast<size_t>(
        VoxelPartitionId::PrimaryPartition)].VoxelCount());
    label += "\nSecondary: ";
    label += std::to_string(voxelWorkload.Partitions[static_cast<size_t>(
        VoxelPartitionId::SecondaryPartition)].VoxelCount());

    const float fontSize = ImGui::GetFontSize() * SceneLabelUiScale;
    const ImVec2 baseTextSize = ImGui::CalcTextSize(label.c_str());
    const ImVec2 textSize(baseTextSize.x * SceneLabelUiScale, baseTextSize.y * SceneLabelUiScale);
    const ImVec2 anchor(screenPosition.x, screenPosition.y);
    const ImVec2 textPos(screenPosition.x - textSize.x * 0.5f,
                         screenPosition.y - textSize.y - 18.0f * SceneLabelUiScale);
    const ImVec2 rectMin(textPos.x - 7.0f * SceneLabelUiScale, textPos.y - 5.0f * SceneLabelUiScale);
    const ImVec2 rectMax(textPos.x + textSize.x + 7.0f * SceneLabelUiScale,
                         textPos.y + textSize.y + 5.0f * SceneLabelUiScale);

    const ImU32 color = IM_COL32(80, 220, 255, 255);
    drawList->AddLine(anchor, ImVec2(textPos.x + textSize.x * 0.5f, rectMax.y), color,
                      2.0f * SceneLabelUiScale);
    drawList->AddCircleFilled(anchor, 4.0f * SceneLabelUiScale, color, 12);
    drawList->AddRectFilled(rectMin, rectMax, IM_COL32(5, 14, 24, 220), 5.0f * SceneLabelUiScale);
    drawList->AddRect(rectMin, rectMax, color, 5.0f * SceneLabelUiScale, 0, 1.5f * SceneLabelUiScale);
    drawList->AddText(ImGui::GetFont(), fontSize, textPos, IM_COL32(235, 250, 255, 255), label.c_str());
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
    metadata.RequestedMode = GetExecutionModeName(requestedExecutionMode);
    metadata.ActualMode = GetExecutionModeName(frameGraphTelemetry.ActualMode);
    metadata.TemporalPolicy =
        executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
        executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation
            ? "TemporalDecimation"
            : "Full";
    if (metadata.RequestedMode != metadata.ActualMode)
        metadata.FallbackReason = "requested mode unavailable; actual mode selected by runtime capability checks";
    const auto& primary = voxelWorkload.Partitions[static_cast<size_t>(VoxelPartitionId::PrimaryPartition)];
    const auto& secondary = voxelWorkload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)];
    metadata.PrimaryPartitionVoxelCount = primary.VoxelCount();
    metadata.SecondaryPartitionVoxelCount = secondary.VoxelCount();
    metadata.TotalVoxelCount = voxelWorkload.TotalVoxelCount;
    metadata.UpdatedVoxelCount = primary.UpdatedVoxelCount + secondary.UpdatedVoxelCount;
    metadata.SimulationStepsThisFrame = voxelSimulationStepsThisFrame;
    metadata.Seed = voxelWorkload.Parameters.Seed;
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
    metadata.CpuWaitMs = currentPrimaryWaitMs;
    metadata.TotalCrossAdapterBytes = frameGraphTelemetry.TotalCrossAdapterBytes;
    metadata.ParticleTransferBytes = frameGraphTelemetry.ParticleTransferBytes;
    metadata.SecondaryDrawCalls = frameGraphTelemetry.SecondaryDrawCalls;
    metadata.ReusedSecondaryImage = frameGraphTelemetry.SecondaryImageReused;
    metadata.VisualValidationPassed = frameGraphTelemetry.VisualValidationPassed;
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
    voxelWorkload.TotalVoxelCount = static_cast<uint32_t>(std::max(1, totalCount));
    voxelWorkloadSettingsPending = true;
    ApplyPendingVoxelSettings();
}

void VoxelWaterfallApp::ApplyBenchmarkSecondaryShare(const float secondaryShare)
{
    voxelWorkload.SecondaryShare = std::clamp(secondaryShare, 0.0f, 1.0f);
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

    Flush();
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

    Flush();
    const auto previousPartitions = std::array{
        voxelWorkload.Partitions[static_cast<size_t>(VoxelPartitionId::PrimaryPartition)].GpuPartition,
        voxelWorkload.Partitions[static_cast<size_t>(VoxelPartitionId::SecondaryPartition)].GpuPartition
    };
    voxelWorkload = VoxelWaterfallWorkloadBuilder::Build(voxelWorkload);
    for (size_t i = 0; i < voxelWorkload.Partitions.size(); ++i)
    {
        auto& partition = voxelWorkload.Partitions[i];
        partition.AdapterOwner = AdapterOwnerForPartition(executionMode, multiGpuAvailable, partition);
        partition.GpuPartition = previousPartitions[i];
        if (partition.GpuPartition)
        {
            const auto ownerDevice = DeviceForAdapterOwner(partition.AdapterOwner, primeDevice, secondDevice);
            partition.GpuPartition->Reset(ownerDevice, partition.GlobalVoxelIds, voxelWorkload.Parameters);
            partition.GpuPartition->SetSimulationEnabled(true);
            partition.GpuPartition->SetRenderEnabled(true);
        }
    }
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    voxelExpectedCount = voxelWorkload.TotalVoxelCount;
    simulationFrameIndex = 0;
    voxelWorkloadSettingsPending = false;
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

    voxelWorkload.Position = Vector3(150.0f, 0.0f, 50.0f);
    voxelWorkload.Rotation = Vector3(0.0f, 90.0f, 0.0f);
    voxelWorkload.TotalVoxelCount = 24192;
    voxelWorkload.SecondaryShare = 0.35f;
    voxelWorkload.TemporalDecimationInterval = 2;
    voxelWorkload.Parameters.VoxelSize = 0.50f;
    voxelWorkload.Parameters.SpawnHeight = 14.0f;
    voxelWorkload.Parameters.FloorHeight = -80.0f;
    voxelWorkload.Parameters.WaterfallWidth = 30.0f;
    voxelWorkload.Parameters.WaterfallDepth = 4.0f;
    voxelWorkload.Parameters.InitialFallSpeed = 7.0f;
    voxelWorkload.Parameters.Gravity = 34.0f;
    voxelWorkload.Parameters.Seed = 1337;
    voxelWorkload = VoxelWaterfallWorkloadBuilder::Build(voxelWorkload);

    SceneFactoryContext sceneContext{
        primeDevice,
        secondDevice,
        *assets,
        models,
        srvTexturesMemory,
        gameObjects,
        typedRenderer,
        voxelWorkload,
        executionMode,
        multiGpuAvailable,
        AspectRatio()
    };
    sceneFactory.CreateScene(sceneContext);
    sceneTransformController.Attach(gameObjects);
    sceneTransformController.SetTransformAppliedCallback(
        [this](const GameObject& object, const Vector3& position)
        {
            SyncVoxelWaterfallPosition(object, position);
        });
    sceneTransformController.SetLogCallback(
        [this](const std::wstring& message)
        {
            logQueue.Push(L"\n" + message);
        });

    logQueue.Push(std::wstring(L"\nFinish create GO"));
}

void VoxelWaterfallApp::SyncVoxelWaterfallPosition(const GameObject& object, const Vector3& position)
{
    const auto& name = object.GetName();
    if (name == "VoxelWaterfall")
        voxelWorkload.Position = position;
}

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
    if (imguiInitialized && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    bool inputHandled = false;
    const LRESULT inputResult = inputRouter.Route(hwnd, msg, wParam, lParam, keyboard, mouse, inputHandled);
    if (inputHandled)
        return inputResult;

    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}
