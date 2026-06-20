#pragma once

#include "MemoryAllocator.h"
#include "SimpleMath.h"
#include "Source/Scene/SceneTransformPersistence.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GameObject;

struct SceneObjectEditorItem
{
    size_t ObjectIndex = 0;
    std::string Name;
    DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Rotation = DirectX::SimpleMath::Vector3::Zero;
    DirectX::SimpleMath::Vector3 Scale = DirectX::SimpleMath::Vector3::One;
};

class SceneTransformController
{
public:
    using ObjectList = PEPEngine::Allocator::custom_vector<std::shared_ptr<GameObject>>;
    using TransformAppliedCallback = std::function<void(const GameObject&, const DirectX::SimpleMath::Vector3&)>;
    using LogCallback = std::function<void(const std::wstring&)>;

    SceneTransformController();
    explicit SceneTransformController(std::filesystem::path transformPath);

    void Attach(ObjectList& sceneObjects);
    void SetTransformAppliedCallback(TransformAppliedCallback callback);
    void SetLogCallback(LogCallback callback);

    void Refresh();
    bool ApplyEditorItem(size_t editorIndex, const SceneObjectEditorItem& item);
    bool Save();
    bool Load();

    const std::filesystem::path& GetPath() const;
    std::vector<SceneObjectEditorItem>& GetEditorItems();
    const std::vector<SceneObjectEditorItem>& GetEditorItems() const;
    int& GetSelectedIndex();
    int GetSelectedIndex() const;
    bool IsDirty() const;

private:
    std::vector<SceneTransformRecord> BuildRecords() const;
    void ApplyRecord(const SceneTransformRecord& record);
    void ClampSelection();
    void Log(const std::wstring& message) const;

    ObjectList* objects = nullptr;
    std::filesystem::path path;
    std::vector<SceneObjectEditorItem> editorItems;
    int selectedIndex = 0;
    bool dirty = false;
    TransformAppliedCallback transformApplied;
    LogCallback log;
};
