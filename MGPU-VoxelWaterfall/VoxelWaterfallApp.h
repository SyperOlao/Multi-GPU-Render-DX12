#pragma once
#include "AssetsLoader.h"
#include "Source/Voxels/CrossAdapterVoxelEmitter.h"
#include "Source/Voxels/VoxelWaterfallEmitter.h"
#include "d3dApp.h"
#include "Renderer.h"
#include "RenderModeFactory.h"
#include "ShadowMap.h"
#include "SSAA.h"
#include "SSAO.h"
#include "FrameResource.h"
#include "GCrossAdapterResource.h"
#include "GDeviceFactory.h"
#include "Light.h"
#include "Source/Benchmark/BenchmarkController.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Platform/Win32InputRouter.h"
#include "Source/Rendering/RenderPipeline.h"
#include "Source/Rendering/VoxelRenderPasses.h"
#include "Source/Scene/SceneFactory.h"
#include "Source/UI/VoxelWaterfallDebugPanel.h"
#include "Source/Voxels/VoxelSimulationScheduler.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <vector>

class VoxelWaterfallApp :
    public Common::D3DApp
{
public:
    VoxelWaterfallApp(HINSTANCE hInstance);
    ~VoxelWaterfallApp() override;

    bool Initialize() override;

    int Run() override;

protected:
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt) override;

    void InitDevices();
    void InitUserInterface();
    void DrawUserInterface(const std::shared_ptr<GCommandList>& cmdList);
    void StartManualBenchmark();
    void StopManualBenchmark();
    void SetVoxelLodEnabled(size_t lodIndex, bool enabled);
    void RequestApplyVoxelLodSettings(size_t lodIndex);
    bool ProjectWorldToScreen(const Vector3& worldPosition, Vector2& screenPosition) const;
    void DrawVoxelWaterfallSceneLabels();
    void ApplyPendingVoxelSettings();
    void ApplyExecutionMode(VoxelExecutionMode requestedMode);
    std::string GetExecutionModeName() const;
    VoxelBenchmarkProfiler::FrameMetadata BuildBenchmarkMetadata() const;
    BenchmarkControllerContext BuildBenchmarkControllerContext();
    void StartAutomaticBenchmark();
    void StopAutomaticBenchmark();
    void ApplyBenchmarkVoxelCounts(int nearCount, int mediumCount, int farCount);
    void InitFrameResource();
    void InitRootSignature();
    void InitPipeLineResource();
    void CreateMaterials();
    void InitSRVMemoryAndMaterials();
    void InitRenderPaths();
    void LoadStudyTexture();
    void LoadModels();
    void GenerateMipMaps();
    void SortGO();
    void CreateGO();
    void CalculateFrameStats() override;
    void LogWriting();
    void UpdateMaterials();
    void UpdateShadowTransform(const GameTimer& gt);
    void UpdateShadowPassCB(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateSsaoCB(const GameTimer& gt);
    bool InitMainWindow() override;
    void OnResize() override;
    void Flush() override;
    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

    std::shared_ptr<GDevice> primeDevice;
    std::shared_ptr<GDevice> secondDevice;

    LockThreadQueue<std::wstring> logQueue{};
    UINT64 primeGPURenderingTime = 0;
    UINT64 secondGPURenderingTime = 0;

    UINT64 primeGPUComputingTime = 0;
    UINT64 secondGPUComputingTime = 0;

    D3D12_VIEWPORT fullViewport{};
    D3D12_RECT fullRect;

    std::shared_ptr<AssetsLoader> assets;

    custom_unordered_map<std::wstring, std::shared_ptr<GModel>> models = MemoryAllocator::CreateUnorderedMap<
        std::wstring, std::shared_ptr<GModel>>();
    std::shared_ptr<GRootSignature> primeDeviceSignature;
    std::shared_ptr<GRootSignature> ssaoPrimeRootSignature;
    std::vector<D3D12_INPUT_ELEMENT_DESC> defaultInputLayout{};
    GDescriptor srvTexturesMemory;
    RenderModeFactory defaultPrimePipelineResources;


    bool isStopRequested = false;

    const int StatisticsStepSecondsCount = 120;


    std::shared_ptr<ShadowMap> shadowPath;
    std::shared_ptr<SSAO> ambientPrimePath;
    std::shared_ptr<SSAA> antiAliasingPrimePath;

    custom_vector<std::shared_ptr<GameObject>> gameObjects = MemoryAllocator::CreateVector<std::shared_ptr<
        GameObject>>();

    custom_vector<custom_vector<std::shared_ptr<Renderer>>> typedRenderer = MemoryAllocator::CreateVector<custom_vector<
        std::shared_ptr<Renderer>>>();

    bool UseCrossAdapter = false;
    bool UseCrossSync = false;

    VoxelLodArray voxelLods{};
    VoxelExecutionMode executionMode = VoxelExecutionMode::PrimaryOnly;
    bool splitMultiGpuAvailable = false;
    std::wstring splitMultiGpuStatus = L"SplitMultiGpu is not initialized";
    UINT64 primaryComputeQueueFenceValue = 0;
    UINT64 secondaryComputeQueueFenceValue = 0;
    UINT64 crossAdapterDataReadyFenceValue = 0;
    UINT64 graphicsPassFenceValue = 0;
    uint64_t simulationFrameIndex = 0;
    VoxelSimulationScheduler voxelScheduler;
    RenderPipeline renderPipeline;
    VoxelRenderPasses voxelRenderPasses;
    VoxelWaterfallDebugPanel debugPanel;
    SceneFactory sceneFactory;
    Win32InputRouter inputRouter;

    VoxelBenchmarkProfiler benchmarkProfiler;
    BenchmarkController benchmarkController;
    std::chrono::steady_clock::time_point cpuFrameStart{};
    double currentPrimaryWaitMs = 0.0;
    double currentSecondaryWaitMs = 0.0;

    GDescriptor imguiSrvMemory;
    bool imguiInitialized = false;

    ComPtr<ID3D12Fence> primeComputeFence;
    ComPtr<ID3D12Fence> secondComputeFence;
    UINT64 sharedComputeFenceValue = 0;

    ComPtr<ID3D12Fence> primeRenderFence;
    ComPtr<ID3D12Fence> secondRenderFence;
    UINT64 sharedRenderFenceValue = 0;

    PassConstants mainPassCB;
    PassConstants shadowPassCB;

    custom_vector<std::shared_ptr<FrameResource>> frameResources = MemoryAllocator::CreateVector<std::shared_ptr<
        FrameResource>>();
    std::shared_ptr<FrameResource> currentFrameResource = nullptr;
    std::atomic<UINT> currentFrameResourceIndex = 0;

    custom_vector<Light*> lights = MemoryAllocator::CreateVector<Light*>();

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    Vector3 mLightPosW;
    Matrix mLightView = Matrix::Identity;
    Matrix mLightProj = Matrix::Identity;
    Matrix mShadowTransform = Matrix::Identity;

    float mLightRotationAngle = 0.0f;
    Vector3 mBaseLightDirections[3] = {
        Vector3(0.57735f, -0.57735f, 0.57735f),
        Vector3(-0.57735f, -0.57735f, 0.57735f),
        Vector3(0.0f, -0.707f, -0.707f)
    };
    Vector3 mRotatedLightDirections[3];

    DirectX::BoundingSphere mSceneBounds;
};
