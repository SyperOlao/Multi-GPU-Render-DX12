#include "Source/Assets/SampleAssetManifest.h"

const std::vector<TextureAssetEntry>& SampleAssetManifest::Textures()
{
    static const std::vector<TextureAssetEntry> entries = {
        {L"seamless", L"Data\\Textures\\seamless_grass.jpg", false},
        {L"defaultNormalMap", L"Data\\Textures\\default_nmap.dds", true}
    };
    return entries;
}

const std::vector<ModelAssetEntry>& SampleAssetManifest::Models()
{
    static const std::vector<ModelAssetEntry> entries = {};
    return entries;
}
