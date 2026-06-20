#include "Source/Scene/SceneTransformController.h"

#include "GameObject.h"
#include "Transform.h"

#include <algorithm>

namespace
{
    const std::filesystem::path DefaultSceneTransformPath =
        std::filesystem::path(L"SceneOverrides") / L"VoxelWaterfallSceneTransforms.tsv";
}

SceneTransformController::SceneTransformController()
    : SceneTransformController(DefaultSceneTransformPath)
{
}

SceneTransformController::SceneTransformController(std::filesystem::path transformPath)
    : path(std::move(transformPath))
{
}

void SceneTransformController::Attach(ObjectList& sceneObjects)
{
    objects = &sceneObjects;
    Refresh();
}

void SceneTransformController::SetTransformAppliedCallback(TransformAppliedCallback callback)
{
    transformApplied = std::move(callback);
}

void SceneTransformController::SetLogCallback(LogCallback callback)
{
    log = std::move(callback);
}

void SceneTransformController::Refresh()
{
    editorItems.clear();

    if (!objects)
    {
        selectedIndex = 0;
        return;
    }

    editorItems.reserve(objects->size());
    for (size_t objectIndex = 0; objectIndex < objects->size(); ++objectIndex)
    {
        const auto& object = (*objects)[objectIndex];
        if (!object)
            continue;

        const auto transform = object->GetTransform();
        SceneObjectEditorItem item{};
        item.ObjectIndex = objectIndex;
        item.Name = object->GetName();
        item.Position = transform->GetLocalPosition();
        item.Rotation = transform->GetEulerAngels();
        item.Scale = transform->GetScale();
        editorItems.push_back(item);
    }

    ClampSelection();
}

bool SceneTransformController::ApplyEditorItem(const size_t editorIndex, const SceneObjectEditorItem& item)
{
    if (!objects || editorIndex >= editorItems.size())
        return false;

    const size_t objectIndex = editorItems[editorIndex].ObjectIndex;
    if (objectIndex >= objects->size() || !(*objects)[objectIndex])
        return false;

    auto& object = *(*objects)[objectIndex];
    const auto transform = object.GetTransform();
    transform->SetPosition(item.Position);
    transform->SetEulerRotate(item.Rotation);
    transform->SetScale(item.Scale);

    editorItems[editorIndex] = item;
    editorItems[editorIndex].ObjectIndex = objectIndex;
    editorItems[editorIndex].Name = object.GetName();
    dirty = true;

    if (transformApplied)
        transformApplied(object, item.Position);

    return true;
}

bool SceneTransformController::Save()
{
    if (!objects)
        return false;

    if (SceneTransformPersistence::Save(path, BuildRecords()))
    {
        dirty = false;
        Log(L"Saved scene transforms: " + path.wstring());
        return true;
    }

    Log(L"Failed to save scene transforms: " + path.wstring());
    return false;
}

bool SceneTransformController::Load()
{
    if (!objects)
        return false;

    std::vector<SceneTransformRecord> records;
    if (!SceneTransformPersistence::Load(path, records))
    {
        Refresh();
        return false;
    }

    for (const auto& record : records)
        ApplyRecord(record);

    dirty = false;
    Refresh();
    Log(L"Loaded scene transforms: " + path.wstring());
    return true;
}

const std::filesystem::path& SceneTransformController::GetPath() const
{
    return path;
}

std::vector<SceneObjectEditorItem>& SceneTransformController::GetEditorItems()
{
    return editorItems;
}

const std::vector<SceneObjectEditorItem>& SceneTransformController::GetEditorItems() const
{
    return editorItems;
}

int& SceneTransformController::GetSelectedIndex()
{
    return selectedIndex;
}

int SceneTransformController::GetSelectedIndex() const
{
    return selectedIndex;
}

bool SceneTransformController::IsDirty() const
{
    return dirty;
}

std::vector<SceneTransformRecord> SceneTransformController::BuildRecords() const
{
    std::vector<SceneTransformRecord> records;
    if (!objects)
        return records;

    records.reserve(objects->size());
    for (size_t i = 0; i < objects->size(); ++i)
    {
        if (!(*objects)[i])
            continue;

        const auto transform = (*objects)[i]->GetTransform();
        SceneTransformRecord record{};
        record.Index = i;
        record.Name = (*objects)[i]->GetName();
        record.Position = transform->GetLocalPosition();
        record.Rotation = transform->GetEulerAngels();
        record.Scale = transform->GetScale();
        records.push_back(record);
    }

    return records;
}

void SceneTransformController::ApplyRecord(const SceneTransformRecord& record)
{
    if (!objects || record.Index >= objects->size() || !(*objects)[record.Index])
        return;

    auto& object = *(*objects)[record.Index];
    if (object.GetName() != record.Name)
        return;

    const auto transform = object.GetTransform();
    transform->SetPosition(record.Position);
    transform->SetEulerRotate(record.Rotation);
    transform->SetScale(record.Scale);

    if (transformApplied)
        transformApplied(object, record.Position);
}

void SceneTransformController::ClampSelection()
{
    if (editorItems.empty())
        selectedIndex = 0;
    else
        selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(editorItems.size()) - 1);
}

void SceneTransformController::Log(const std::wstring& message) const
{
    if (log)
        log(message);
}
