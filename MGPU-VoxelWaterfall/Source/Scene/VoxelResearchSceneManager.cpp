#include "Source/Scene/VoxelResearchSceneManager.h"

#include "Camera.h"
#include "GameObject.h"
#include "Light.h"
#include "ModelRenderer.h"
#include "Source/Voxels/VoxelSceneWorkload.h"
#include "Source/Voxels/VoxelResearchEnvironmentGenerator.h"
#include "Source/Scene/VoxelResearchCameraController.h"
#include "Transform.h"

#include <algorithm>
#include <array>
#include <cassert>

using namespace DirectX::SimpleMath;
using namespace PEPEngine::Graphics;

namespace
{
    constexpr std::array<const char*, 3> OcclusionPrimitiveNames = {
        "OcclusionMatteSlab",
        "OcclusionRearColumn",
        "OcclusionVoxelFrameSection"
    };

    uint32_t DynamicBudgetForPreset(const DynamicVoxelBudgetPreset preset)
    {
        switch (preset)
        {
        case DynamicVoxelBudgetPreset::Small:
            return 25000;
        case DynamicVoxelBudgetPreset::Medium:
            return 100000;
        case DynamicVoxelBudgetPreset::Large:
            return 250000;
        case DynamicVoxelBudgetPreset::VeryLarge:
            return 500000;
        default:
            return 25000;
        }
    }

    void ConfigureWaterfallParameters(VoxelSceneWorkload& workload, const DynamicVoxelBudgetPreset preset)
    {
        workload.DynamicBudgetPreset = preset;
        workload.DynamicVoxelBudget = DynamicBudgetForPreset(preset);
        workload.Parameters.VoxelSize = 0.35f;
        workload.Parameters.SpawnHeight = 41.0f;
        workload.Parameters.FloorHeight = 2.0f;
        workload.Parameters.WaterfallWidth = 12.0f;
        workload.Parameters.WaterfallDepth = 4.0f;
        workload.Parameters.InitialFallSpeed = 5.5f;
        workload.Parameters.Gravity = 16.0f;
        workload.Parameters.Seed = 4242;
        workload.TemporalDecimationInterval = 1;
    }

    VoxelSceneLayer GenerateStaticLayer(VoxelSceneWorkload& workload)
    {
        VoxelResearchEnvironmentGenerator::Settings settings{};
        settings.Seed = workload.StaticGenerationSeed;
        settings.StaticVoxelBudget = workload.StaticVoxelBudget;
        settings.BudgetPreset = workload.StaticBudgetPreset;
        settings.StorageMode = workload.StaticStorageMode;
        settings.VoxelSize = workload.StaticVoxelSize;
        settings.SecondaryShare = workload.SecondaryShare;
        settings.PartitionStrategy = workload.PartitionStrategy;
        settings.LoadBalanceScenario = workload.LoadBalanceScenario;
        settings.ChunkSize = workload.ChunkSize;
        settings.SpatialLod = workload.SpatialLod;
        auto generated = VoxelResearchEnvironmentGenerator::Generate(settings);
        workload.StaticTelemetry = generated.Telemetry;
        return std::move(generated.Layer);
    }

    bool HasGameObjectNamed(
        const PEPEngine::Allocator::custom_vector<std::shared_ptr<GameObject>>& gameObjects,
        const char* name)
    {
        return std::any_of(
            gameObjects.begin(),
            gameObjects.end(),
            [name](const std::shared_ptr<GameObject>& object)
            {
                return object && object->GetName() == name;
            });
    }

    void AssertOcclusionPrimitiveOwnership(
        const VoxelResearchSceneContext& context,
        const VoxelResearchScenePreset preset)
    {
        const bool expectsOcclusionPrimitives = preset == VoxelResearchScenePreset::OcclusionValidation;
        for (const char* name : OcclusionPrimitiveNames)
        {
            const bool exists = HasGameObjectNamed(context.GameObjects, name);
            assert(expectsOcclusionPrimitives ? exists : !exists);
        }
    }

    VoxelResearchWorkloadProfile ProfileForPreset(const VoxelResearchScenePreset preset)
    {
        switch (preset)
        {
        case VoxelResearchScenePreset::StaticVoxelEnvironment:
            return VoxelResearchWorkloadProfile::StaticRenderOnly;
        case VoxelResearchScenePreset::DynamicWaterfall:
            return VoxelResearchWorkloadProfile::DynamicSimulationAndRender;
        case VoxelResearchScenePreset::MixedVoxelEnvironment:
            return VoxelResearchWorkloadProfile::MixedStaticAndDynamic;
        case VoxelResearchScenePreset::OcclusionValidation:
            return VoxelResearchWorkloadProfile::OcclusionValidation;
        case VoxelResearchScenePreset::SpatialLodDemonstration:
            return VoxelResearchWorkloadProfile::SpatialLodDemonstration;
        case VoxelResearchScenePreset::EmptyBaseline:
        default:
            return VoxelResearchWorkloadProfile::StaticRenderOnly;
        }
    }
}

VoxelResearchScenePreset VoxelResearchSceneManager::GetActivePreset() const
{
    return activePreset;
}

bool VoxelResearchSceneManager::RequestPreset(const VoxelResearchScenePreset preset)
{
    requestedPreset = preset;
    if (requestedPreset == activePreset && !rebuildPending)
        return false;

    rebuildPending = true;
    return true;
}

bool VoxelResearchSceneManager::RequiresRebuild() const
{
    return rebuildPending;
}

void VoxelResearchSceneManager::RebuildScene(const VoxelResearchSceneContext& context)
{
    if (!rebuildPending)
        return;

    ClearScene(context);
    activePreset = requestedPreset;
    context.Workload = CreateLogicalWorkload(activePreset);

    switch (activePreset)
    {
    case VoxelResearchScenePreset::EmptyBaseline:
    case VoxelResearchScenePreset::StaticVoxelEnvironment:
    case VoxelResearchScenePreset::DynamicWaterfall:
    case VoxelResearchScenePreset::MixedVoxelEnvironment:
        CreateEmptyBaseline(context);
        break;
    case VoxelResearchScenePreset::OcclusionValidation:
        CreateEmptyBaseline(context);
        CreateOcclusionPrimitives(context);
        break;
    case VoxelResearchScenePreset::SpatialLodDemonstration:
    default:
        CreateEmptyBaseline(context);
        break;
    }

    rebuildPending = false;
    AssertOcclusionPrimitiveOwnership(context, activePreset);
}

void VoxelResearchSceneManager::ClearScene(const VoxelResearchSceneContext& context)
{
    context.GameObjects.clear();
    for (auto& renderers : context.TypedRenderers)
        renderers.clear();
}

void VoxelResearchSceneManager::AddRenderer(const VoxelResearchSceneContext& context, const RenderMode mode,
                                            const std::shared_ptr<Renderer>& renderer)
{
    context.TypedRenderers[static_cast<int>(mode)].push_back(renderer);
}

void VoxelResearchSceneManager::CreateOcclusionPrimitives(const VoxelResearchSceneContext& context)
{
    const auto boxIt = context.Models.find(L"box");
    if (boxIt == context.Models.end())
        return;

    auto createBox = [&](const char* name, const Vector3& position, const Vector3& scale)
    {
        auto object = std::make_unique<GameObject>(name);
        object->GetTransform()->SetPosition(position);
        object->GetTransform()->SetScale(scale);
        auto renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, boxIt->second);
        object->AddComponent(renderer);
        AddRenderer(context, RenderMode::Opaque, renderer);
        context.GameObjects.push_back(std::move(object));
    };

    createBox("OcclusionMatteSlab", Vector3(-2.8f, 14.0f, -7.0f), Vector3(4.5f, 7.5f, 0.55f));
    createBox("OcclusionRearColumn", Vector3(4.0f, 12.0f, 3.5f), Vector3(1.6f, 12.0f, 1.6f));
    createBox("OcclusionVoxelFrameSection", Vector3(-8.5f, 7.0f, -13.0f), Vector3(2.0f, 8.0f, 7.0f));
}

VoxelSceneWorkload VoxelResearchSceneManager::CreateLogicalWorkload(const VoxelResearchScenePreset preset)
{
    VoxelSceneWorkload workload{};
    workload.Profile = ProfileForPreset(preset);
    workload.ScenePreset = PresetName(preset);
    workload.Position = Vector3::Zero;
    workload.Rotation = Vector3::Zero;
    workload.TotalVoxelCount = 0;
    workload.DynamicVoxelBudget = 0;
    workload.PartitionStrategy = VoxelPartitionStrategy::HashedChunks;
    workload.LoadBalanceScenario = VoxelLoadBalanceScenario::Balanced;
    workload.SecondaryShare = 0.5f;
    workload.TemporalPolicy = VoxelTemporalPolicy::Full;
    workload.TemporalDecimationInterval = 1;
    workload.StaticBudgetPreset = StaticVoxelBudgetPreset::Small;
    workload.StaticVoxelBudget = VoxelResearchEnvironmentGenerator::BudgetForPreset(workload.StaticBudgetPreset);
    workload.StaticStorageMode = StaticVoxelStorageMode::SurfaceOnly;
    workload.StaticGenerationSeed = 1337;
    workload.StaticVoxelSize = 0.65f;
    workload.SpatialLod.Mode = VoxelSpatialLodMode::Off;
    workload.SpatialLod.Lod0Distance = 45.0f;
    workload.SpatialLod.Lod1Distance = 120.0f;
    workload.SpatialLod.Hysteresis = 8.0f;
    workload.Parameters.VoxelSize = 0.5f;
    workload.Parameters.SpawnHeight = 1.0f;
    workload.Parameters.FloorHeight = 0.0f;
    workload.Parameters.WaterfallWidth = 1.0f;
    workload.Parameters.WaterfallDepth = 1.0f;
    workload.Parameters.InitialFallSpeed = 0.0f;
    workload.Parameters.Gravity = 0.0f;
    workload.Parameters.Seed = 1337;
    workload.CameraPath = "FixedOverview";
    workload.LightingPreset = "BenchmarkNeutral";
    workload.CameraMode = VoxelResearchCameraMode::FixedOverview;
    workload.LightingMode = VoxelResearchLightingPreset::BenchmarkNeutral;
    workload.DynamicShadowsEnabled = false;
    workload.BenchmarkConfigClass = VoxelBenchmarkConfigClass::ValidMatchingBenchmark;

    switch (preset)
    {
    case VoxelResearchScenePreset::StaticVoxelEnvironment:
    {
        workload.Profile = VoxelResearchWorkloadProfile::StaticRenderOnly;
        workload.DynamicVoxelBudget = 0;
        workload.Layers.push_back(GenerateStaticLayer(workload));
        return VoxelSceneWorkloadBuilder::Build(workload);
    }
    case VoxelResearchScenePreset::DynamicWaterfall:
    {
        workload.Profile = VoxelResearchWorkloadProfile::DynamicSimulationAndRender;
        workload.StaticVoxelBudget = 0;
        workload.CameraMode = VoxelResearchCameraMode::WaterfallCloseup;
        workload.CameraPath = "WaterfallCloseup";
        ConfigureWaterfallParameters(workload, DynamicVoxelBudgetPreset::Small);
        return VoxelSceneWorkloadBuilder::Build(workload);
    }
    case VoxelResearchScenePreset::MixedVoxelEnvironment:
    {
        workload.Profile = VoxelResearchWorkloadProfile::MixedStaticAndDynamic;
        workload.StaticBudgetPreset = StaticVoxelBudgetPreset::Small;
        workload.StaticVoxelBudget = VoxelResearchEnvironmentGenerator::BudgetForPreset(workload.StaticBudgetPreset);
        ConfigureWaterfallParameters(workload, DynamicVoxelBudgetPreset::Small);
        assert(workload.SpatialLod.Mode == VoxelSpatialLodMode::Off);
        workload.Layers.push_back(GenerateStaticLayer(workload));
        return VoxelSceneWorkloadBuilder::Build(workload);
    }
    case VoxelResearchScenePreset::OcclusionValidation:
    {
        workload.Profile = VoxelResearchWorkloadProfile::OcclusionValidation;
        workload.StaticBudgetPreset = StaticVoxelBudgetPreset::Small;
        workload.StaticVoxelBudget = 25000;
        workload.SecondaryShare = 0.5f;
        workload.PartitionStrategy = VoxelPartitionStrategy::SpatialPlane;
        workload.LoadBalanceScenario = VoxelLoadBalanceScenario::Balanced;
        workload.BenchmarkConfigClass = VoxelBenchmarkConfigClass::Diagnostic;
        workload.BenchmarkConfigReason = "OcclusionValidation is a visual composition diagnostic";
        workload.CameraMode = VoxelResearchCameraMode::FixedOcclusion;
        workload.CameraPath = "FixedOcclusion";
        workload.LightingMode = VoxelResearchLightingPreset::OcclusionValidationLighting;
        workload.LightingPreset = "OcclusionValidationLighting";
        ConfigureWaterfallParameters(workload, DynamicVoxelBudgetPreset::Small);
        workload.DynamicVoxelBudget = 12000;
        workload.Layers.push_back(GenerateStaticLayer(workload));
        return VoxelSceneWorkloadBuilder::Build(workload);
    }
    case VoxelResearchScenePreset::SpatialLodDemonstration:
    {
        workload.Profile = VoxelResearchWorkloadProfile::SpatialLodDemonstration;
        workload.StaticBudgetPreset = StaticVoxelBudgetPreset::Large;
        workload.StaticVoxelBudget = VoxelResearchEnvironmentGenerator::BudgetForPreset(workload.StaticBudgetPreset);
        workload.DynamicVoxelBudget = 0;
        workload.SpatialLod.Mode = VoxelSpatialLodMode::ThreeLevel;
        workload.SpatialLod.Lod0Distance = 35.0f;
        workload.SpatialLod.Lod1Distance = 110.0f;
        workload.SpatialLod.Hysteresis = 10.0f;
        workload.CameraMode = VoxelResearchCameraMode::LodSweepRoute;
        workload.CameraPath = "LodSweepRoute";
        workload.Layers.push_back(GenerateStaticLayer(workload));
        return VoxelSceneWorkloadBuilder::Build(workload);
    }
    case VoxelResearchScenePreset::EmptyBaseline:
    default:
        workload.ScenePreset = "EmptyBaseline";
        workload.DynamicVoxelBudget = 0;
        workload.StaticVoxelBudget = 0;
        workload.SpatialLod.Mode = VoxelSpatialLodMode::Off;
        return VoxelSceneWorkloadBuilder::Build(workload);
    }
}

void VoxelResearchSceneManager::CreateEmptyBaseline(const VoxelResearchSceneContext& context)
{
    auto voxelHost = std::make_unique<GameObject>("VoxelSceneGpuPartitions");
    context.GameObjects.push_back(std::move(voxelHost));

    auto resolveQuad = std::make_unique<GameObject>("FinalResolveQuad");
    auto renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"quad"]);
    resolveQuad->AddComponent(renderer);
    AddRenderer(context, RenderMode::Quad, renderer);
    context.GameObjects.push_back(std::move(resolveQuad));

    auto sun = std::make_unique<GameObject>("FixedDirectionalLight");
    auto light = std::make_shared<Light>(Directional);
    light->Direction({0.57735f, -0.57735f, 0.57735f});
    light->Strength({0.8f, 0.8f, 0.8f});
    sun->AddComponent(light);
    context.GameObjects.push_back(std::move(sun));

    auto camera = std::make_unique<GameObject>("MainCamera");
    auto controller = std::make_shared<VoxelResearchCameraController>();
    controller->SetMode(context.Workload.CameraMode);
    controller->SetInputBlocked(context.Workload.CameraMode != VoxelResearchCameraMode::Interactive);
    camera->AddComponent(controller);
    camera->AddComponent(std::make_shared<Camera>(context.AspectRatio));
    context.GameObjects.push_back(std::move(camera));
}

const char* VoxelResearchSceneManager::PresetName(const VoxelResearchScenePreset preset)
{
    switch (preset)
    {
    case VoxelResearchScenePreset::EmptyBaseline:
        return "EmptyBaseline";
    case VoxelResearchScenePreset::StaticVoxelEnvironment:
        return "StaticVoxelEnvironment";
    case VoxelResearchScenePreset::DynamicWaterfall:
        return "DynamicWaterfall";
    case VoxelResearchScenePreset::MixedVoxelEnvironment:
        return "MixedVoxelEnvironment";
    case VoxelResearchScenePreset::OcclusionValidation:
        return "OcclusionValidation";
    case VoxelResearchScenePreset::SpatialLodDemonstration:
        return "SpatialLodDemonstration";
    default:
        return "Unknown";
    }
}
