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
#include "Source/Research/ResearchRunnerTypes.h"
#include "Source/Scene/VoxelResearchCameraController.h"
#include "Source/Scene/VoxelResearchSceneManager.h"
#include "Source/UI/VoxelWaterfallDebugPanel.h"
#include "Source/Validation/TwoAdapterVerification.h"
#include "Source/Validation/VoxelVisualValidationRunner.h"
#include "Source/Voxels/VoxelSimulationScheduler.h"

#include <array>
#include <chrono>
#include <fstream>
#include <optional>
#include <vector>

class VoxelWaterfallApp :
    public Common::D3DApp
{
public:
    VoxelWaterfallApp(HINSTANCE hInstance);
    ~VoxelWaterfallApp() override;

    bool Initialize() override;

    int Run() override;
    int RunValidationSuiteOnce(const std::filesystem::path& outputDirectory = {});
    int RunTwoAdapterVerificationOnce(const std::filesystem::path& outputDirectory = {});
    int RunAutomaticBenchmarkSuiteOnce(BenchmarkSuite suite,
                                       uint32_t seedOverride = 0,
                                       uint32_t repetitionOverride = 0,
                                       const std::filesystem::path& outputDirectory = {});
    int RunMemorySoakTestOnce(uint32_t durationSeconds = 600,
                              const std::filesystem::path& outputDirectory = {});
    int RunMemoryRebuildStressTestOnce(uint32_t rebuildCycles = 100,
                                       uint32_t stableSeconds = 60,
                                       const std::filesystem::path& outputDirectory = {});
    int RunProfileSweepOnce(uint32_t seedOverride = 0,
                            uint32_t warmupFrames = 30,
                            uint32_t measuredFrames = 120,
                            uint32_t repetitions = 1,
                            const std::filesystem::path& outputDirectory = {});
    int RunRuntimeMutationStressTestOnce(uint32_t frameCount = 1000,
                                         const std::filesystem::path& outputDirectory = {});
    int RunAddressSanitizerRuntimeMutationScenarioOnce(const std::filesystem::path& outputDirectory = {});

protected:
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt) override;
    void UpdateAfterFrameResourceAcquire(const GameTimer& gt);
    bool DrawFrame(const GameTimer& gt);
    bool TryAcquireCurrentFrameResource();
    bool PumpOneFrame();
    bool AdvanceRuntimeWorkOutsideFrame(bool presentedFrame);
    uint32_t DrainPendingWin32Messages();

    void InitDevices();
    void InitUserInterface();
    void DrawUserInterface(const std::shared_ptr<GCommandList>& cmdList);
    void StartManualBenchmark();
    void StopManualBenchmark();
    void RunVisualValidation(const std::filesystem::path& outputDirectory = {},
                             BenchmarkSuite suite = BenchmarkSuite::Smoke,
                             uint32_t seedOverride = 0);
    VoxelVisualValidationComparisonInput CaptureVisualValidationCase(
        const VoxelVisualValidationConfig& config,
        const VoxelVisualValidationCase& validationCase,
        const std::filesystem::path& outputDirectory);
    void RequestResearchRunner(const ResearchRunnerRequest& request);
    void CancelResearchRunner();
    void AdvanceResearchRunner(bool presentedFrame);
    void RequestApplyVoxelWorkloadSettings();
    void RequestSecondaryShare(float secondaryShare);
    void RequestVisualValidation(const std::filesystem::path& outputDirectory = {},
                                 BenchmarkSuite suite = BenchmarkSuite::Smoke,
                                 uint32_t seedOverride = 0);
    void ApplyPendingRuntimeChangesAtFrameBoundary();
    void ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile profile);
    void ApplyResearchCameraMode(VoxelResearchCameraMode mode);
    void ApplyResearchLightingPreset(VoxelResearchLightingPreset preset);
    void ApplyRenderResolutionPreset(VoxelRenderResolutionPreset preset);
    bool HandleDemoPresetHotkey(WPARAM key);
    void ResetBenchmarkDeterministicState();
    void ApplyPendingVoxelSettings();
    void ApplyExecutionMode(VoxelExecutionMode requestedMode);
    void ResetVoxelRuntimeAfterPartitionRebuild(bool resetTimeline);
    void RebuildGpuPartitionsForMode();
    VoxelFrameRenderPlan BuildVoxelFrameRenderPlan() const;
    void ValidateVoxelFrameRenderPlan(const VoxelFrameRenderPlan& renderPlan) const;
    void ValidateVoxelFrameDrawResultsCheap(const VoxelFrameRenderPlan& renderPlan,
                                            const std::vector<VoxelPartitionRenderResult>& primaryResults,
                                            const std::vector<VoxelPartitionRenderResult>& secondaryResults,
                                            bool secondaryGraphicsSubmitted) const;
    void ValidateVoxelWorkloadGlobalIdsSlow() const;
    std::string GetExecutionModeName() const;
    std::string GetExecutionModeName(VoxelExecutionMode mode) const;
    float RenderAspectRatio() const;
    VoxelBenchmarkProfiler::FrameMetadata BuildBenchmarkMetadata() const;
    void RefreshBenchmarkFrameTelemetry(VoxelBenchmarkProfiler::FrameMetadata& metadata) const;
    BenchmarkControllerContext BuildBenchmarkControllerContext();
    void InitializeBenchmarkProvenanceCache();
    void PumpOneMemoryAuditFrame();
    void ServiceDeferredResourceLifetime();
    void DrainD3D12InfoQueues(uint64_t frameIndex, const std::wstring& phase);
    bool ShouldCaptureFinalOutputDiagnostics() const;
    void RecordFinalOutputDiagnosticReadbacks(
        const std::shared_ptr<PEPEngine::Graphics::GCommandList>& cmdList,
        PEPEngine::Graphics::GTexture* primaryCompositeSource,
        PEPEngine::Graphics::GTexture& backBuffer,
        FinalResolveSource resolveSource,
        uint64_t sourceGeneration);
    void AttachFinalOutputDiagnosticFence(uint64_t frameIndex, UINT64 fenceValue);
    void AttachFinalOutputDiagnosticPresentResult(uint64_t frameIndex, HRESULT presentResult);
    void ProcessCompletedFinalOutputDiagnostics();
    void RetireOffscreenRenderPaths(std::shared_ptr<SSAO> ambientPath,
                                    std::shared_ptr<SSAA> antiAliasingPath);
    void RetireCurrentMultiGpuVoxelRenderTargets();
    void RetireVoxelGpuPartitions(std::vector<std::shared_ptr<VoxelGpuPartition>> partitions);
    void RetireDescriptorOwner(PEPEngine::Graphics::GDescriptor descriptor);
    void StartAutomaticBenchmark();
    void StartAutomaticBenchmark(BenchmarkSuite suite, uint32_t seedOverride = 0,
                                 uint32_t repetitionOverride = 0);
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
    MultiGpuVoxelRenderTargetDesc BuildMultiGpuVoxelRenderTargetDesc(UINT width,
                                                                      UINT height,
                                                                      DXGI_FORMAT colorFormat) const;
    bool ValidateOffscreenRenderTargetBudget(UINT renderWidth, UINT renderHeight,
                                             std::wstring& failureReason);
    bool RebuildOffscreenVoxelRenderTargets();
    bool RebuildMultiGpuVoxelRenderTargets();
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

    struct PendingRuntimeChanges
    {
        struct VisualValidationRequest
        {
            std::filesystem::path OutputDirectory;
            BenchmarkSuite Suite = BenchmarkSuite::Smoke;
            uint32_t SeedOverride = 0;
        };

        std::optional<VoxelResearchWorkloadProfile> WorkloadProfile;
        std::optional<VoxelResearchCameraMode> CameraMode;
        std::optional<VoxelResearchLightingPreset> LightingPreset;
        std::optional<VoxelRenderResolutionPreset> RenderResolutionPreset;
        std::optional<VoxelExecutionMode> ExecutionMode;
        std::optional<bool> VoxelWorkloadSettings;
        std::optional<float> SecondaryShare;
        std::optional<FinalResolveSource> FinalResolve;
        std::optional<VisualValidationRequest> RunVisualValidation;

        bool HasAny() const
        {
            return WorkloadProfile.has_value() ||
                CameraMode.has_value() ||
                LightingPreset.has_value() ||
                RenderResolutionPreset.has_value() ||
                ExecutionMode.has_value() ||
                VoxelWorkloadSettings.has_value() ||
                SecondaryShare.has_value() ||
                FinalResolve.has_value() ||
                RunVisualValidation.has_value();
        }
    };

    VoxelSceneWorkload voxelWorkload{};
    PendingRuntimeChanges pendingRuntimeChanges;
    struct RetainedVoxelFrameRenderPlan
    {
        VoxelFrameRenderPlan Plan;
        UINT64 PrimaryRenderFenceValue = 0;
    };
    std::vector<RetainedVoxelFrameRenderPlan> retainedVoxelFrameRenderPlans;
    struct DeferredGpuResourceRelease
    {
        MultiGpuVoxelRenderTargets RenderTargets;
        std::vector<std::shared_ptr<VoxelGpuPartition>> PartitionResources;
        std::shared_ptr<SSAO> AmbientPath;
        std::shared_ptr<SSAA> AntiAliasingPath;
        std::vector<PEPEngine::Graphics::GDescriptor> DescriptorOwners;
        UINT64 RequiredPrimaryRenderFenceValue = 0;
        UINT64 RequiredSecondaryRenderFenceValue = 0;
        UINT64 RequiredPrimaryComputeFenceValue = 0;
        UINT64 RequiredSecondaryComputeFenceValue = 0;
    };
    std::vector<DeferredGpuResourceRelease> deferredGpuResourceReleases;
    struct DiagnosticTextureReadback
    {
        PEPEngine::Graphics::GResource Readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
        UINT NumRows = 0;
        UINT64 RowSizeInBytes = 0;
        UINT64 TotalBytes = 0;
        UINT Width = 0;
        UINT Height = 0;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
        bool Captured = false;
    };
    struct PendingFinalOutputDiagnosticReadback
    {
        uint64_t FrameIndex = 0;
        UINT FrameResourceIndex = 0;
        FinalResolveSource ResolveSource = FinalResolveSource::PrimaryComposite;
        uint64_t SourceGeneration = 0;
        UINT64 RequiredPrimaryFenceValue = 0;
        HRESULT PresentResult = S_OK;
        bool PresentResultRecorded = false;
        DiagnosticTextureReadback Source;
        DiagnosticTextureReadback BackBuffer;
    };
    struct FinalOutputDiagnosticMeasurement
    {
        uint64_t FrameIndex = 0;
        FinalResolveSource ResolveSource = FinalResolveSource::PrimaryComposite;
        uint64_t SourceGeneration = 0;
        uint64_t SourceHash = 0;
        double SourceNonBlackRatio = 0.0;
        uint64_t BackBufferHash = 0;
        double BackBufferNonBlackRatio = 0.0;
        HRESULT PresentResult = S_OK;
        bool SourceMeasured = false;
        bool BackBufferMeasured = false;
        std::string Interpretation;
    };
    std::vector<PendingFinalOutputDiagnosticReadback> pendingFinalOutputDiagnosticReadbacks;
    std::optional<FinalOutputDiagnosticMeasurement> latestFinalOutputDiagnosticMeasurement;
    std::ofstream finalOutputDiagnosticCsv;
    bool finalOutputDiagnosticCsvHeaderWritten = false;
    uint32_t consecutiveBlackBackbufferWithNonBlackComposite = 0;
    bool runtimeMutationStressActive = false;
    uint64_t d3d12ErrorOrCorruptionMessageCount = 0;
    uint64_t sceneGeneration = 0;
    uint64_t partitionGeneration = 0;
    uint64_t renderTargetGeneration = 0;
    uint64_t descriptorGeneration = 0;
    VoxelExecutionMode requestedExecutionMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode executionMode = VoxelExecutionMode::SingleGpuFull;
    bool isDrawingFrame = false;
    bool isPumpingFrame = false;
    uint32_t currentFramePumpDepth = 0;
    uint32_t maximumObservedFramePumpDepth = 0;
    uint64_t rejectedRecursiveFrameRequests = 0;
    bool multiGpuAvailable = false;
    CrossAdapterTransferMode crossAdapterTransferMode = CrossAdapterTransferMode::Unavailable;
    bool multiGpuPublicationEligible = false;
    std::wstring multiGpuStatus = L"MultiGpu is not initialized";
    std::vector<std::wstring> adapterReportLines;
    MultiGpuVoxelRenderTargets multiGpuVoxelRenderTargets;
    struct OffscreenRenderTargetBudget
    {
        UINT RenderWidth = 0;
        UINT RenderHeight = 0;
        UINT SsaaSampleMultiplier = 1;
        UINT SsaaLinearScale = 1;
        UINT SsaaWidth = 0;
        UINT SsaaHeight = 0;
        uint64_t SsaaColorBytes = 0;
        uint64_t SsaaDepthBytes = 0;
        uint64_t CompositeBytes = 0;
        uint64_t CrossAdapterResourceBytes = 0;
        uint64_t EstimatedTotalBytes = 0;
        uint64_t EstimatedCrossAdapterBytesPerFrame = 0;
        uint64_t PrimaryBudgetLimitBytes = 0;
        uint64_t SecondaryBudgetLimitBytes = 0;
        uint64_t PrimaryRequiredBytes = 0;
        uint64_t SecondaryRequiredBytes = 0;
        std::wstring FailureReason;
    };
    OffscreenRenderTargetBudget offscreenRenderTargetBudget{};
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
    FinalResolveSource finalResolveSource = FinalResolveSource::PrimaryComposite;
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
        std::string ShaderSetHash = "Unknown: benchmark provenance cache not initialized";
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
    bool skipDestructorGpuFlush = false;
    VoxelVisualValidationRunner visualValidationRunner;
    VoxelVisualValidationMetrics visualValidationMetrics{};
    std::string visualValidationBuildHash;
    std::string visualValidationShaderHash;
    std::string visualValidationAdapterPairIdentity;
    std::string visualValidationProtocolHash;
    std::string visualValidationCaseConfigHash;
    std::string visualValidationCameraHash;
    std::string currentBenchmarkResolvedConfigHash;
    std::filesystem::path visualValidationJsonPath;
    std::filesystem::path visualValidationCsvPath;
    std::filesystem::path twoAdapterVerificationJsonPath;
    std::chrono::steady_clock::time_point cpuFrameStart{};
    std::chrono::steady_clock::time_point frameResourceBackpressureStart{};
    std::chrono::steady_clock::time_point lastSuccessfulPresentTime{};
    double currentPresentToPresentMs = 0.0;
    bool hasSuccessfulPresentTime = false;
    uint64_t frameSerial = 0;
    uint64_t mainLoopIterationCount = 0;
    uint64_t successfulPresentCount = 0;
    uint64_t totalSuccessfulPresentCount = 0;
    uint32_t interactiveMaxCatchUpSteps = 3;
    double currentPrimaryWaitMs = 0.0;
    bool currentFrameResourceReady = true;
    bool frameResourceBackpressureActive = false;
    bool pumpFrameQuitRequested = false;
    bool visualValidationFixedStepMode = false;
    uint32_t visualValidationFixedStepsPerFrame = 1;
    uint64_t totalFrameResourceBackpressurePollCount = 0;
    uint64_t currentFrameResourceBackpressurePollCount = 0;
    uint64_t primaryD3D12InfoQueueReadIndex = 0;
    uint64_t secondaryD3D12InfoQueueReadIndex = 0;
    uint32_t currentFrameDrainedMessageCount = 0;
    bool researchRunnerActive = false;
    bool researchRunnerCancelRequested = false;
    bool researchRunnerHasRequest = false;
    ResearchRunnerRequest researchRunnerRequest{};
    std::string researchRunnerPhase = "Idle";
    std::string researchRunnerBlockedReason;
    std::filesystem::path researchRunnerOutputPath;
    uint32_t researchRunnerConfigIndex = 0;
    uint32_t researchRunnerConfigTotal = 0;
    uint32_t profileSweepProfileIndex = 0;
    uint32_t profileSweepModeIndex = 0;
    uint32_t profileSweepFrameIndex = 0;
    uint32_t profileSweepRepetition = 0;
    uint64_t profileSweepStartSimulationFrameIndex = 0;
    bool profileSweepCsvOpen = false;
    std::ofstream profileSweepCsv;
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
