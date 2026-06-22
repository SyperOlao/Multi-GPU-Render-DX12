#pragma once
#include "AssetsLoader.h"
#include "Source/Voxels/VoxelGpuPartition.h"
#include "Source/Voxels/VoxelSceneWorkload.h"
#include "d3dApp.h"
#include "Renderer.h"
#include "RenderModeFactory.h"
#include "ShadowMap.h"
#include "SSAA.h"
#include "SSAO.h"
#include "FrameResource.h"
#include "GDeviceFactory.h"
#include "Light.h"
#include "Source/Benchmark/BenchmarkController.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Platform/Win32InputRouter.h"
#include "Source/Rendering/RenderPipeline.h"
#include "Source/Rendering/MultiGpuVoxelRenderTargets.h"
#include "Source/Rendering/VoxelCompositePass.h"
#include "Source/Rendering/VoxelRenderPasses.h"
#include "Source/Scene/VoxelResearchCameraController.h"
#include "Source/Scene/VoxelResearchSceneManager.h"
#include "Source/UI/VoxelWaterfallDebugPanel.h"
#include "Source/Validation/TwoAdapterVerification.h"
#include "Source/Validation/VoxelVisualValidationRunner.h"
#include "Source/Voxels/VoxelSimulationScheduler.h"

#include <array>
#include <chrono>
#include <vector>

class VoxelWaterfallApp :
    public Common::D3DApp
{
public:
    VoxelWaterfallApp(HINSTANCE hInstance);
    ~VoxelWaterfallApp() override;

    bool Initialize() override;

    int Run() override;
    int RunValidationSuiteOnce();
    int RunTwoAdapterVerificationOnce();
    int RunAutomaticBenchmarkSuiteOnce(BenchmarkSuite suite,
                                       uint32_t seedOverride = 0,
                                       const std::filesystem::path& outputDirectory = {});
    int RunMemorySoakTestOnce(uint32_t durationSeconds = 600,
                              const std::filesystem::path& outputDirectory = {});
    int RunMemoryRebuildStressTestOnce(uint32_t rebuildCycles = 100,
                                       uint32_t stableSeconds = 60,
                                       const std::filesystem::path& outputDirectory = {});

protected:
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt) override;

    void InitDevices();
    void InitUserInterface();
    void DrawUserInterface(const std::shared_ptr<GCommandList>& cmdList);
    void StartManualBenchmark();
    void StopManualBenchmark();
    void RunVisualValidation();
    void RequestApplyVoxelWorkloadSettings();
    void ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile profile);
    void ApplyResearchCameraMode(VoxelResearchCameraMode mode);
    void ApplyResearchLightingPreset(VoxelResearchLightingPreset preset);
    void ApplyRenderResolutionPreset(VoxelRenderResolutionPreset preset);
    bool HandleDemoPresetHotkey(WPARAM key);
    void ResetBenchmarkDeterministicState();
    void ApplyPendingVoxelSettings();
    void ApplyExecutionMode(VoxelExecutionMode requestedMode);
    void RebuildGpuPartitionsForMode();
    VoxelRenderWorkload BuildVoxelRenderWorkload() const;
    void ValidateVoxelRenderWorkload(const VoxelRenderWorkload& renderWorkload) const;
    void ValidateVoxelFrameDrawResultsCheap(const VoxelRenderWorkload& renderWorkload,
                                            const std::vector<VoxelPartitionRenderResult>& primaryResults,
                                            const std::vector<VoxelPartitionRenderResult>& secondaryResults,
                                            bool secondaryGraphicsSubmitted) const;
    void ValidateVoxelWorkloadGlobalIdsSlow() const;
    std::string GetExecutionModeName() const;
    std::string GetExecutionModeName(VoxelExecutionMode mode) const;
    VoxelBenchmarkProfiler::FrameMetadata BuildBenchmarkMetadata() const;
    void RefreshBenchmarkFrameTelemetry(VoxelBenchmarkProfiler::FrameMetadata& metadata) const;
    BenchmarkControllerContext BuildBenchmarkControllerContext();
    void InitializeBenchmarkProvenanceCache();
    void PumpOneMemoryAuditFrame();
    void ServiceDeferredResourceLifetime();
    void StartAutomaticBenchmark();
    void StartAutomaticBenchmark(BenchmarkSuite suite, uint32_t seedOverride = 0);
    void StopAutomaticBenchmark();
    BenchmarkConfigurationApplyResult ApplyBenchmarkConfigurationAtomic(const AutomaticBenchmarkConfig& config);
    void ApplyBenchmarkVoxelCount(int totalCount);
    void ApplyBenchmarkSecondaryShare(float secondaryShare);
    void ApplyBenchmarkSpatialLodEnabled(bool enabled);
    void ApplyBenchmarkTemporalInterval(uint32_t interval);
    void InitFrameResource();
    void InitRootSignature();
    void InitPipeLineResource();
    void CreateMaterials();
    void InitSRVMemoryAndMaterials();
    void InitRenderPaths();
    MultiGpuVoxelRenderTargetDesc BuildMultiGpuVoxelRenderTargetDesc() const;
    void RebuildMultiGpuVoxelRenderTargets();
    void DisableMultiGpu(const std::wstring& reason);
    void LoadStudyTexture();
    void LoadModels();
    void GenerateMipMaps();
    void SortGO();
    void CreateGO();
#if defined(DEBUG) || defined(_DEBUG)
    void RunSceneOwnershipSettingsSelfTest();
#endif
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
    std::shared_ptr<VoxelResearchCameraController> researchCameraController;

    LockThreadQueue<std::wstring> logQueue{};
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

    VoxelSceneWorkload voxelWorkload{};
    bool voxelWorkloadSettingsPending = false;
    VoxelExecutionMode requestedExecutionMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode executionMode = VoxelExecutionMode::SingleGpuFull;
    bool multiGpuAvailable = false;
    std::wstring multiGpuStatus = L"MultiGpu is not initialized";
    std::vector<std::wstring> adapterReportLines;
    MultiGpuVoxelRenderTargets multiGpuVoxelRenderTargets;
    UINT64 primaryComputeQueueFenceValue = 0;
    UINT64 secondaryComputeQueueFenceValue = 0;
    UINT64 crossAdapterRenderReadyFenceValue = 0;
    UINT64 graphicsPassFenceValue = 0;
    UINT64 lastPrimaryPartitionGraphicsFenceValue = 0;
    UINT64 lastSecondaryPartitionGraphicsFenceValue = 0;
    VoxelAdapterOwner lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Primary;
    uint64_t simulationFrameIndex = 0;
    double voxelSimulationAccumulator = 0.0;
    double voxelSimulationTime = 0.0;
    uint32_t voxelSimulationStepsThisFrame = 0;
    float voxelInterpolationAlpha = 0.0f;
    uint32_t voxelRecycledCount = 0;
    uint32_t voxelAliveCount = 0;
    uint32_t voxelExpectedCount = 0;
    VoxelFrameGraphTelemetry frameGraphTelemetry{};
    VoxelSimulationScheduler voxelScheduler;
    RenderPipeline renderPipeline;
    VoxelCompositePass voxelCompositePass;
    VoxelRenderPasses voxelRenderPasses;
    VoxelWaterfallDebugPanel debugPanel;
    VoxelResearchSceneManager voxelResearchSceneManager;
    Win32InputRouter inputRouter;

    VoxelBenchmarkProfiler benchmarkProfiler;
    BenchmarkController benchmarkController;
    struct BenchmarkProvenanceCache
    {
        bool Initialized = false;
        std::string BuildHash = "Unknown: benchmark provenance cache not initialized";
        std::string ShaderHash = "Unknown: benchmark provenance cache not initialized";
        std::string GitCommit = "Unknown: benchmark provenance cache not initialized";
        std::string GitDirtyState = "Unknown: benchmark provenance cache not initialized";
        std::string OperatingSystem = "Windows";
        std::string BuildConfiguration;
        bool D3D12DebugLayerEnabled = false;
        std::wstring PrimaryAdapterName = L"unavailable";
        std::wstring SecondaryAdapterName = L"unavailable";
        uint32_t PrimaryVendorId = 0;
        uint32_t PrimaryDeviceId = 0;
        uint64_t PrimaryDedicatedVideoMemory = 0;
        std::string PrimaryAdapterLuid;
        uint32_t SecondaryVendorId = 0;
        uint32_t SecondaryDeviceId = 0;
        uint64_t SecondaryDedicatedVideoMemory = 0;
        std::string SecondaryAdapterLuid;
    };
    BenchmarkProvenanceCache benchmarkProvenanceCache;
    mutable uint64_t benchmarkMetadataBuildCount = 0;
    mutable uint64_t slowFrameValidationCount = 0;
    TwoAdapterVerificationRunner twoAdapterVerificationRunner;
    TwoAdapterVerificationResult twoAdapterVerificationResult{};
    bool twoAdapterVerificationHasResult = false;
    VoxelVisualValidationRunner visualValidationRunner;
    VoxelVisualValidationMetrics visualValidationMetrics{};
    std::string visualValidationBuildHash;
    std::string visualValidationShaderHash;
    std::string visualValidationAdapterPairIdentity;
    std::chrono::steady_clock::time_point cpuFrameStart{};
    uint64_t frameSerial = 0;
    uint64_t mainLoopIterationCount = 0;
    uint64_t successfulPresentCount = 0;
    uint32_t interactiveMaxCatchUpSteps = 3;
    double currentPrimaryWaitMs = 0.0;
    bool currentFrameResourceReady = true;
    VoxelCompositeDebugView voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
    Vector3 spatialLodCameraPosition = Vector3::Zero;
    bool spatialLodCameraInitialized = false;

    GDescriptor imguiSrvMemory;
    bool imguiInitialized = false;

    ComPtr<ID3D12Fence> primeCrossAdapterRenderReadyFence;
    ComPtr<ID3D12Fence> secondCrossAdapterRenderReadyFence;

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

    Vector3 mBaseLightDirections[3] = {
        Vector3(0.57735f, -0.57735f, 0.57735f),
        Vector3(-0.57735f, -0.57735f, 0.57735f),
        Vector3(0.0f, -0.707f, -0.707f)
    };

    DirectX::BoundingSphere mSceneBounds;
};
