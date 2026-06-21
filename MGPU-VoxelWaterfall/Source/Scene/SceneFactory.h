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

struct SceneFactoryContext
{
    std::shared_ptr<PEPEngine::Graphics::GDevice> PrimaryDevice;
    std::shared_ptr<PEPEngine::Graphics::GDevice> SecondaryDevice;
    AssetsLoader& Assets;
    PEPEngine::Allocator::custom_unordered_map<std::wstring, std::shared_ptr<GModel>>& Models;
    PEPEngine::Graphics::GDescriptor& SrvTexturesMemory;
    PEPEngine::Allocator::custom_vector<std::shared_ptr<GameObject>>& GameObjects;
    PEPEngine::Allocator::custom_vector<PEPEngine::Allocator::custom_vector<std::shared_ptr<Renderer>>>& TypedRenderers;
    VoxelWaterfallWorkload& Workload;
    VoxelExecutionMode ExecutionMode = VoxelExecutionMode::SingleGpuFull;
    bool MultiGpuAvailable = false;
    float AspectRatio = 1.0f;
};

class SceneFactory
{
public:
    void CreateScene(const SceneFactoryContext& context) const;

private:
    static void AddRenderer(const SceneFactoryContext& context, PEPEngine::Graphics::RenderMode mode,
                            const std::shared_ptr<Renderer>& renderer);
    static void CreateVoxelWaterfall(const SceneFactoryContext& context);
};
