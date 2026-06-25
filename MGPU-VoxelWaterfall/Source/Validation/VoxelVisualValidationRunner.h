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
    uint32_t SchemaVersion = 2;
    std::string ProtocolVersion = "mgpu_voxel_visual_validation_protocol.v2";
    std::string GeneratorVersion = "mgpu_voxel_visual_validation_generator.v2";
    uint64_t SnapshotHash = 0;
    std::string ValidationRunId;
    std::string WorkloadProfile = "MixedStaticAndDynamic";
    std::string ScenePreset = "MixedVoxelEnvironment";
    uint32_t StaticSeed = 0;
    uint32_t DynamicSeed = 0;
    uint32_t RequestedStaticCount = 0;
    uint32_t ActualStaticCount = 0;
    uint32_t RequestedDynamicCount = 0;
    uint32_t ActualDynamicCount = 0;
    uint32_t TotalVoxelCount = 0;
    float StaticVoxelSize = 0.0f;
    float DynamicVoxelSize = 0.0f;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    uint32_t SampleCount = 1;
    std::string ColorFormat;
    std::string LinearDepthFormat;
    double FixedDeltaTime = 1.0 / 60.0;
    uint64_t FixedStepCount = 240;
    uint32_t WarmupStepCount = 0;
    VoxelSpatialLodSettings SpatialLod{};
    VoxelTemporalPolicy TemporalPolicy = VoxelTemporalPolicy::Decimated;
    uint32_t TemporalInterval = 2;
    float SecondaryShare = 0.5f;
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
    std::string ShaderSetHash = "unknown";
    std::string AdapterPairIdentity;
    std::string PrimaryDriverVersion;
    std::string SecondaryDriverVersion;
    std::string ProtocolHash = "unknown";
    std::string CaseConfigHash = "unknown";
    std::string CameraHash = "unknown";
};

struct VoxelVisualValidationCase
{
    std::string CaseId;
    std::string ValidationKind = "implementation_equivalence";
    std::string ModeFamily = "Full";
    std::string Suite = "Full";
    std::string Preset = "Low";
    std::string ConfigKey;
    uint32_t RequestedLabelCount = 0;
    uint32_t RequestedStaticBudget = 0;
    uint32_t RequestedDynamicBudget = 0;
    uint32_t RandomizationSeed = 0;
    float SecondaryShare = 0.5f;
    VoxelExecutionMode SingleMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode MultiMode = VoxelExecutionMode::MultiGpuFull;
    VoxelExecutionMode ReferenceMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode CandidateMode = VoxelExecutionMode::MultiGpuFull;
    bool SpatialLodEnabled = false;
    uint32_t TemporalInterval = 1;
    uint64_t FixedStepCount = 240;
    std::string CheckpointId = "steady_240";
    uint32_t CheckpointIndex = 0;
    std::string CameraMode = "FixedOverview";
    float CameraPosition[3] = {};
    float CameraTarget[3] = {};
    float View[16] = {};
    float Projection[16] = {};
    float NearZ = 0.25f;
    float FarZ = 900.0f;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    uint32_t SampleCount = 1;
    std::string ColorFormat;
    std::string LinearDepthFormat;
    uint32_t ActualStaticCount = 0;
    uint32_t ActualDynamicCount = 0;
    uint32_t ActualTotalCount = 0;
    std::string ReferenceConfigHash;
    std::string CandidateConfigHash;
    std::string ConfigHash;
    std::string CameraHash;
    std::string ProtocolHash;
};

struct VoxelValidationTileStats
{
    uint32_t PixelCount = 0;
    uint32_t ForegroundUnionCount = 0;
    uint32_t ForegroundIntersectionCount = 0;
    uint32_t ColorMismatchCount = 0;
    uint32_t DepthMismatchCount = 0;
    uint32_t CombinedMismatchCount = 0;
    uint32_t CoverageMismatchCount = 0;
    uint32_t Padding0 = 0;
    float RgbAbsoluteErrorSum = 0.0f;
    float RgbSquaredErrorSum = 0.0f;
    float RgbMaxAbsoluteError = 0.0f;
    float AlphaAbsoluteErrorSum = 0.0f;
    float DepthAbsoluteErrorSum = 0.0f;
    float DepthSquaredErrorSum = 0.0f;
    float DepthRelativeErrorSum = 0.0f;
    float DepthMaxAbsoluteError = 0.0f;
};

struct VoxelVisualValidationComparisonInput
{
    std::string CaseId;
    bool CaptureAvailable = false;
    bool CompareShaderDispatched = false;
    bool ReadbackComplete = false;
    std::string BlockedReason;
    VoxelExecutionMode ActualSingleMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ActualMultiMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode RequestedReferenceMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ActualReferenceMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode RequestedCandidateMode = VoxelExecutionMode::MultiGpuFull;
    VoxelExecutionMode ActualCandidateMode = VoxelExecutionMode::SingleGpuFull;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    uint32_t SampleCount = 1;
    std::string ColorFormat;
    std::string LinearDepthFormat;
    uint32_t ActualStaticCount = 0;
    uint32_t ActualDynamicCount = 0;
    uint32_t ActualTotalCount = 0;
    std::string ValidationKind;
    std::string CheckpointId;
    std::string ProtocolHash;
    std::string ReferenceConfigHash;
    std::string CandidateConfigHash;
    std::string ConfigHash;
    std::string CameraHash;
    std::string AdapterPairIdentity;
    std::filesystem::path SingleColorReferencePath;
    std::filesystem::path SingleDepthReferencePath;
    std::filesystem::path MultiColorReferencePath;
    std::filesystem::path MultiDepthReferencePath;
    std::filesystem::path DiffReferencePath;
    std::vector<VoxelValidationTileStats> TileStats;
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
    VoxelExecutionMode RequestedReferenceMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode ActualReferenceMode = VoxelExecutionMode::SingleGpuFull;
    VoxelExecutionMode RequestedCandidateMode = VoxelExecutionMode::MultiGpuFull;
    VoxelExecutionMode ActualCandidateMode = VoxelExecutionMode::SingleGpuFull;
    VoxelSpatialLodMode SpatialLodMode = VoxelSpatialLodMode::Off;
    uint32_t TemporalInterval = 1;
    std::string ValidationKind;
    std::string ModeFamily;
    std::string ConfigKey;
    std::string CheckpointId;
    std::string ProtocolHash;
    std::string ReferenceConfigHash;
    std::string CandidateConfigHash;
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
    std::string ConfigHash;
    std::string CameraHash;
    std::string AdapterPairIdentity;
    std::filesystem::path SingleColorReferencePath;
    std::filesystem::path SingleDepthReferencePath;
    std::filesystem::path MultiColorReferencePath;
    std::filesystem::path MultiDepthReferencePath;
    std::filesystem::path DiffReferencePath;
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
    std::vector<VoxelVisualValidationComparisonInput> CompletedComparisons;

    static std::vector<VoxelVisualValidationCase> DefaultCases();
};

class VoxelVisualValidationRunner
{
public:
    VoxelVisualValidationMetrics RunDeterministicSuite(
        const VoxelVisualValidationConfig& config,
        const std::filesystem::path& outputDirectory) const;
    static std::string BuildCanonicalProtocolJson(const VoxelVisualValidationConfig& config);
    static std::string BuildProtocolHash(const VoxelVisualValidationConfig& config);

private:
    static uint64_t ComputeSnapshotHash(const VoxelVisualValidationSnapshot& snapshot);
    static void ExportCsv(const VoxelVisualValidationConfig& config,
                          const VoxelVisualValidationMetrics& metrics,
                          const std::filesystem::path& path);
    static void ExportJson(const VoxelVisualValidationConfig& config,
                           const VoxelVisualValidationMetrics& metrics,
                           const std::filesystem::path& path);
};
