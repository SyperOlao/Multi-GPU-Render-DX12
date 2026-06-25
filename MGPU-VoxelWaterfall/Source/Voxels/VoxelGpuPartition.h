#pragma once

#include "Emitter.h"
#include "GBuffer.h"
#include "GDescriptor.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"
#include "Source/Voxels/VoxelPartitionGpuResources.h"
#include "Source/Voxels/VoxelTypes.h"

#include <vector>
#include <wrl.h>

struct VoxelPartitionStatistics
{
    UINT LastDispatchedVoxelCount = 0;
    UINT LastRecycledVoxelCount = 0;
    UINT LastAliveVoxelCount = 0;
    UINT ExpectedVoxelCount = 0;
};

enum class VoxelPartitionRenderOutputMode
{
    PrimaryColor,
    SecondaryColorAndLinearDepth
};

class VoxelGpuPartition : public Emitter
{
    VoxelPartitionGpuResources gpuResources;
    VoxelEmitterData emitterData{};
    VoxelSimulationParameters parameters{};
    ObjectConstants objectWorldData{};
    std::shared_ptr<GraphicPSO> secondaryRenderPSO;
    std::shared_ptr<GRootSignature> lodBuildSignature;
    std::shared_ptr<ComputePSO> lodBuildPSO;
    std::shared_ptr<GShader> lodBuildShader;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawCommandSignature;
    VoxelAdapterOwner adapterOwner = VoxelAdapterOwner::Primary;
    VoxelSpatialLodSettings spatialLodSettings{};
    DirectX::SimpleMath::Vector3 spatialLodCameraPosition = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 cachedObjectPosition = DirectX::SimpleMath::Vector3::Zero;
    VoxelSpatialLodStats lastLodStats{};
    uint32_t lodStatsReadbackThrottle = 0;

    DWORD nextSpawnIndex = 0;
    bool isWorked = false;
    DWORD lastDispatchVoxelCount = 0;
    DWORD lastRecycledVoxelCount = 0;
    DWORD lastAliveVoxelCount = 0;
    DWORD recordedAliveVoxelCount = 0;
    bool simulationStatsResetPending = true;
    bool simulationEnabled = true;
    bool renderEnabled = true;
    std::vector<VoxelPartitionDrawStream> drawStreams;
    std::vector<VoxelGlobalId> globalVoxelIds;
    std::vector<DWORD> simulationVoxelIds;
    std::vector<VoxelGridCoordinate> gridCoordinates;
    std::vector<uint32_t> materialIds;

    double CalculateGroupCount(DWORD particleCount) const;
    VoxelParticleData GenerateVoxelParticle(DWORD index) const;
    VoxelParticleData GenerateStaticVoxelParticle(DWORD index) const;
    void CreatePipelineState();
    void CreateDescriptors();
    void CreateBuffers();
    bool HasVoxels() const;
    bool HasStaticStreams() const;
    bool HasDynamicStreams() const;
    void InitializeStaticParticleSet();
    void ValidateCommandListOwnership(const std::shared_ptr<GCommandList>& cmdList, const char* operation) const;
    void ValidateGpuResourceOwnership(const char* operation) const;
    void ValidateDescriptorOwnership(const PEPEngine::Graphics::GDescriptor& descriptor,
                                     const char* descriptorName,
                                     const char* operation) const;
    void ValidateBufferOwnership(const PEPEngine::Graphics::GBuffer* buffer,
                                 const char* bufferName,
                                 const char* operation) const;
    void ValidateRootSignatureOwnership(const PEPEngine::Graphics::GRootSignature* rootSignature,
                                        const char* signatureName,
                                        const char* operation) const;
    void ValidateGraphicsPsoOwnership(const PEPEngine::Graphics::GraphicPSO* pso,
                                      const char* psoName,
                                      const char* operation) const;
    void BuildLodRenderList(const std::shared_ptr<GCommandList>& cmdList, bool forceStatsReadback);
    void UpdateLodStatsReadback();

protected:
    void Update() override;
    void Draw(const std::shared_ptr<GCommandList>& cmdList) override;

public:
    VoxelGpuPartition(const std::shared_ptr<GDevice>& owningDevice, std::vector<VoxelPartitionDrawStream> streams,
                      VoxelAdapterOwner owner = VoxelAdapterOwner::Primary);

    void Dispatch(const std::shared_ptr<GCommandList>& cmdList) override;
    void Initialize();
    void DispatchSimulation(const std::shared_ptr<GCommandList>& cmdList);
    VoxelPartitionRenderResult RecordRender(
        const std::shared_ptr<GCommandList>& cmdList,
        VoxelPartitionRenderOutputMode outputMode = VoxelPartitionRenderOutputMode::PrimaryColor,
        const PEPEngine::Graphics::GBuffer* passConstants = nullptr,
        VoxelBenchmarkProfiler* benchmarkProfiler = nullptr,
        VoxelBenchmarkProfiler::QueueId profilerQueue = VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
        VoxelBenchmarkProfiler::RangeId lodCompactionRange =
            VoxelBenchmarkProfiler::RangeId::PrimaryLodCompaction);
    void UpdateFrameConstants();
    void BeginSimulationFrame();
    void ChangeParticleCount(UINT count);
    void ApplySettings(UINT count, const VoxelSimulationParameters& newParameters);
    void ApplySettings(std::vector<VoxelPartitionDrawStream> streams);
    const VoxelSimulationParameters& GetParameters() const;
    UINT GetParticleCount() const;
    VoxelEmitterData& GetEmitterData();
    const VoxelEmitterData& GetEmitterData() const;
    bool HasStartedSimulation() const;
    VoxelParticleData GenerateParticleForIndex(DWORD index) const;
    DWORD ConsumeNextSpawnIndex(DWORD count);
    double CalculateDispatchGroupCount(DWORD particleCount) const;
    void SetLastDispatchVoxelCount(UINT count);
    void SetEnabled(bool value);
    void SetRenderEnabled(bool value);
    void SetSimulationEnabled(bool value);
    bool IsEnabled() const;
    bool IsRenderEnabled() const;
    bool IsSimulationEnabled() const;
    void SetUpdateInterval(uint32_t value);
    uint32_t GetUpdateInterval() const;
    void SetLastSimulationFrame(uint64_t value);
    uint64_t GetLastSimulationFrame() const;
    void SetSimulationDeltaTime(float value);
    void SetSimulationTime(float value);
    void SetInterpolationAlpha(float value);
    void ConfigureSpatialLod(const VoxelSpatialLodSettings& settings,
                             const DirectX::SimpleMath::Vector3& cameraPosition);
    UINT GetLastDispatchVoxelCount() const;
    UINT GetLastRecycledVoxelCount() const;
    UINT GetLastAliveVoxelCount() const;
    UINT GetExpectedVoxelCount() const;
    VoxelPartitionStatistics GetStatistics() const;
    VoxelSpatialLodStats GetLastLodStats() const;
    std::shared_ptr<GDevice> GetOwningDevice() const;
    VoxelAdapterOwner GetAdapterOwner() const;
    const std::vector<VoxelGlobalId>& GetGlobalVoxelIds() const;
    const std::vector<VoxelPartitionDrawStream>& GetDrawStreams() const;

    uint32_t UpdateInterval = 1;
    uint64_t LastSimulationFrame = 0;
};
