#pragma once

#include "Emitter.h"
#include "GBuffer.h"
#include "GDescriptor.h"
#include "Source/Voxels/VoxelPartitionGpuResources.h"
#include "Source/Voxels/VoxelTypes.h"

#include <vector>

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

    DWORD nextSpawnIndex = 0;
    bool isWorked = false;
    DWORD lastDispatchVoxelCount = 0;
    DWORD lastRecycledVoxelCount = 0;
    DWORD lastAliveVoxelCount = 0;
    DWORD recordedAliveVoxelCount = 0;
    bool simulationStatsResetPending = true;
    bool simulationEnabled = true;
    bool renderEnabled = true;
    std::vector<DWORD> globalVoxelIds;

    double CalculateGroupCount(DWORD particleCount) const;
    VoxelParticleData GenerateVoxelParticle(DWORD index) const;
    void CreatePipelineState();
    void CreateDescriptors();
    void CreateBuffers();

protected:
    void Update() override;
    void Draw(const std::shared_ptr<GCommandList>& cmdList) override;

public:
    VoxelGpuPartition(const std::shared_ptr<GDevice>& owningDevice, DWORD particleCount,
                      const VoxelSimulationParameters& initialParameters = {});
    VoxelGpuPartition(const std::shared_ptr<GDevice>& owningDevice, std::vector<DWORD> voxelIds,
                      const VoxelSimulationParameters& initialParameters = {});

    void Dispatch(const std::shared_ptr<GCommandList>& cmdList) override;
    void Reset(const std::shared_ptr<GDevice>& owningDevice,
               const std::vector<DWORD>& voxelIds,
               const VoxelSimulationParameters& newParameters);
    void Initialize();
    void DispatchSimulation(const std::shared_ptr<GCommandList>& cmdList);
    void RecordRender(const std::shared_ptr<GCommandList>& cmdList,
                      VoxelPartitionRenderOutputMode outputMode = VoxelPartitionRenderOutputMode::PrimaryColor,
                      const PEPEngine::Graphics::GBuffer* passConstants = nullptr);
    void UpdateFrameConstants();
    void BeginSimulationFrame();
    void ChangeParticleCount(UINT count);
    void ApplySettings(UINT count, const VoxelSimulationParameters& newParameters);
    void ApplySettings(const std::vector<DWORD>& voxelIds, const VoxelSimulationParameters& newParameters);
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
    UINT GetLastDispatchVoxelCount() const;
    UINT GetLastRecycledVoxelCount() const;
    UINT GetLastAliveVoxelCount() const;
    UINT GetExpectedVoxelCount() const;
    VoxelPartitionStatistics GetStatistics() const;
    std::shared_ptr<GDevice> GetOwningDevice() const;

    uint32_t UpdateInterval = 1;
    uint64_t LastSimulationFrame = 0;
};
