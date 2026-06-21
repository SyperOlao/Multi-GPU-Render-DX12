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
};

struct VoxelVisualValidationSnapshot
{
    uint32_t Seed = 0;
    uint32_t TotalVoxelCount = 0;
    float SecondaryShare = 0.0f;
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    double FixedDeltaTime = 1.0 / 60.0;
    uint64_t FixedStepCount = 240;
    VoxelSpatialLodSettings SpatialLod{};
    uint32_t TemporalInterval = 1;
};

struct VoxelVisualValidationCase
{
    std::string Name;
    float SecondaryShare = 0.0f;
    bool SpatialLodEnabled = false;
    uint32_t TemporalInterval = 1;
};

struct VoxelVisualValidationMetrics
{
    bool HasResult = false;
    bool Passed = false;
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
    static void ExportCsv(const VoxelVisualValidationConfig& config,
                          const VoxelVisualValidationMetrics& metrics,
                          const std::filesystem::path& path);
    static void ExportJson(const VoxelVisualValidationConfig& config,
                           const VoxelVisualValidationMetrics& metrics,
                           const std::filesystem::path& path);
};
