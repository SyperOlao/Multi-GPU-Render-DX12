#pragma once

#include "GDescriptor.h"
#include "GraphicPSO.h"
#include "MemoryAllocator.h"
#include "Source/Voxels/VoxelTypes.h"

#include <memory>
#include <string>

class AssetsLoader;
class GameObject;
class GModel;
class Renderer;

namespace PEPEngine::Graphics
{
    class GDevice;
}

enum class VoxelResearchScenePreset
{
    EmptyBaseline,
    StaticVoxelEnvironment,
    DynamicWaterfall,
    MixedVoxelEnvironment,
    OcclusionValidation,
    SpatialLodDemonstration,
    DemoMixed
};

struct VoxelResearchSceneContext
{
    std::shared_ptr<PEPEngine::Graphics::GDevice> PrimaryDevice;
    std::shared_ptr<PEPEngine::Graphics::GDevice> SecondaryDevice;
    AssetsLoader& Assets;
    PEPEngine::Allocator::custom_unordered_map<std::wstring, std::shared_ptr<GModel>>& Models;
    PEPEngine::Graphics::GDescriptor& SrvTexturesMemory;
    PEPEngine::Allocator::custom_vector<std::shared_ptr<GameObject>>& GameObjects;
    PEPEngine::Allocator::custom_vector<PEPEngine::Allocator::custom_vector<std::shared_ptr<Renderer>>>& TypedRenderers;
    VoxelSceneWorkload& Workload;
    float AspectRatio = 1.0f;
};

class VoxelResearchSceneManager
{
public:
    VoxelResearchScenePreset GetActivePreset() const;
    bool RequestPreset(VoxelResearchScenePreset preset);
    void ForceRebuildActivePreset();
    bool RequiresRebuild() const;
    void RebuildScene(const VoxelResearchSceneContext& context);

private:
    static void ClearScene(const VoxelResearchSceneContext& context);
    static void AddRenderer(const VoxelResearchSceneContext& context, PEPEngine::Graphics::RenderMode mode,
                            const std::shared_ptr<Renderer>& renderer);
    static VoxelSceneWorkload CreateLogicalWorkload(VoxelResearchScenePreset preset);
    static void CreateOcclusionPrimitives(const VoxelResearchSceneContext& context);
    static void CreateEmptyBaseline(const VoxelResearchSceneContext& context);
    static const char* PresetName(VoxelResearchScenePreset preset);

    VoxelResearchScenePreset activePreset = VoxelResearchScenePreset::EmptyBaseline;
    VoxelResearchScenePreset requestedPreset = VoxelResearchScenePreset::EmptyBaseline;
    bool rebuildPending = true;
};
