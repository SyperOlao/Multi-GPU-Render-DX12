#pragma once

#include "SimpleMath.h"

#include <filesystem>
#include <string>
#include <vector>

struct SceneTransformRecord
{
    size_t Index = 0;
    std::string Name;
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Rotation = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Scale = DirectX::SimpleMath::Vector3::One;
};

class SceneTransformPersistence
{
public:
    static bool Save(const std::filesystem::path& path, const std::vector<SceneTransformRecord>& records);
    static bool Load(const std::filesystem::path& path, std::vector<SceneTransformRecord>& records);
};
