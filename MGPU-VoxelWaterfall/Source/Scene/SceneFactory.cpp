#include "Source/Scene/SceneFactory.h"

#include "AssetsLoader.h"
#include "Camera.h"
#include "CameraController.h"
#include "Source/Voxels/CrossAdapterVoxelEmitter.h"
#include "GameObject.h"
#include "GModel.h"
#include "Light.h"
#include "ModelRenderer.h"
#include "Rotater.h"
#include "SkyBox.h"
#include "Transform.h"
#include "Source/Voxels/VoxelWaterfallEmitter.h"

#include <algorithm>

using namespace DirectX::SimpleMath;
using namespace PEPEngine::Graphics;

void SceneFactory::AddRenderer(const SceneFactoryContext& context, const RenderMode mode,
                               const std::shared_ptr<Renderer>& renderer)
{
    context.TypedRenderers[static_cast<int>(mode)].push_back(renderer);
}

void SceneFactory::CreateVoxelLod(const SceneFactoryContext& context, const char* displayName,
                                  const char* objectName, const size_t lodIndex,
                                  const Vector3& position, const int count,
                                  const VoxelSimulationParameters& parameters)
{
    auto voxelObject = std::make_unique<GameObject>(objectName);
    voxelObject->GetTransform()->SetPosition(position);

    std::shared_ptr<VoxelWaterfallEmitter> emitter;
    std::shared_ptr<CrossAdapterVoxelEmitter> crossEmitter;
    if (lodIndex != NearVoxelWaterfall && context.SplitMultiGpuAvailable)
    {
        crossEmitter = std::make_shared<CrossAdapterVoxelEmitter>(
            context.PrimaryDevice, context.SecondaryDevice, static_cast<DWORD>(std::max(1, count)), parameters);
        crossEmitter->SetEnabled(true);
        voxelObject->AddComponent(crossEmitter);
        AddRenderer(context, RenderMode::Particle, crossEmitter);
    }
    else
    {
        emitter = std::make_shared<VoxelWaterfallEmitter>(
            context.PrimaryDevice, static_cast<DWORD>(std::max(1, count)), parameters);
        emitter->SetEnabled(true);
        voxelObject->AddComponent(emitter);
        AddRenderer(context, RenderMode::Particle, emitter);
    }

    auto& lod = context.VoxelLods[lodIndex];
    lod.DisplayName = displayName;
    lod.ObjectName = objectName;
    lod.Enabled = true;
    lod.SettingsPending = false;
    lod.VoxelCount = std::max(1, count);
    lod.Parameters = parameters;
    lod.Position = position;
    lod.Emitter = emitter;
    lod.CrossEmitter = crossEmitter;

    context.GameObjects.push_back(std::move(voxelObject));
}

void SceneFactory::CreateScene(const SceneFactoryContext& context) const
{
    auto skySphere = std::make_unique<GameObject>("Sky");
    skySphere->GetTransform()->SetScale({500, 500, 500});
    {
        const auto skyTextureIndex = context.Assets.GetTextureIndex(L"skyTex");
        const auto renderer = std::make_shared<SkyBox>(
            context.PrimaryDevice,
            context.Models[L"sphere"],
            *context.Assets.GetTexture(skyTextureIndex).get(),
            &context.SrvTexturesMemory,
            skyTextureIndex);

        skySphere->AddComponent(renderer);
        AddRenderer(context, RenderMode::SkyBox, renderer);
    }
    context.GameObjects.push_back(std::move(skySphere));

    auto quadRitem = std::make_unique<GameObject>("Quad");
    {
        auto renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"quad"]);
        renderer->SetModel(context.Models[L"quad"]);
        quadRitem->AddComponent(renderer);
        AddRenderer(context, RenderMode::Debug, renderer);
        AddRenderer(context, RenderMode::Quad, renderer);
    }
    context.GameObjects.push_back(std::move(quadRitem));

    auto sun = std::make_unique<GameObject>("Directional Light");
    auto light = std::make_shared<Light>(Directional);
    light->Direction({0.57735f, -0.57735f, 0.57735f});
    light->Strength({0.8f, 0.8f, 0.8f});
    sun->AddComponent(light);
    context.GameObjects.push_back(std::move(sun));

    for (int i = 0; i < 11; ++i)
    {
        auto nano = std::make_unique<GameObject>();
        nano->GetTransform()->SetPosition(Vector3::Right * -15.0f + Vector3::Forward * 12.0f * static_cast<float>(i));
        nano->GetTransform()->SetEulerRotate(Vector3(0.0f, -90.0f, 0.0f));
        auto renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"nano"]);
        nano->AddComponent(renderer);
        AddRenderer(context, RenderMode::Opaque, renderer);
        context.GameObjects.push_back(std::move(nano));

        auto doom = std::make_unique<GameObject>();
        doom->SetScale(0.08f);
        doom->GetTransform()->SetPosition(Vector3::Right * 15.0f + Vector3::Forward * 12.0f * static_cast<float>(i));
        doom->GetTransform()->SetEulerRotate(Vector3(0.0f, 90.0f, 0.0f));
        renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"doom"]);
        doom->AddComponent(renderer);
        AddRenderer(context, RenderMode::Opaque, renderer);
        context.GameObjects.push_back(std::move(doom));
    }

    for (int i = 0; i < 12; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            auto atlas = std::make_unique<GameObject>();
            atlas->GetTransform()->SetPosition(
                Vector3::Right * -60.0f + Vector3::Right * -30.0f * static_cast<float>(j) + Vector3::Up * 11.0f +
                Vector3::Forward * 10.0f * static_cast<float>(i));
            auto renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"atlas"]);
            atlas->AddComponent(renderer);
            AddRenderer(context, RenderMode::Opaque, renderer);
            context.GameObjects.push_back(std::move(atlas));

            auto pbody = std::make_unique<GameObject>();
            pbody->GetTransform()->SetPosition(
                Vector3::Right * 130.0f + Vector3::Right * -30.0f * static_cast<float>(j) + Vector3::Up * 11.0f +
                Vector3::Forward * 10.0f * static_cast<float>(i));
            renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"pbody"]);
            pbody->AddComponent(renderer);
            AddRenderer(context, RenderMode::Opaque, renderer);
            context.GameObjects.push_back(std::move(pbody));
        }
    }

    VoxelSimulationParameters nearParameters{};
    nearParameters.VoxelSize = 0.50f;
    nearParameters.SpawnHeight = 45.0f;
    nearParameters.WaterfallWidth = 22.0f;
    nearParameters.WaterfallDepth = 3.0f;
    nearParameters.InitialFallSpeed = 7.0f;
    nearParameters.Gravity = 34.0f;
    nearParameters.Seed = 1337;

    VoxelSimulationParameters mediumParameters = nearParameters;
    mediumParameters.VoxelSize = nearParameters.VoxelSize * 2.0f;
    mediumParameters.SpawnHeight = 42.0f;
    mediumParameters.WaterfallWidth = 26.0f;
    mediumParameters.WaterfallDepth = 3.6f;
    mediumParameters.Seed = 7331;

    VoxelSimulationParameters farParameters = nearParameters;
    farParameters.VoxelSize = nearParameters.VoxelSize * 4.0f;
    farParameters.SpawnHeight = 38.0f;
    farParameters.WaterfallWidth = 32.0f;
    farParameters.WaterfallDepth = 4.8f;
    farParameters.Seed = 9001;

    CreateVoxelLod(context, "NearVoxelWaterfall", "NearVoxelWaterfall", NearVoxelWaterfall,
                   Vector3(-34.0f, 0.0f, 24.0f), 18432, nearParameters);
    CreateVoxelLod(context, "MediumVoxelWaterfall", "MediumVoxelWaterfall", MediumVoxelWaterfall,
                   Vector3(0.0f, 0.0f, 24.0f), 4608, mediumParameters);
    CreateVoxelLod(context, "FarVoxelWaterfall", "FarVoxelWaterfall", FarVoxelWaterfall,
                   Vector3(36.0f, 0.0f, 24.0f), 1152, farParameters);

    context.VoxelLods[NearVoxelWaterfall].UpdateInterval = 1;
    context.VoxelLods[MediumVoxelWaterfall].UpdateInterval = 2;
    context.VoxelLods[FarVoxelWaterfall].UpdateInterval = 4;

    auto voxelFloor = std::make_unique<GameObject>();
    voxelFloor->GetTransform()->SetEulerRotate(Vector3(90.0f, 0.0f, 0.0f));
    voxelFloor->GetTransform()->SetPosition(Vector3(0.0f, nearParameters.FloorHeight, 28.0f));
    voxelFloor->GetTransform()->SetScale(Vector3(3.0f, 1.0f, 3.0f));
    auto renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"quad"]);
    voxelFloor->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);
    context.GameObjects.push_back(std::move(voxelFloor));

    auto platform = std::make_unique<GameObject>();
    platform->SetScale(0.2f);
    platform->GetTransform()->SetEulerRotate(Vector3(90.0f, 90.0f, 0.0f));
    platform->GetTransform()->SetPosition(Vector3::Backward * -130.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"platform"]);
    platform->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);

    auto rotater = std::make_unique<GameObject>();
    rotater->GetTransform()->SetParent(platform->GetTransform().get());
    rotater->GetTransform()->SetPosition(Vector3::Forward * 325.0f + Vector3::Left * 625.0f);
    rotater->GetTransform()->SetEulerRotate(Vector3(0.0f, -90.0f, 90.0f));
    rotater->AddComponent(std::make_shared<Rotater>(10.0f));

    auto camera = std::make_unique<GameObject>("MainCamera");
    camera->AddComponent(std::make_shared<CameraController>(35.0f, 80.0f, 60.0f));
    camera->GetTransform()->SetEulerRotate(Vector3(-8.0f, 180.0f, 0.0f));
    camera->GetTransform()->SetPosition(Vector3(0.0f, 24.0f, -72.0f));
    camera->AddComponent(std::make_shared<Camera>(context.AspectRatio));

    context.GameObjects.push_back(std::move(camera));
    context.GameObjects.push_back(std::move(rotater));

    auto stair = std::make_unique<GameObject>();
    stair->GetTransform()->SetParent(platform->GetTransform().get());
    stair->SetScale(0.2f);
    stair->GetTransform()->SetEulerRotate(Vector3(0.0f, 0.0f, 90.0f));
    stair->GetTransform()->SetPosition(Vector3::Left * 700.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"stair"]);
    stair->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);

    auto columns = std::make_unique<GameObject>();
    columns->GetTransform()->SetParent(stair->GetTransform().get());
    columns->SetScale(0.8f);
    columns->GetTransform()->SetEulerRotate(Vector3(0.0f, 0.0f, 90.0f));
    columns->GetTransform()->SetPosition(Vector3::Up * 2000.0f + Vector3::Forward * 900.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"columns"]);
    columns->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);

    auto fountain = std::make_unique<GameObject>();
    fountain->SetScale(0.005f);
    fountain->GetTransform()->SetEulerRotate(Vector3(90.0f, 0.0f, 0.0f));
    fountain->GetTransform()->SetPosition(Vector3::Up * 35.0f + Vector3::Backward * 77.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"fountain"]);
    fountain->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);

    context.GameObjects.push_back(std::move(platform));
    context.GameObjects.push_back(std::move(stair));
    context.GameObjects.push_back(std::move(columns));
    context.GameObjects.push_back(std::move(fountain));

    auto mountDragon = std::make_unique<GameObject>();
    mountDragon->GetTransform()->SetEulerRotate(Vector3(90.0f, 0.0f, 0.0f));
    mountDragon->GetTransform()->SetPosition(Vector3::Right * -960.0f + Vector3::Up * 45.0f + Vector3::Backward * 775.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"mountDragon"]);
    mountDragon->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);
    context.GameObjects.push_back(std::move(mountDragon));

    auto desertDragon = std::make_unique<GameObject>();
    desertDragon->GetTransform()->SetEulerRotate(Vector3(90.0f, 0.0f, 0.0f));
    desertDragon->GetTransform()->SetPosition(Vector3::Right * 960.0f + Vector3::Up * -5.0f + Vector3::Backward * 775.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"desertDragon"]);
    desertDragon->AddComponent(renderer);
    AddRenderer(context, RenderMode::Opaque, renderer);
    context.GameObjects.push_back(std::move(desertDragon));

    auto griffon = std::make_unique<GameObject>();
    griffon->GetTransform()->SetEulerRotate(Vector3(90.0f, 0.0f, 0.0f));
    griffon->SetScale(0.8f);
    griffon->GetTransform()->SetPosition(Vector3::Right * -355.0f + Vector3::Up * -7.0f + Vector3::Backward * 17.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"griffon"]);
    griffon->AddComponent(renderer);
    AddRenderer(context, RenderMode::OpaqueAlphaDrop, renderer);
    context.GameObjects.push_back(std::move(griffon));

    griffon = std::make_unique<GameObject>();
    griffon->SetScale(0.8f);
    griffon->GetTransform()->SetEulerRotate(Vector3(90.0f, 0.0f, 0.0f));
    griffon->GetTransform()->SetPosition(Vector3::Right * 355.0f + Vector3::Up * -7.0f + Vector3::Backward * 17.0f);
    renderer = std::make_shared<ModelRenderer>(context.PrimaryDevice, context.Models[L"griffon"]);
    griffon->AddComponent(renderer);
    AddRenderer(context, RenderMode::OpaqueAlphaDrop, renderer);
    context.GameObjects.push_back(std::move(griffon));
}
