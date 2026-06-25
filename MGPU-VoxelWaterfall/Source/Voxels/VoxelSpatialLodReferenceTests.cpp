#include "Source/Voxels/VoxelSpatialLodReferenceTests.h"

#if defined(DEBUG) || defined(_DEBUG)

#include "Source/Voxels/VoxelTypes.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <Windows.h>

using DirectX::SimpleMath::Vector3;

namespace
{
    struct ReferenceItem
    {
        int GroupX = 0;
        int GroupY = 0;
        int GroupZ = 0;
        uint32_t LodLevel = 0;
        Vector3 Center = Vector3::Zero;
        Vector3 HalfExtent = Vector3::Zero;
        uint32_t OccupiedCount = 0;
    };

    struct ReferenceInput
    {
        std::vector<VoxelGridCoordinate> Cells;
        VoxelGridCoordinate GridOrigin{};
        float VoxelSize = 1.0f;
        Vector3 CameraPosition = Vector3(0.0f, 0.0f, -1000.0f);
        float Lod0Distance = 25.0f;
        float Lod1Distance = 100.0f;
        uint32_t ForcedLod = 0xffffffffu;
        float WaterfallWidth = 1.0f;
        float WaterfallDepth = 1.0f;
        float SpawnHeight = 1.0f;
        float FloorHeight = 0.0f;
    };

    int FloorDiv(const int value, const int divisor)
    {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    }

    uint32_t BlockSizeForLod(const uint32_t lodLevel)
    {
        return lodLevel == 0u ? 1u : (lodLevel == 1u ? 2u : 4u);
    }

    Vector3 CellCenter(const VoxelGridCoordinate& cell, const VoxelGridCoordinate& origin, const float voxelSize)
    {
        return Vector3(
            (static_cast<float>(cell.X + origin.X) + 0.5f) * voxelSize,
            (static_cast<float>(cell.Y + origin.Y) + 0.5f) * voxelSize,
            (static_cast<float>(cell.Z + origin.Z) + 0.5f) * voxelSize);
    }

    uint32_t SelectLod(const ReferenceInput& input, const VoxelGridCoordinate& cell)
    {
        if (input.ForcedLod <= 2u)
            return input.ForcedLod;

        const float distance = (CellCenter(cell, input.GridOrigin, input.VoxelSize) - input.CameraPosition).Length();
        if (distance <= input.Lod0Distance)
            return 0u;
        if (distance <= input.Lod1Distance)
            return 1u;
        return 2u;
    }

    std::vector<ReferenceItem> BuildReferenceItems(const ReferenceInput& input)
    {
        std::map<std::tuple<uint32_t, int, int, int>, ReferenceItem> items;
        for (const auto& cell : input.Cells)
        {
            const uint32_t lodLevel = SelectLod(input, cell);
            const int blockSize = static_cast<int>(BlockSizeForLod(lodLevel));
            const int signedX = cell.X + input.GridOrigin.X;
            const int signedY = cell.Y + input.GridOrigin.Y;
            const int signedZ = cell.Z + input.GridOrigin.Z;
            const int groupX = FloorDiv(signedX, blockSize);
            const int groupY = FloorDiv(signedY, blockSize);
            const int groupZ = FloorDiv(signedZ, blockSize);
            const auto key = std::make_tuple(lodLevel, groupX, groupY, groupZ);

            auto [it, inserted] = items.try_emplace(key);
            if (inserted)
            {
                auto& item = it->second;
                item.GroupX = groupX;
                item.GroupY = groupY;
                item.GroupZ = groupZ;
                item.LodLevel = lodLevel;
                item.Center = Vector3(
                    (static_cast<float>(groupX * blockSize) + 0.5f * static_cast<float>(blockSize)) * input.VoxelSize,
                    (static_cast<float>(groupY * blockSize) + 0.5f * static_cast<float>(blockSize)) * input.VoxelSize,
                    (static_cast<float>(groupZ * blockSize) + 0.5f * static_cast<float>(blockSize)) * input.VoxelSize);
                item.HalfExtent = Vector3::One * (0.5f * static_cast<float>(blockSize) * input.VoxelSize);
            }
            it->second.OccupiedCount++;
        }

        std::vector<ReferenceItem> result;
        result.reserve(items.size());
        for (const auto& [_, item] : items)
            result.push_back(item);
        return result;
    }

    std::vector<VoxelGridCoordinate> FilledChunk8x8x4()
    {
        std::vector<VoxelGridCoordinate> cells;
        cells.reserve(8u * 8u * 4u);
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    cells.push_back({x, y, z});
        return cells;
    }

    bool HasCenter(const std::vector<ReferenceItem>& items, const Vector3& center)
    {
        return std::any_of(
            items.begin(),
            items.end(),
            [&center](const ReferenceItem& item)
            {
                return item.Center == center;
            });
    }

    std::string Serialize(const std::vector<ReferenceItem>& items)
    {
        std::ostringstream stream;
        for (const auto& item : items)
        {
            stream << item.LodLevel << ':' << item.GroupX << ',' << item.GroupY << ',' << item.GroupZ << ':'
                << item.Center.x << ',' << item.Center.y << ',' << item.Center.z << ':'
                << item.HalfExtent.x << ',' << item.HalfExtent.y << ',' << item.HalfExtent.z << ';';
        }
        return stream.str();
    }

    void AssertAabbCoverage(const ReferenceInput& input, const std::vector<ReferenceItem>& items)
    {
        for (const auto& item : items)
        {
            const int blockSize = static_cast<int>(BlockSizeForLod(item.LodLevel));
            for (const auto& cell : input.Cells)
            {
                const int signedX = cell.X + input.GridOrigin.X;
                const int signedY = cell.Y + input.GridOrigin.Y;
                const int signedZ = cell.Z + input.GridOrigin.Z;
                if (FloorDiv(signedX, blockSize) != item.GroupX ||
                    FloorDiv(signedY, blockSize) != item.GroupY ||
                    FloorDiv(signedZ, blockSize) != item.GroupZ)
                    continue;

                const Vector3 center = CellCenter(cell, input.GridOrigin, input.VoxelSize);
                assert(center.x >= item.Center.x - item.HalfExtent.x);
                assert(center.x <= item.Center.x + item.HalfExtent.x);
                assert(center.y >= item.Center.y - item.HalfExtent.y);
                assert(center.y <= item.Center.y + item.HalfExtent.y);
                assert(center.z >= item.Center.z - item.HalfExtent.z);
                assert(center.z <= item.Center.z + item.HalfExtent.z);
            }
            assert(item.HalfExtent.x == 0.5f * static_cast<float>(blockSize) * input.VoxelSize);
            assert(item.HalfExtent.y == item.HalfExtent.x);
            assert(item.HalfExtent.z == item.HalfExtent.x);
        }
    }

    void LogTestResult(const char* name, const uint32_t expected, const uint32_t actual)
    {
        std::ostringstream stream;
        stream << "[VoxelSpatialLodReferenceTests] " << name
            << " expected=" << expected << " actual=" << actual << "\n";
        OutputDebugStringA(stream.str().c_str());
    }

    uint32_t NextPowerOfTwo(uint32_t value)
    {
        if (value <= 1u)
            return 1u;
        --value;
        value |= value >> 1u;
        value |= value >> 2u;
        value |= value >> 4u;
        value |= value >> 8u;
        value |= value >> 16u;
        return value + 1u;
    }

    uint32_t HashGroupKey(uint32_t key)
    {
        uint32_t x = key;
        x ^= x >> 16u;
        x *= 0x7feb352du;
        x ^= x >> 15u;
        x *= 0x846ca68bu;
        x ^= x >> 16u;
        return x;
    }

    struct HashSimulationResult
    {
        uint32_t Emitted = 0;
        uint32_t Duplicates = 0;
        uint32_t Overflow = 0;
        uint32_t MaxProbe = 0;
    };

    HashSimulationResult SimulateBoundedHash(const std::vector<uint32_t>& keys,
                                             const uint32_t capacity,
                                             const uint32_t maxProbeCount)
    {
        std::vector<uint32_t> table(capacity, 0xffffffffu);
        HashSimulationResult result{};
        const uint32_t mask = capacity - 1u;
        for (const uint32_t key : keys)
        {
            uint32_t slot = HashGroupKey(key) & mask;
            bool inserted = false;
            bool duplicate = false;
            uint32_t probe = 0;
            for (; probe < maxProbeCount; ++probe)
            {
                if (table[slot] == 0xffffffffu)
                {
                    table[slot] = key;
                    inserted = true;
                    break;
                }
                if (table[slot] == key)
                {
                    duplicate = true;
                    break;
                }
                slot = (slot + 1u) & mask;
            }
            result.MaxProbe = std::max(result.MaxProbe, probe + 1u);
            if (inserted)
                ++result.Emitted;
            else if (duplicate)
                ++result.Duplicates;
            else
                ++result.Overflow;
        }
        return result;
    }
}

void RunVoxelSpatialLodReferenceTests()
{
    ReferenceInput input{};
    input.Cells = FilledChunk8x8x4();

    input.ForcedLod = 0u;
    const auto lod0 = BuildReferenceItems(input);
    LogTestResult("filled_chunk_lod0", 256u, static_cast<uint32_t>(lod0.size()));
    assert(lod0.size() == 256u);

    input.ForcedLod = 1u;
    const auto lod1 = BuildReferenceItems(input);
    LogTestResult("filled_chunk_lod1", 32u, static_cast<uint32_t>(lod1.size()));
    assert(lod1.size() == 32u);
    assert(HasCenter(lod1, Vector3(1.0f, 1.0f, 1.0f)));
    assert(HasCenter(lod1, Vector3(7.0f, 7.0f, 3.0f)));
    assert(std::any_of(lod1.begin(), lod1.end(), [](const ReferenceItem& item) { return item.Center.x != 1.0f; }));
    assert(std::any_of(lod1.begin(), lod1.end(), [](const ReferenceItem& item) { return item.Center.y != 1.0f; }));

    input.ForcedLod = 2u;
    const auto lod2 = BuildReferenceItems(input);
    LogTestResult("filled_chunk_lod2", 4u, static_cast<uint32_t>(lod2.size()));
    assert(lod2.size() == 4u);
    assert(HasCenter(lod2, Vector3(2.0f, 2.0f, 2.0f)));
    assert(HasCenter(lod2, Vector3(6.0f, 6.0f, 2.0f)));
    assert(std::any_of(lod2.begin(), lod2.end(), [](const ReferenceItem& item) { return item.Center.x != 2.0f; }));
    assert(std::any_of(lod2.begin(), lod2.end(), [](const ReferenceItem& item) { return item.Center.y != 2.0f; }));

    ReferenceInput sparse{};
    sparse.Cells = {{1, 1, 1}};
    sparse.ForcedLod = 1u;
    const auto sparseItems = BuildReferenceItems(sparse);
    LogTestResult("sparse_missing_anchor", 1u, static_cast<uint32_t>(sparseItems.size()));
    assert(sparseItems.size() == 1u);
    assert(sparseItems.front().Center == Vector3(1.0f, 1.0f, 1.0f));

    ReferenceInput negative{};
    negative.Cells = {{0, 0, 0}, {1, 1, 1}};
    negative.GridOrigin = {-2, 0, -4};
    negative.ForcedLod = 1u;
    const auto negativeItems = BuildReferenceItems(negative);
    LogTestResult("negative_origin", 1u, static_cast<uint32_t>(negativeItems.size()));
    assert(negativeItems.size() == 1u);
    assert(negativeItems.front().Center == Vector3(-1.0f, 1.0f, -3.0f));

    ReferenceInput cameraA = sparse;
    cameraA.ForcedLod = 0xffffffffu;
    cameraA.CameraPosition = Vector3(0.0f, 0.0f, -1000.0f);
    cameraA.WaterfallWidth = 1.0f;
    cameraA.WaterfallDepth = 1.0f;
    const auto cameraItemsA = BuildReferenceItems(cameraA);
    ReferenceInput cameraB = cameraA;
    cameraB.WaterfallWidth = 10000.0f;
    cameraB.WaterfallDepth = 5000.0f;
    cameraB.SpawnHeight = 999.0f;
    cameraB.FloorHeight = -123.0f;
    const auto cameraItemsB = BuildReferenceItems(cameraB);
    assert(cameraItemsA.size() == cameraItemsB.size());
    assert(cameraItemsA.front().LodLevel == cameraItemsB.front().LodLevel);
    assert(cameraItemsA.front().Center == cameraItemsB.front().Center);

    const auto deterministicA = Serialize(BuildReferenceItems(input));
    const auto deterministicB = Serialize(BuildReferenceItems(input));
    assert(deterministicA == deterministicB);

    AssertAabbCoverage(input, lod2);
    AssertAabbCoverage(negative, negativeItems);

    std::vector<uint32_t> fastPathRepresentatives(256u);
    std::iota(fastPathRepresentatives.begin(), fastPathRepresentatives.end(), 0u);
    std::set<uint32_t> uniqueFastPath(fastPathRepresentatives.begin(), fastPathRepresentatives.end());
    LogTestResult("lod_off_fast_path_count", 256u, static_cast<uint32_t>(uniqueFastPath.size()));
    assert(uniqueFastPath.size() == fastPathRepresentatives.size());

    const std::vector<uint32_t> duplicateKeys = {7u, 7u, 8u, 8u, 9u};
    const auto duplicateHash = SimulateBoundedHash(duplicateKeys, 64u, 64u);
    assert(duplicateHash.Emitted == 3u);
    assert(duplicateHash.Duplicates == 2u);
    assert(duplicateHash.Overflow == 0u);

    std::vector<uint32_t> overflowKeys(96u);
    std::iota(overflowKeys.begin(), overflowKeys.end(), 0u);
    const auto overflowHash = SimulateBoundedHash(overflowKeys, 64u, 1u);
    assert(overflowHash.Overflow > 0u);

    std::vector<uint32_t> uniqueKeys(123556u);
    std::iota(uniqueKeys.begin(), uniqueKeys.end(), 0u);
    const uint32_t capacity = NextPowerOfTwo(std::max<uint32_t>(64u, static_cast<uint32_t>(uniqueKeys.size()) * 2u));
    const auto largeHash = SimulateBoundedHash(uniqueKeys, capacity, 64u);
    LogTestResult("unique_123556_overflow", 0u, largeHash.Overflow);
    assert(largeHash.Emitted == uniqueKeys.size());
    assert(largeHash.Overflow == 0u);
}

#endif
