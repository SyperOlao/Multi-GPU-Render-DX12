#include "Source/Voxels/VoxelResearchEnvironmentGenerator.h"

#include "Source/Voxels/VoxelSceneWorkload.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace
{
    struct Cell
    {
        int32_t X = 0;
        int32_t Y = 0;
        int32_t Z = 0;
        uint32_t MaterialId = 0;
    };

    struct OccupancyGrid
    {
        VoxelResearchEnvironmentGenerator::GridDesc Desc{};
        std::vector<uint8_t> Occupied;
        std::vector<uint8_t> Material;

        size_t Index(const int32_t x, const int32_t y, const int32_t z) const
        {
            return static_cast<size_t>(x + Desc.Width * (y + Desc.Height * z));
        }

        bool InBounds(const int32_t x, const int32_t y, const int32_t z) const
        {
            return x >= 0 && y >= 0 && z >= 0 &&
                x < Desc.Width && y < Desc.Height && z < Desc.Depth;
        }

        bool IsOccupied(const int32_t x, const int32_t y, const int32_t z) const
        {
            return InBounds(x, y, z) && Occupied[Index(x, y, z)] != 0;
        }

        void Set(const int32_t x, const int32_t y, const int32_t z, const uint8_t materialId)
        {
            if (!InBounds(x, y, z))
                return;
            const auto index = Index(x, y, z);
            Occupied[index] = 1;
            Material[index] = materialId;
        }

        void Clear(const int32_t x, const int32_t y, const int32_t z)
        {
            if (InBounds(x, y, z))
                Occupied[Index(x, y, z)] = 0;
        }
    };

    uint32_t HashSeed(const uint32_t value)
    {
        uint32_t x = value;
        x ^= x >> 16u;
        x *= 0x7feb352du;
        x ^= x >> 15u;
        x *= 0x846ca68bu;
        x ^= x >> 16u;
        return x;
    }

    int32_t Variation(const uint32_t seed, const uint32_t salt, const int32_t range)
    {
        if (range <= 0)
            return 0;
        return static_cast<int32_t>(HashSeed(seed ^ salt) % static_cast<uint32_t>(range * 2 + 1)) - range;
    }

    void FillBox(
        OccupancyGrid& grid,
        const int32_t minX,
        const int32_t minY,
        const int32_t minZ,
        const int32_t maxX,
        const int32_t maxY,
        const int32_t maxZ,
        const uint8_t materialId)
    {
        for (int32_t z = minZ; z <= maxZ; ++z)
            for (int32_t y = minY; y <= maxY; ++y)
                for (int32_t x = minX; x <= maxX; ++x)
                    grid.Set(x, y, z, materialId);
    }

    void ClearBox(
        OccupancyGrid& grid,
        const int32_t minX,
        const int32_t minY,
        const int32_t minZ,
        const int32_t maxX,
        const int32_t maxY,
        const int32_t maxZ)
    {
        for (int32_t z = minZ; z <= maxZ; ++z)
            for (int32_t y = minY; y <= maxY; ++y)
                for (int32_t x = minX; x <= maxX; ++x)
                    grid.Clear(x, y, z);
    }

    void FillCylinder(
        OccupancyGrid& grid,
        const int32_t centerX,
        const int32_t centerZ,
        const int32_t radius,
        const int32_t minY,
        const int32_t maxY,
        const uint8_t materialId)
    {
        const int32_t radiusSq = radius * radius;
        for (int32_t z = centerZ - radius; z <= centerZ + radius; ++z)
        {
            for (int32_t x = centerX - radius; x <= centerX + radius; ++x)
            {
                const int32_t dx = x - centerX;
                const int32_t dz = z - centerZ;
                if (dx * dx + dz * dz > radiusSq)
                    continue;
                for (int32_t y = minY; y <= maxY; ++y)
                    grid.Set(x, y, z, materialId);
            }
        }
    }

    bool IsSurfaceCell(const OccupancyGrid& grid, const int32_t x, const int32_t y, const int32_t z)
    {
        static constexpr int32_t Offsets[6][3] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
        };

        for (const auto& offset : Offsets)
        {
            if (!grid.IsOccupied(x + offset[0], y + offset[1], z + offset[2]))
                return true;
        }
        return false;
    }

    uint32_t PackGridCoordinate(const int32_t x, const int32_t y, const int32_t z)
    {
        return (static_cast<uint32_t>(x) & 0x3ffu) |
            ((static_cast<uint32_t>(y) & 0x3ffu) << 10u) |
            ((static_cast<uint32_t>(z) & 0x3ffu) << 20u);
    }

    void BuildCanyon(OccupancyGrid& grid, const uint32_t seed)
    {
        const int32_t w = grid.Desc.Width;
        const int32_t h = grid.Desc.Height;
        const int32_t d = grid.Desc.Depth;
        const int32_t cx = w / 2;
        const int32_t cz = d / 2;

        const int32_t floorLift = Variation(seed, 11u, 1);
        for (int32_t z = 3; z < d - 3; ++z)
        {
            for (int32_t x = 3; x < w - 3; ++x)
            {
                int32_t height = 2 + floorLift;
                if (z > d / 3)
                    height += 1;
                if (std::abs(x - cx) > w / 4)
                    height += 1;
                if ((x > w / 6 && x < w / 3 && z > d / 2) ||
                    (x > 2 * w / 3 && z > d / 3 && z < 2 * d / 3))
                    height += 2;
                FillBox(grid, x, 0, z, x, std::clamp(height, 1, h - 1), z, 1);
            }
        }

        const int32_t rearZ = d - 9;
        FillBox(grid, 3, 0, rearZ, w - 4, h - 3, d - 4, 2);
        ClearBox(grid, cx - 4, 4, rearZ - 1, cx + 4, h - 7, d - 2);
        FillBox(grid, cx - 9, h - 8, rearZ - 2, cx + 9, h - 6, d - 5, 2);
        FillBox(grid, cx - 14, h - 5, rearZ - 1, cx - 10, h - 3, d - 4, 2);
        FillBox(grid, cx + 10, h - 6, rearZ - 1, cx + 14, h - 3, d - 4, 2);

        const int32_t basinCenterZ = rearZ - std::max<int32_t>(8, d / 12);
        const int32_t basinRadiusX = std::max<int32_t>(6, w / 10);
        const int32_t basinRadiusZ = std::max<int32_t>(5, d / 12);
        for (int32_t z = basinCenterZ - basinRadiusZ; z <= basinCenterZ + basinRadiusZ + 4; ++z)
        {
            for (int32_t x = cx - basinRadiusX; x <= cx + basinRadiusX; ++x)
            {
                const int32_t dx = std::abs(x - cx);
                const int32_t dz = std::abs(z - basinCenterZ);
                if (dx * dx * basinRadiusZ * basinRadiusZ + dz * dz * basinRadiusX * basinRadiusX >
                    basinRadiusX * basinRadiusX * basinRadiusZ * basinRadiusZ)
                    continue;
                ClearBox(grid, x, 2, z, x, 6, z);
                FillBox(grid, x, 0, z, x, 1, z, 3);
            }
        }
        FillBox(grid, cx - basinRadiusX - 3, 2, basinCenterZ - basinRadiusZ - 3,
                cx + basinRadiusX + 3, 3, basinCenterZ + basinRadiusZ + 7, 3);

        FillBox(grid, 0, 0, 8, 6 + Variation(seed, 23u, 1), h - 5, d - 8, 2);
        FillBox(grid, w - 8 + Variation(seed, 31u, 1), 0, 4, w - 1, h - 9, d - 12, 2);
        FillBox(grid, 6, h - 8, d / 3, 12, h - 5, d / 3 + 18, 2);
        FillBox(grid, w - 14, h - 10, d / 2, w - 6, h - 7, d / 2 + 16, 2);

        FillCylinder(grid, cx - w / 4, cz - d / 5, 4, 3, h / 2, 4);
        FillCylinder(grid, cx + w / 6, cz + d / 12, 5, 3, (2 * h) / 3, 4);
        FillCylinder(grid, cx + w / 4, cz + d / 3, 4, 4, h - 6, 4);

        FillBox(grid, cx - 18, 10, cz - 2, cx + 18, 13, cz + 2, 4);
        ClearBox(grid, cx - 12, 3, cz - 3, cx + 12, 10, cz + 3);

        for (int32_t step = 0; step < 14; ++step)
        {
            FillBox(grid, 8 + step * 2, 3 + step / 2, 8 + step, 17 + step * 2, 3 + step / 2, 10 + step, 1);
        }

        ClearBox(grid, w - 18, 4, rearZ - 11, w - 8, 12, rearZ - 4);
        ClearBox(grid, 2, 5, d / 2 - 6, 10, 13, d / 2 + 8);

        FillBox(grid, w / 3, h - 11, d / 2 + 4, w - 12, h - 7, d / 2 + 16, 2);
        ClearBox(grid, w / 3 + 5, h - 12, d / 2 + 8, w - 18, h - 9, d / 2 + 13);
    }
}

uint32_t VoxelResearchEnvironmentGenerator::BudgetForPreset(const StaticVoxelBudgetPreset preset)
{
    switch (preset)
    {
    case StaticVoxelBudgetPreset::Small:
        return 100000;
    case StaticVoxelBudgetPreset::Medium:
        return 250000;
    case StaticVoxelBudgetPreset::Large:
        return 500000;
    case StaticVoxelBudgetPreset::VeryLarge:
        return 1000000;
    default:
        return 100000;
    }
}

VoxelResearchEnvironmentGenerator::GridDesc VoxelResearchEnvironmentGenerator::SelectGrid(
    const uint32_t staticVoxelBudget)
{
    const float scale = std::sqrt(static_cast<float>(std::max<uint32_t>(staticVoxelBudget, 100000)) / 100000.0f);
    GridDesc desc{};
    desc.Width = static_cast<int32_t>(std::clamp(std::lround(160.0f * scale), 160l, 520l));
    desc.Depth = static_cast<int32_t>(std::clamp(std::lround(132.0f * scale), 132l, 440l));
    desc.Height = static_cast<int32_t>(std::clamp(std::lround(72.0f * scale), 72l, 256l));
    desc.Origin = {-desc.Width / 2, 0, -(desc.Depth - 9)};
    return desc;
}

VoxelResearchEnvironmentGenerator::Result VoxelResearchEnvironmentGenerator::Generate(const Settings& settings)
{
    const auto desc = SelectGrid(settings.StaticVoxelBudget);
    OccupancyGrid grid{};
    grid.Desc = desc;
    grid.Occupied.assign(static_cast<size_t>(desc.Width * desc.Height * desc.Depth), 0);
    grid.Material.assign(grid.Occupied.size(), 0);
    BuildCanyon(grid, settings.Seed);

    std::vector<Cell> renderCells;
    renderCells.reserve(settings.StaticVoxelBudget);
    uint32_t occupiedCount = 0;
    uint32_t surfaceCount = 0;
    for (int32_t z = 0; z < desc.Depth; ++z)
    {
        for (int32_t y = 0; y < desc.Height; ++y)
        {
            for (int32_t x = 0; x < desc.Width; ++x)
            {
                if (!grid.IsOccupied(x, y, z))
                    continue;
                ++occupiedCount;
                const bool surface = IsSurfaceCell(grid, x, y, z);
                if (surface)
                    ++surfaceCount;
                if (settings.StorageMode == StaticVoxelStorageMode::DenseSolidStress || surface)
                    renderCells.push_back({x, y, z, grid.Material[grid.Index(x, y, z)]});
            }
        }
    }

    Result result{};
    auto& layer = result.Layer;
    layer.LayerId = 2;
    layer.LayerType = VoxelSceneLayerType::Static;
    layer.DisplayName = "StaticVoxelEnvironmentLayer";
    layer.SimulationPolicy = {false, 1};
    layer.SimulationParameters.VoxelSize = settings.VoxelSize;
    layer.SimulationParameters.FloorHeight = 0.0f;
    layer.SimulationParameters.SpawnHeight = static_cast<float>(desc.Height) * settings.VoxelSize;
    layer.SimulationParameters.WaterfallWidth = static_cast<float>(desc.Width) * settings.VoxelSize;
    layer.SimulationParameters.WaterfallDepth = static_cast<float>(desc.Depth) * settings.VoxelSize;
    layer.SimulationParameters.Gravity = 0.0f;
    layer.SimulationParameters.InitialFallSpeed = 0.0f;
    layer.SimulationParameters.Seed = settings.Seed;
    layer.RenderSettings.Color = DirectX::SimpleMath::Vector4(0.44f, 0.50f, 0.45f, 1.0f);
    layer.RenderSettings.VoxelSize = settings.VoxelSize;
    layer.SpatialLodPolicy.Settings = settings.SpatialLod;
    layer.SpatialLodPolicy.ChunkSize = settings.ChunkSize;
    layer.BoundsMin = DirectX::SimpleMath::Vector3(
        static_cast<float>(desc.Origin.X) * settings.VoxelSize,
        0.0f,
        static_cast<float>(desc.Origin.Z) * settings.VoxelSize);
    layer.BoundsMax = DirectX::SimpleMath::Vector3(
        static_cast<float>(desc.Origin.X + desc.Width) * settings.VoxelSize,
        static_cast<float>(desc.Height) * settings.VoxelSize,
        static_cast<float>(desc.Origin.Z + desc.Depth) * settings.VoxelSize);

    std::unordered_set<VoxelChunkId> chunks;
    chunks.reserve(renderCells.size() / 32u);
    const auto chunkWidth = std::max<uint32_t>(1, settings.ChunkSize.Width);
    const auto chunkHeight = std::max<uint32_t>(1, settings.ChunkSize.Height);
    const auto chunkDepth = std::max<uint32_t>(1, settings.ChunkSize.Depth);
    const uint32_t chunkGridX = (static_cast<uint32_t>(desc.Width) + chunkWidth - 1u) / chunkWidth;
    const uint32_t chunkGridY = (static_cast<uint32_t>(desc.Height) + chunkHeight - 1u) / chunkHeight;
    const uint32_t chunkGridZ = (static_cast<uint32_t>(desc.Depth) + chunkDepth - 1u) / chunkDepth;

    for (DWORD sequenceId = 0; sequenceId < renderCells.size(); ++sequenceId)
    {
        const auto& cell = renderCells[sequenceId];
        const auto gridCoordinate = VoxelGridCoordinate{cell.X, cell.Y, cell.Z};
        const uint32_t chunkX = static_cast<uint32_t>(cell.X) / chunkWidth;
        const uint32_t chunkY = static_cast<uint32_t>(cell.Y) / chunkHeight;
        const uint32_t chunkZ = static_cast<uint32_t>(cell.Z) / chunkDepth;
        const auto chunkId = VoxelSceneWorkloadBuilder::EncodeChunkId(layer.LayerId, chunkX, chunkY, chunkZ);
        const uint32_t chunkLinearIndex = chunkX + chunkGridX * (chunkY + chunkGridY * chunkZ);
        const auto owner = VoxelSceneWorkloadBuilder::ChooseChunkOwner(
            settings.PartitionStrategy,
            settings.LoadBalanceScenario,
            chunkId,
            chunkLinearIndex,
            chunkX,
            chunkY,
            chunkZ,
            chunkGridX,
            chunkGridY,
            chunkGridZ,
            settings.SecondaryShare);
        const auto globalId = VoxelSceneWorkloadBuilder::EncodeSequenceGlobalVoxelId(layer.LayerId, sequenceId);

        layer.GlobalVoxelIds.push_back(globalId);
        layer.SimulationVoxelIds.push_back(sequenceId);
        layer.ChunkIds.push_back(chunkId);
        layer.GridCoordinates.push_back(gridCoordinate);
        layer.MaterialIds.push_back(cell.MaterialId);
        chunks.insert(chunkId);

        auto& stream = layer.AdapterPartitions[static_cast<size_t>(owner)];
        stream.LayerId = layer.LayerId;
        stream.LayerType = layer.LayerType;
        stream.GridOrigin = desc.Origin;
        stream.SimulationParameters = layer.SimulationParameters;
        stream.RenderSettings = layer.RenderSettings;
        stream.SimulationPolicy = layer.SimulationPolicy;
        stream.SpatialLodPolicy = layer.SpatialLodPolicy;
        stream.GlobalVoxelIds.push_back(globalId);
        stream.SimulationVoxelIds.push_back(sequenceId);
        stream.ChunkIds.push_back(chunkId);
        stream.GridCoordinates.push_back(gridCoordinate);
        stream.MaterialIds.push_back(cell.MaterialId);
    }

    result.Telemetry.OccupiedCells = occupiedCount;
    result.Telemetry.SurfaceVoxels = surfaceCount;
    result.Telemetry.HiddenInteriorVoxels = occupiedCount - surfaceCount;
    result.Telemetry.ChunkCount = static_cast<uint32_t>(chunks.size());
    result.Telemetry.PrimaryStaticVoxels =
        layer.AdapterPartitions[static_cast<size_t>(VoxelAdapterPartitionId::PrimaryPartition)].VoxelCount();
    result.Telemetry.SecondaryStaticVoxels =
        layer.AdapterPartitions[static_cast<size_t>(VoxelAdapterPartitionId::SecondaryPartition)].VoxelCount();
    result.Telemetry.ActualRenderedStaticVoxels = layer.LogicalVoxelCount();
    result.Telemetry.GenerationSeed = settings.Seed;
    result.Telemetry.StorageMode = settings.StorageMode;
    return result;
}
