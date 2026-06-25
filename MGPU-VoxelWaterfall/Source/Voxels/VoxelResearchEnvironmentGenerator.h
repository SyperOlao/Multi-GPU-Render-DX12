#pragma once

#include "Source/Voxels/VoxelTypes.h"

class VoxelResearchEnvironmentGenerator
{
public:
    struct Settings
    {
        uint32_t Seed = 1337;
        uint32_t StaticVoxelBudget = 100000;
        StaticVoxelBudgetPreset BudgetPreset = StaticVoxelBudgetPreset::Small;
        StaticVoxelStorageMode StorageMode = StaticVoxelStorageMode::SurfaceOnly;
        float VoxelSize = 0.65f;
        float SecondaryShare = 0.1f;
        VoxelChunkSize ChunkSize{};
        VoxelSpatialLodSettings SpatialLod{};
    };

    struct Result
    {
        VoxelSceneLayer Layer{};
        VoxelStaticEnvironmentTelemetry Telemetry{};
    };
    
    struct GridDesc
    {
        int32_t Width = 0;
        int32_t Height = 0;
        int32_t Depth = 0;
        VoxelGridCoordinate Origin{};
    };

    static uint32_t BudgetForPreset(StaticVoxelBudgetPreset preset);
    static Result Generate(const Settings& settings);

private:
    static GridDesc SelectGrid(uint32_t staticVoxelBudget);
};
