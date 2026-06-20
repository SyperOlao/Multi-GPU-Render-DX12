#pragma once
#include "AssetsLoader.h"
#include "CrossAdapterVoxelEmitter.h"
#include "VoxelWaterfallEmitter.h"
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
#include "VoxelBenchmarkProfiler.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <vector>

class VoxelWaterfallApp :
    public Common::D3DApp
{
public:
    enum class VoxelExecutionMode
    {
        PrimaryOnly,
        SplitMultiGpu,
        SplitMultiGpuLod
    };

    VoxelWaterfallApp(HINSTANCE hInstance);
    ~VoxelWaterfallApp() override;

    bool Initialize() override;;

    int Run() override;

protected:
    void Update(const GameTimer& gt) override;
    void PopulateShadowMapCommands(std::shared_ptr<GCommandList> cmdList);;
    void PopulateNormalMapCommands(const std::shared_ptr<GCommandList>& cmdList);
    void PopulateAmbientMapCommands(const std::shared_ptr<GCommandList>& cmdList);
    void PopulateForwardPathCommands(const std::shared_ptr<GCommandList>& cmdList);
    void PopulateDrawCommands(std::shared_ptr<GCommandList> cmdList,
                              RenderMode type);
    void PopulateInitRenderTarget(const std::shared_ptr<GCommandList>& cmdList, GTexture& renderTarget, GDescriptor* rtvMemory,
                                  UINT offsetRTV);
    void PopulateDrawFullQuadTexture(const std::shared_ptr<GCommandList>& cmdList,
                                     GDescriptor* renderTextureSRVMemory, UINT renderTextureMemoryOffset,
                                     GraphicPSO& pso);
    void Draw(const GameTimer& gt) override;

    void InitDevices();
    void InitUserInterface();
    void DrawUserInterface(const std::shared_ptr<GCommandList>& cmdList);
    void ApplyPendingVoxelSettings();
    void ApplyExecutionMode(VoxelExecutionMode requestedMode);
    std::string GetExecutionModeName() const;
    VoxelBenchmarkProfiler::FrameMetadata BuildBenchmarkMetadata() const;
    void StartAutomaticBenchmark();
    void StopAutomaticBenchmark();
    void UpdateAutomaticBenchmark();
    void StartAutomaticBenchmarkTest();
    void ApplyBenchmarkVoxelCounts(int nearCount, int mediumCount, int farCount);
    void WriteAutomaticBenchmarkSummary();
    void CreateVoxelLod(const char* displayName, const char* objectName, size_t lodIndex,
                        const Vector3& position, int count, const VoxelSimulationParameters& parameters);
    void InitFrameResource();
    void InitRootSignature();
    void InitPipeLineResource();
    void CreateMaterials();
    void InitSRVMemoryAndMaterials();
    void InitRenderPaths();
    void LoadStudyTexture();
    void LoadModels();
    void MipMasGenerate();
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


    bool IsStop = false;

    const int StatisticStepSecondsCount = 120;


    std::shared_ptr<ShadowMap> shadowPath;
    std::shared_ptr<SSAO> ambientPrimePath;
    std::shared_ptr<SSAA> antiAliasingPrimePath;

    custom_vector<std::shared_ptr<GameObject>> gameObjects = MemoryAllocator::CreateVector<std::shared_ptr<
        GameObject>>();

    custom_vector<custom_vector<std::shared_ptr<Renderer>>> typedRenderer = MemoryAllocator::CreateVector<custom_vector<
        std::shared_ptr<Renderer>>>();

    bool UseCrossAdapter = false;
    bool UseCrossSync = false;

    struct VoxelLodState
    {
        const char* DisplayName = "";
        const char* ObjectName = "";
        bool Enabled = true;
        bool SettingsPending = false;
        int VoxelCount = 1;
        uint32_t UpdateInterval = 1;
        uint64_t LastSimulationFrame = 0;
        bool UpdatedThisFrame = false;
        UINT UpdatedVoxelCount = 0;
        VoxelSimulationParameters Parameters{};
        Vector3 Position = Vector3::Zero;
        std::shared_ptr<VoxelWaterfallEmitter> Emitter;
        std::shared_ptr<CrossAdapterVoxelEmitter> CrossEmitter;
    };

    static constexpr size_t NearVoxelWaterfall = 0;
    static constexpr size_t MediumVoxelWaterfall = 1;
    static constexpr size_t FarVoxelWaterfall = 2;
    std::array<VoxelLodState, 3> voxelLods{};
    VoxelExecutionMode executionMode = VoxelExecutionMode::PrimaryOnly;
    bool splitMultiGpuAvailable = false;
    std::wstring splitMultiGpuStatus = L"SplitMultiGpu is not initialized";
    UINT64 primaryComputeQueueFenceValue = 0;
    UINT64 secondaryComputeQueueFenceValue = 0;
    UINT64 crossAdapterDataReadyFenceValue = 0;
    UINT64 graphicsPassFenceValue = 0;
    uint64_t simulationFrameIndex = 0;
    static constexpr float FixedSimulationDeltaTime = 1.0f / 60.0f;
    static constexpr float MaxSimulationDeltaTime = 1.0f / 15.0f;

    VoxelBenchmarkProfiler benchmarkProfiler;
    std::chrono::steady_clock::time_point cpuFrameStart{};
    double currentPrimaryWaitMs = 0.0;
    double currentSecondaryWaitMs = 0.0;
    bool benchmarkVSyncWasEnabled = true;
    std::filesystem::path benchmarkDirectory = L"VoxelBenchmarkResults";

    struct AutomaticBenchmarkConfig
    {
        VoxelExecutionMode Mode = VoxelExecutionMode::PrimaryOnly;
        const char* ModeName = "PrimaryOnly";
        const char* Preset = "Low";
        int NearCount = 0;
        int MediumCount = 0;
        int FarCount = 0;
        uint32_t TotalCount = 0;
    };

    std::vector<AutomaticBenchmarkConfig> automaticBenchmarkConfigs;
    std::vector<VoxelBenchmarkProfiler::BenchmarkSummary> automaticBenchmarkSummaries;
    size_t automaticBenchmarkIndex = 0;
    bool automaticBenchmarkActive = false;
    bool automaticBenchmarkStopRequested = false;
    std::filesystem::path automaticBenchmarkSummaryPath;

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
