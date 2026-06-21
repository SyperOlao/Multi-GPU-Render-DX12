#include "Source/Voxels/VoxelSceneWorkload.h"

#include "Source/Voxels/VoxelParticleSpawner.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_set>

namespace
{
    uint32_t DivideRoundUp(const uint32_t value, const uint32_t divisor)
    {
        return (value + divisor - 1u) / divisor;
    }

    VoxelLayerSimulationPolicy DynamicWaterfallSimulationPolicy(const VoxelSceneWorkload& workload)
    {
        VoxelLayerSimulationPolicy policy{};
        policy.Enabled = true;
        policy.UpdateInterval =
            workload.TemporalPolicy == VoxelTemporalPolicy::Decimated
                ? std::max<uint32_t>(2, workload.TemporalDecimationInterval)
                : 1;
        return policy;
    }

    uint32_t StableChunkSample(const uint32_t chunkLinearIndex)
    {
        uint32_t x = chunkLinearIndex * 2654435761u;
        x ^= x >> 16u;
        x *= 2246822519u;
        x ^= x >> 13u;
        return x % 10000u;
    }

    VoxelAdapterPartitionId OwnerFromNormalizedPlane(
        const float coordinate,
        const float secondaryShare,
        const bool secondaryFirst)
    {
        const float share = std::clamp(secondaryShare, 0.0f, 1.0f);
        if (share <= 0.0f)
            return VoxelAdapterPartitionId::PrimaryPartition;
        if (share >= 1.0f)
            return VoxelAdapterPartitionId::SecondaryPartition;

        if (secondaryFirst)
        {
            return coordinate < share
                       ? VoxelAdapterPartitionId::SecondaryPartition
                       : VoxelAdapterPartitionId::PrimaryPartition;
        }

        return coordinate < (1.0f - share)
                   ? VoxelAdapterPartitionId::PrimaryPartition
                   : VoxelAdapterPartitionId::SecondaryPartition;
    }

}

VoxelGlobalId VoxelSceneWorkloadBuilder::EncodeSequenceGlobalVoxelId(
    const VoxelLayerId layerId,
    const DWORD sequenceId)
{
    return (static_cast<VoxelGlobalId>(layerId) << 32u) | static_cast<VoxelGlobalId>(sequenceId);
}

VoxelChunkId VoxelSceneWorkloadBuilder::EncodeChunkId(
    const VoxelLayerId layerId,
    const uint32_t chunkX,
    const uint32_t chunkY,
    const uint32_t chunkZ)
{
    constexpr VoxelChunkId CoordinateMask = 0xFFFFu;
    return (static_cast<VoxelChunkId>(layerId) << 48u) |
        ((static_cast<VoxelChunkId>(chunkZ) & CoordinateMask) << 32u) |
        ((static_cast<VoxelChunkId>(chunkY) & CoordinateMask) << 16u) |
        (static_cast<VoxelChunkId>(chunkX) & CoordinateMask);
}

VoxelAdapterPartitionId VoxelSceneWorkloadBuilder::ChooseChunkOwner(
    const VoxelPartitionStrategy strategy,
    const VoxelLoadBalanceScenario scenario,
    const VoxelChunkId,
    const uint32_t chunkLinearIndex,
    const uint32_t chunkX,
    const uint32_t,
    const uint32_t chunkZ,
    const uint32_t chunkGridX,
    const uint32_t,
    const uint32_t chunkGridZ,
    const float secondaryShare)
{
    const float share = std::clamp(secondaryShare, 0.0f, 1.0f);
    if (share <= 0.0f)
        return VoxelAdapterPartitionId::PrimaryPartition;
    if (share >= 1.0f)
        return VoxelAdapterPartitionId::SecondaryPartition;

    if (scenario == VoxelLoadBalanceScenario::PrimaryHeavy ||
        scenario == VoxelLoadBalanceScenario::SecondaryHeavy)
    {
        const float zPlane =
            chunkGridZ > 1
                ? (static_cast<float>(chunkZ) + 0.5f) / static_cast<float>(chunkGridZ)
                : 0.0f;
        return OwnerFromNormalizedPlane(
            zPlane,
            share,
            scenario == VoxelLoadBalanceScenario::SecondaryHeavy);
    }

    if (strategy == VoxelPartitionStrategy::SpatialPlane)
    {
        const float xPlane =
            chunkGridX > 1
                ? (static_cast<float>(chunkX) + 0.5f) / static_cast<float>(chunkGridX)
                : 0.0f;
        return OwnerFromNormalizedPlane(xPlane, share, false);
    }

    const uint32_t secondaryThreshold = static_cast<uint32_t>(share * 10000.0f);
    return StableChunkSample(chunkLinearIndex) < secondaryThreshold
               ? VoxelAdapterPartitionId::SecondaryPartition
               : VoxelAdapterPartitionId::PrimaryPartition;
}

void VoxelSceneWorkloadBuilder::InitializeAdapterPartitions(VoxelSceneWorkload& workload)
{
    workload.Partitions.clear();
}

VoxelSceneLayer VoxelSceneWorkloadBuilder::BuildDynamicWaterfallLayer(const VoxelSceneWorkload& workload)
{
    const uint32_t dynamicVoxelCount =
        workload.DynamicVoxelBudget > 0 ? workload.DynamicVoxelBudget : workload.TotalVoxelCount;
    VoxelSceneLayer layer{};
    layer.LayerId = 1;
    layer.LayerType = VoxelSceneLayerType::Dynamic;
    layer.DisplayName = "DynamicWaterfallLayer";
    layer.Position = workload.Position;
    layer.Rotation = workload.Rotation;
    layer.WorldTransform = DirectX::SimpleMath::Matrix::CreateFromYawPitchRoll(
        workload.Rotation.y,
        workload.Rotation.x,
        workload.Rotation.z) * DirectX::SimpleMath::Matrix::CreateTranslation(workload.Position);
    layer.SimulationParameters = workload.Parameters;
    layer.SimulationPolicy = DynamicWaterfallSimulationPolicy(workload);
    layer.RenderSettings.VoxelSize = workload.Parameters.VoxelSize;
    layer.SpatialLodPolicy.Settings = workload.SpatialLod;
    layer.SpatialLodPolicy.ChunkSize = workload.ChunkSize;
    layer.BoundsMin = DirectX::SimpleMath::Vector3(
        -workload.Parameters.WaterfallWidth * 0.5f,
        workload.Parameters.FloorHeight,
        -workload.Parameters.WaterfallDepth * 0.5f);
    layer.BoundsMax = DirectX::SimpleMath::Vector3(
        workload.Parameters.WaterfallWidth * 0.5f,
        workload.Parameters.SpawnHeight,
        workload.Parameters.WaterfallDepth * 0.5f);

    const auto chunkWidth = std::max<uint32_t>(1, workload.ChunkSize.Width);
    const auto chunkHeight = std::max<uint32_t>(1, workload.ChunkSize.Height);
    const auto chunkDepth = std::max<uint32_t>(1, workload.ChunkSize.Depth);

    for (DWORD sequenceId = 0; sequenceId < dynamicVoxelCount; ++sequenceId)
    {
        const auto cell = VoxelParticleSpawner::ComputeSpawnGridCell(sequenceId, workload.Parameters);
        const auto gridCoordinate = VoxelGridCoordinate{
            static_cast<int32_t>(cell.X),
            static_cast<int32_t>(cell.Y),
            static_cast<int32_t>(cell.Z)
        };
        const uint32_t chunkX = cell.X / chunkWidth;
        const uint32_t chunkY = cell.Y / chunkHeight;
        const uint32_t chunkZ = cell.Z / chunkDepth;
        const uint32_t chunkGridX = DivideRoundUp(cell.Width, chunkWidth);
        const uint32_t chunkGridY = DivideRoundUp(cell.Height, chunkHeight);
        const uint32_t chunkGridZ = DivideRoundUp(cell.Depth, chunkDepth);
        const uint32_t chunkLinearIndex = chunkX + chunkGridX * (chunkY + chunkGridY * chunkZ);
        const auto globalId = EncodeSequenceGlobalVoxelId(layer.LayerId, sequenceId);
        const auto chunkId = EncodeChunkId(layer.LayerId, chunkX, chunkY, chunkZ);
        const auto owner = ChooseChunkOwner(
            workload.PartitionStrategy,
            workload.LoadBalanceScenario,
            chunkId,
            chunkLinearIndex,
            chunkX,
            chunkY,
            chunkZ,
            chunkGridX,
            chunkGridY,
            chunkGridZ,
            workload.SecondaryShare);

        layer.GlobalVoxelIds.push_back(globalId);
        layer.SimulationVoxelIds.push_back(sequenceId);
        layer.ChunkIds.push_back(chunkId);
        layer.GridCoordinates.push_back(gridCoordinate);
        layer.MaterialIds.push_back(0u);

        auto& stream = layer.AdapterPartitions[static_cast<size_t>(owner)];
        stream.LayerId = layer.LayerId;
        stream.LayerType = layer.LayerType;
        stream.SimulationParameters = layer.SimulationParameters;
        stream.RenderSettings = layer.RenderSettings;
        stream.SimulationPolicy = layer.SimulationPolicy;
        stream.SpatialLodPolicy = layer.SpatialLodPolicy;
        stream.GlobalVoxelIds.push_back(globalId);
        stream.SimulationVoxelIds.push_back(sequenceId);
        stream.ChunkIds.push_back(chunkId);
        stream.GridCoordinates.push_back(gridCoordinate);
        stream.MaterialIds.push_back(0u);
    }

    return layer;
}

void VoxelSceneWorkloadBuilder::AppendLayerToAdapterPartitions(
    VoxelSceneWorkload& workload,
    const VoxelSceneLayer& layer)
{
    for (size_t partitionIndex = 0; partitionIndex < VoxelAdapterPartitionCount; ++partitionIndex)
    {
        const auto& stream = layer.AdapterPartitions[partitionIndex];
        if (stream.VoxelCount() == 0)
            continue;

        const auto partitionId = static_cast<VoxelAdapterPartitionId>(partitionIndex);
        auto& partition = workload.Partitions.emplace_back();
        partition.DisplayName =
            partitionId == VoxelAdapterPartitionId::SecondaryPartition
                ? (stream.LayerType == VoxelSceneLayerType::Static
                       ? "SecondaryStaticVoxelPartition"
                       : "SecondaryDynamicVoxelPartition")
                : (stream.LayerType == VoxelSceneLayerType::Static
                       ? "PrimaryStaticVoxelPartition"
                       : "PrimaryDynamicVoxelPartition");
        partition.ObjectName = partition.DisplayName;
        partition.PartitionId = partitionId;
        partition.AdapterOwner =
            partitionId == VoxelAdapterPartitionId::SecondaryPartition
                ? VoxelAdapterOwner::Secondary
                : VoxelAdapterOwner::Primary;
        partition.LayerId = stream.LayerId;
        partition.LayerType = stream.LayerType;
        partition.SimulationPolicy = stream.SimulationPolicy;
        partition.UpdateInterval =
            partition.PartitionId == VoxelAdapterPartitionId::SecondaryPartition
                ? std::max<uint32_t>(1, stream.SimulationPolicy.UpdateInterval)
                : 1;
        partition.EffectiveUpdateInterval = partition.UpdateInterval;
        partition.CoarseDeltaTime =
            static_cast<float>(static_cast<double>(partition.EffectiveUpdateInterval) / 60.0);
        partition.GlobalVoxelIds.insert(
            partition.GlobalVoxelIds.end(),
            stream.GlobalVoxelIds.begin(),
            stream.GlobalVoxelIds.end());
        partition.SimulationVoxelIds.insert(
            partition.SimulationVoxelIds.end(),
            stream.SimulationVoxelIds.begin(),
            stream.SimulationVoxelIds.end());
        partition.ChunkIds.insert(
            partition.ChunkIds.end(),
            stream.ChunkIds.begin(),
            stream.ChunkIds.end());
        partition.GridCoordinates.insert(
            partition.GridCoordinates.end(),
            stream.GridCoordinates.begin(),
            stream.GridCoordinates.end());
        partition.MaterialIds.insert(
            partition.MaterialIds.end(),
            stream.MaterialIds.begin(),
            stream.MaterialIds.end());
        partition.DrawStreams.push_back(stream);
        partition.Parameters = stream.SimulationParameters;
        partition.SpatialLod = stream.SpatialLodPolicy.Settings;
    }
}

VoxelSceneWorkload VoxelSceneWorkloadBuilder::Build(const VoxelSceneWorkload& source)
{
    const auto authoredLayers = source.Layers;
    VoxelSceneWorkload workload = source;
    workload.SecondaryShare = std::clamp(workload.SecondaryShare, 0.0f, 1.0f);
    workload.TemporalDecimationInterval =
        workload.TemporalPolicy == VoxelTemporalPolicy::Decimated
            ? std::max<uint32_t>(2, workload.TemporalDecimationInterval)
            : 1;
    workload.ChunkSize.Width = std::max<uint32_t>(1, workload.ChunkSize.Width);
    workload.ChunkSize.Height = std::max<uint32_t>(1, workload.ChunkSize.Height);
    workload.ChunkSize.Depth = std::max<uint32_t>(1, workload.ChunkSize.Depth);
    workload.Layers.clear();
    InitializeAdapterPartitions(workload);

    for (auto layer : authoredLayers)
    {
        AppendLayerToAdapterPartitions(workload, layer);
        workload.Layers.push_back(std::move(layer));
    }

    const bool authoredDynamicLayer = std::any_of(
        authoredLayers.begin(),
        authoredLayers.end(),
        [](const VoxelSceneLayer& layer)
        {
            return layer.LayerType == VoxelSceneLayerType::Dynamic;
        });
    if (!authoredDynamicLayer && (workload.DynamicVoxelBudget > 0 || (authoredLayers.empty() && workload.TotalVoxelCount > 0)))
    {
        auto dynamicLayer = BuildDynamicWaterfallLayer(workload);
        AppendLayerToAdapterPartitions(workload, dynamicLayer);
        workload.Layers.push_back(std::move(dynamicLayer));
    }

    size_t logicalCount = 0;
    size_t staticCount = 0;
    size_t dynamicCount = 0;
    for (const auto& layer : workload.Layers)
    {
        logicalCount += layer.GlobalVoxelIds.size();
        if (layer.LayerType == VoxelSceneLayerType::Static)
            staticCount += layer.GlobalVoxelIds.size();
        else
            dynamicCount += layer.GlobalVoxelIds.size();
    }
    workload.TotalVoxelCount = static_cast<uint32_t>(logicalCount);
    workload.ActualStaticVoxelCount = static_cast<uint32_t>(staticCount);
    workload.ActualDynamicVoxelCount = static_cast<uint32_t>(dynamicCount);

    if (!Validate(workload))
        throw std::runtime_error("Invalid voxel scene workload partitioning");

    return workload;
}

bool VoxelSceneWorkloadBuilder::Validate(const VoxelSceneWorkload& workload)
{
    size_t totalLayerVoxels = 0;
    for (const auto& layer : workload.Layers)
    {
        totalLayerVoxels += layer.GlobalVoxelIds.size();
        size_t layerPartitionVoxels = 0;
        for (const auto& stream : layer.AdapterPartitions)
            layerPartitionVoxels += stream.GlobalVoxelIds.size();

        if (layerPartitionVoxels != layer.GlobalVoxelIds.size())
        {
            assert(false && "Layer adapter partitions do not cover the layer exactly once");
            return false;
        }

        if (layer.LayerType == VoxelSceneLayerType::Static && layer.SimulationPolicy.Enabled)
        {
            assert(false && "Static voxel layer must not enable simulation");
            return false;
        }
    }

    if (totalLayerVoxels != workload.TotalVoxelCount)
    {
        assert(false && "Layer voxel counts do not match total scene voxel count");
        return false;
    }

    size_t totalPartitionVoxels = 0;
    std::unordered_set<VoxelGlobalId> globalIds;
    globalIds.reserve(workload.TotalVoxelCount);
    std::unordered_set<VoxelGlobalId> drawListIds;
    drawListIds.reserve(workload.TotalVoxelCount);

    for (const auto& partition : workload.Partitions)
    {
        totalPartitionVoxels += partition.GlobalVoxelIds.size();
        if (partition.SimulationPolicy.Enabled && !partition.HasDynamicVoxels() && partition.VoxelCount() > 0)
        {
            assert(false && "Static-only adapter partition must not request simulation dispatch");
            return false;
        }

        for (const auto globalId : partition.GlobalVoxelIds)
        {
            if (!globalIds.insert(globalId).second)
            {
                assert(false && "Duplicate GlobalVoxelId found across adapter partitions");
                return false;
            }
        }

        for (const auto& stream : partition.DrawStreams)
        {
            if (stream.GlobalVoxelIds.size() != stream.SimulationVoxelIds.size() ||
                stream.GlobalVoxelIds.size() != stream.ChunkIds.size() ||
                stream.GlobalVoxelIds.size() != stream.GridCoordinates.size() ||
                stream.GlobalVoxelIds.size() != stream.MaterialIds.size())
            {
                assert(false && "Draw stream id arrays must have matching lengths");
                return false;
            }

            for (const auto globalId : stream.GlobalVoxelIds)
            {
                if (!drawListIds.insert(globalId).second)
                {
                    assert(false && "GlobalVoxelId appears in more than one adapter draw list");
                    return false;
                }
            }
        }
    }

    if (totalPartitionVoxels != workload.TotalVoxelCount ||
        globalIds.size() != workload.TotalVoxelCount ||
        drawListIds.size() != workload.TotalVoxelCount)
    {
        assert(false && "Adapter partition counts do not match total scene voxel count");
        return false;
    }

    return true;
}
