#pragma once

#include <SimpleMath.h>

#include <string>
#include <vector>

struct TextureAssetEntry
{
    std::wstring Name;
    std::wstring RelativePath;
    bool IsNormalMap = false;
};

struct ModelAssetEntry
{
    std::wstring Name;
    std::string RelativePath;
    DirectX::SimpleMath::Matrix Scale = DirectX::SimpleMath::Matrix::CreateScale(1.0f);
};

class SampleAssetManifest
{
public:
    static const std::vector<TextureAssetEntry>& Textures();
    static const std::vector<ModelAssetEntry>& Models();
};

