#pragma once

#include "Source/Voxels/VoxelTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct VoxelVisualValidationTolerances
{
    double ColorTolerance = 0.025;
    double DepthTolerance = 0.02;
    double MaxColorMismatchPercent = 0.5;
    double MaxDepthMismatchPercent = 0.5;
    double MaxCombinedMismatchPercent = 0.5;
    double ValidDepthMax = 1000000.0;
};

struct VoxelVisualValidationSnapshot
{
    uint32_t SchemaVersion = 1;
    uint64_t SnapshotHash = 0;
    std::string ValidationRunId;
    std::string WorkloadProfile = "MixedStaticAndDynamic";
    std::string ScenePreset = "MixedVoxelEnvironment";
    uint32_t StaticSeed = 1337;
    uint32_t DynamicSeed = 4242;
    uint32_t RequestedStaticCount = 100000;
    uint32_t ActualStaticCount = 98556;
    uint32_t RequestedDynamicCount = 25000;
    uint32_t ActualDynamicCount = 25000;
    uint32_t TotalVoxelCount = 123556;
    float StaticVoxelSize = 0.65f;
    float DynamicVoxelSize = 0.35f;
    uint32_t RenderWidth = 1920;
    uint32_t RenderHeight = 1080;
    std::string ColorFormat = "R8G8B8A8_UNORM";
    std::string LinearDepthFormat = "R32_FLOAT";
    double FixedDeltaTime = 1.0 / 60.0;
    uint64_t FixedStepCount = 240;
    uint32_t WarmupStepCount = 0;
    VoxelSpatialLodSettings SpatialLod{};
    VoxelTemporalPolicy TemporalPolicy = VoxelTemporalPolicy::Decimated;
    uint32_t TemporalInterval = 2;
    float SecondaryShare = 0.5f;
    VoxelPartitionStrategy PartitionStrategy = VoxelPartitionStrategy::HashedChunks;
    VoxelLoadBalanceScenario LoadBalanceScenario = VoxelLoadBalanceScenario::Balanced;
    VoxelChunkSize ChunkSize{};
    VoxelExecutionMode RequestedExecutionMode = VoxelExecutionMode::MultiGpuFull;
    float View[16] = {};
    float Projection[16] = {};
    float NearZ = 0.25f;
    float FarZ = 900.0f;
    std::string LightingPreset = "BenchmarkNeutral";
    bool DynamicShadowsEnabled = false;
    std::string Background = "BenchmarkNeutral";
    std::string BuildHash = "unknown";
    std::string ShaderHash = "unknown";
};

struct VoxelVisualValidationCase
{
    std::string CaseId;
    VoxelExecutionMode SingleMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode MultiMode = VoxelExecutionMode::MultiGpuFull;
    bool SpatialLodEnabled = false;
    uint32_t TemporalInterval = 1;
};

struct VoxelVisualValidationCaseResult
{
    std::string CaseId;
    uint64_t SnapshotHash = 0;
    std::string ValidationRunId;
    VoxelExecutionMode RequestedSingleMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ActualSingleMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode RequestedMultiMode = VoxelExecutionMode::MultiGpuFull;
    VoxelExecutionMode ActualMultiMode = VoxelExecutionMode::SingleGpuFull;
    VoxelSpatialLodMode SpatialLodMode = VoxelSpatialLodMode::Off;
    uint32_t TemporalInterval = 1;
    uint32_t TotalVoxelCount = 0;
    uint32_t ActualStaticCount = 0;
    uint32_t ActualDynamicCount = 0;
    uint64_t ComparedPixelCount = 0;
    uint64_t ForegroundUnionCount = 0;
    uint64_t ForegroundIntersectionCount = 0;
    uint64_t ColorMismatchCount = 0;
    uint64_t DepthMismatchCount = 0;
    uint64_t CombinedMismatchCount = 0;
    uint64_t CoverageMismatchCount = 0;
    double ColorMAE = 0.0;
    double ColorRMSE = 0.0;
    double ColorPSNR = 0.0;
    double MaxColorError = 0.0;
    double AlphaMAE = 0.0;
    double DepthMAE = 0.0;
    double DepthRMSE = 0.0;
    double DepthRelativeMAE = 0.0;
    double MaxDepthError = 0.0;
    double ColorMismatchPercent = 0.0;
    double DepthMismatchPercent = 0.0;
    double CombinedMismatchPercent = 0.0;
    double CoverageMismatchPercent = 0.0;
    bool Passed = false;
    bool Blocked = true;
    std::string Status = "BLOCKED";
    std::string Reason;
};

struct VoxelVisualValidationMetrics
{
    bool HasResult = false;
    bool Passed = false;
    bool Blocked = true;
    std::string ValidationRunId;
    uint64_t SnapshotHash = 0;
    double ColorMAE = 0.0;
    double ColorRMSE = 0.0;
    double ColorPSNR = 0.0;
    double MaxColorError = 0.0;
    double ColorMismatchPercent = 0.0;
    double DepthRMSE = 0.0;
    double DepthMismatchPercent = 0.0;
    uint32_t ActualSecondaryDrawCount = 0;
    uint64_t PipelinePrimitiveCount = 0;
    std::string FailReason;
    std::filesystem::path CsvPath;
    std::filesystem::path JsonPath;
    std::vector<VoxelVisualValidationCaseResult> CaseResults;
};

struct VoxelVisualValidationConfig
{
    VoxelVisualValidationSnapshot Snapshot{};
    VoxelVisualValidationTolerances Tolerances{};
    std::vector<VoxelVisualValidationCase> Cases;

    static std::vector<VoxelVisualValidationCase> DefaultCases();
};

class VoxelVisualValidationRunner
{
public:
    VoxelVisualValidationMetrics RunDeterministicSuite(
        const VoxelVisualValidationConfig& config,
        const std::filesystem::path& outputDirectory) const;

private:
    static uint64_t ComputeSnapshotHash(const VoxelVisualValidationSnapshot& snapshot);
    static void ExportCsv(const VoxelVisualValidationConfig& config,
                          const VoxelVisualValidationMetrics& metrics,
                          const std::filesystem::path& path);
    static void ExportJson(const VoxelVisualValidationConfig& config,
                           const VoxelVisualValidationMetrics& metrics,
                           const std::filesystem::path& path);
};
