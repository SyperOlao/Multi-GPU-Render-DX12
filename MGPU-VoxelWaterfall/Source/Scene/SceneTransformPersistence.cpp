#include "Source/Scene/SceneTransformPersistence.h"

#include <fstream>
#include <iomanip>

bool SceneTransformPersistence::Save(const std::filesystem::path& path,
                                     const std::vector<SceneTransformRecord>& records)
{
    std::filesystem::create_directories(path.parent_path());

    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream)
        return false;

    stream << "VoxelWaterfallSceneTransformsV1\n";
    stream << std::setprecision(9);
    for (const auto& record : records)
    {
        stream << record.Index << '\t' << std::quoted(record.Name)
            << '\t' << record.Position.x << '\t' << record.Position.y << '\t' << record.Position.z
            << '\t' << record.Rotation.x << '\t' << record.Rotation.y << '\t' << record.Rotation.z
            << '\t' << record.Scale.x << '\t' << record.Scale.y << '\t' << record.Scale.z
            << '\n';
    }

    return true;
}

bool SceneTransformPersistence::Load(const std::filesystem::path& path, std::vector<SceneTransformRecord>& records)
{
    std::ifstream stream(path);
    if (!stream)
        return false;

    std::string header;
    std::getline(stream, header);
    if (header != "VoxelWaterfallSceneTransformsV1")
        return false;

    std::vector<SceneTransformRecord> loaded;
    SceneTransformRecord record{};
    while (stream >> record.Index >> std::quoted(record.Name)
           >> record.Position.x >> record.Position.y >> record.Position.z
           >> record.Rotation.x >> record.Rotation.y >> record.Rotation.z
           >> record.Scale.x >> record.Scale.y >> record.Scale.z)
    {
        loaded.push_back(record);
    }

    records = std::move(loaded);
    return true;
}
