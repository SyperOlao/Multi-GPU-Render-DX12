#include "Source/Assets/SampleAssetManifest.h"

const std::vector<TextureAssetEntry>& SampleAssetManifest::Textures()
{
    static const std::vector<TextureAssetEntry> entries = {
        {L"bricksTex", L"Data\\Textures\\bricks2.dds", false},
        {L"stoneTex", L"Data\\Textures\\stone.dds", false},
        {L"tileTex", L"Data\\Textures\\tile.dds", false},
        {L"fenceTex", L"Data\\Textures\\WireFence.dds", false},
        {L"waterTex", L"Data\\Textures\\water1.dds", false},
        {L"skyTex", L"Data\\Textures\\skymap.dds", false},
        {L"grassTex", L"Data\\Textures\\grass.dds", false},
        {L"treeArrayTex", L"Data\\Textures\\treeArray2.dds", false},
        {L"seamless", L"Data\\Textures\\seamless_grass.jpg", false},
        {L"bricksNormalMap", L"Data\\Textures\\bricks2_nmap.dds", true},
        {L"tileNormalMap", L"Data\\Textures\\tile_nmap.dds", true},
        {L"defaultNormalMap", L"Data\\Textures\\default_nmap.dds", true}
    };
    return entries;
}

const std::vector<ModelAssetEntry>& SampleAssetManifest::Models()
{
    using DirectX::SimpleMath::Matrix;

    static const std::vector<ModelAssetEntry> entries = {
        {L"nano", "Data\\Objects\\Nanosuit\\Nanosuit.obj"},
        {L"atlas", "Data\\Objects\\Atlas\\Atlas.obj"},
        {L"pbody", "Data\\Objects\\P-Body\\P-Body.obj"},
        {L"griffon", "Data\\Objects\\Griffon\\Griffon.FBX", Matrix::CreateScale(0.1f)},
        {L"mountDragon", "Data\\Objects\\MOUNTAIN_DRAGON\\MOUNTAIN_DRAGON.FBX", Matrix::CreateScale(0.1f)},
        {L"desertDragon", "Data\\Objects\\DesertDragon\\DesertDragon.FBX", Matrix::CreateScale(0.1f)},
        {L"stair", "Data\\Objects\\Temple\\SM_AsianCastle_A.FBX"},
        {L"columns", "Data\\Objects\\Temple\\SM_AsianCastle_E.FBX"},
        {L"fountain", "Data\\Objects\\Temple\\SM_Fountain.FBX"},
        {L"platform", "Data\\Objects\\Temple\\SM_PlatformSquare.FBX"},
        {L"doom", "Data\\Objects\\DoomSlayer\\doommarine.obj"}
    };
    return entries;
}

