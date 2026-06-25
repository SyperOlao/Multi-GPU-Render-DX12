#include "Source/Scene/VoxelResearchCameraController.h"

#include "Camera.h"
#include "d3dApp.h"
#include "GameObject.h"
#include "KeyboardDevice.h"
#include "Mousepad.h"
#include "Transform.h"

#include <algorithm>
#include <cmath>
#include <iterator>

using namespace DirectX::SimpleMath;

namespace
{
    constexpr double LodSweepDurationSeconds = 12.0;
    constexpr double BenchmarkRouteDurationSeconds = 24.0;

    float SmoothStep(const float t)
    {
        const float x = std::clamp(t, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }

    Vector3 LerpVector(const Vector3& a, const Vector3& b, const float t)
    {
        return a + (b - a) * t;
    }
}

VoxelResearchCameraController::VoxelResearchCameraController()
{
    auto& app = static_cast<Common::D3DApp&>(Common::D3DApp::GetApp());
    keyboard = app.GetKeyboard();
    mouse = app.GetMouse();
}

void VoxelResearchCameraController::SetMode(const VoxelResearchCameraMode newMode)
{
    if (mode == newMode)
        return;
    mode = newMode;
    ResetRoute();
}

void VoxelResearchCameraController::SetDeterministicTime(const double fixedSimulationTimeSeconds)
{
    deterministicTimeSeconds = fixedSimulationTimeSeconds;
}

void VoxelResearchCameraController::ResetRoute()
{
    routeTimeOffsetSeconds = deterministicTimeSeconds;
}

void VoxelResearchCameraController::SetInputBlocked(const bool blocked)
{
    inputBlocked = blocked;
}

const char* VoxelResearchCameraController::GetCameraPathName() const
{
    switch (mode)
    {
    case VoxelResearchCameraMode::Interactive:
        return "Interactive";
    case VoxelResearchCameraMode::FixedOverview:
        return "FixedOverview";
    case VoxelResearchCameraMode::FixedOcclusion:
        return "FixedOcclusion";
    case VoxelResearchCameraMode::WaterfallCloseup:
        return "WaterfallCloseup";
    case VoxelResearchCameraMode::LodSweepRoute:
        return "LodSweepRoute";
    case VoxelResearchCameraMode::BenchmarkRoute:
        return "BenchmarkRoute";
    case VoxelResearchCameraMode::DemoMixedOverview:
        return "DemoMixedOverview";
    default:
        return "Unknown";
    }
}

void VoxelResearchCameraController::Update()
{
    if (mode == VoxelResearchCameraMode::Interactive && !inputBlocked)
    {
        UpdateInteractive();
        return;
    }

    DrainInputBuffers();
    UpdateDeterministic();
}

void VoxelResearchCameraController::UpdateInteractive()
{
    ApplyCameraLens();

    while (!keyboard->CharBufferIsEmpty())
        keyboard->ReadChar();
    while (!keyboard->KeyBufferIsEmpty())
        keyboard->ReadKey();

    float cameraSpeed = moveSpeed;
    const float dt = Common::D3DApp::GetApp().GetTimer()->DeltaTime();
    auto tr = gameObject->GetTransform();

    if (keyboard->KeyIsPressed(VK_SHIFT))
        cameraSpeed *= 3.0f;

    while (!mouse->EventBufferIsEmpty())
    {
        const MouseEvent me = mouse->ReadEvent();
        if (mouse->IsRightDown() && me.GetType() == MouseEvent::EventType::RAW_MOVE)
        {
            tr->AdjustEulerRotation(
                -static_cast<float>(me.GetPosY()) * dt * yMouseSpeed,
                static_cast<float>(me.GetPosX()) * dt * xMouseSpeed,
                0.0f);
        }
    }

    if (keyboard->KeyIsPressed('W'))
        tr->AdjustPosition(tr->GetForwardVector() * cameraSpeed * dt);
    if (keyboard->KeyIsPressed('S'))
        tr->AdjustPosition(tr->GetBackwardVector() * cameraSpeed * dt);
    if (keyboard->KeyIsPressed('A'))
        tr->AdjustPosition(tr->GetLeftVector() * cameraSpeed * dt);
    if (keyboard->KeyIsPressed('D'))
        tr->AdjustPosition(tr->GetRightVector() * cameraSpeed * dt);
    if (keyboard->KeyIsPressed(VK_SPACE))
        tr->AdjustPosition(tr->GetUpVector() * cameraSpeed * dt);
    if (keyboard->KeyIsPressed('Z'))
        tr->AdjustPosition(tr->GetDownVector() * cameraSpeed * dt);
}

void VoxelResearchCameraController::UpdateDeterministic()
{
    ApplyCameraLens();

    switch (mode)
    {
    case VoxelResearchCameraMode::FixedOcclusion:
        ApplyCameraPose(Vector3(-14.0f, 12.5f, -28.0f), Vector3(-2.0f, 12.0f, -1.0f));
        break;
    case VoxelResearchCameraMode::WaterfallCloseup:
        ApplyCameraPose(Vector3(0.0f, 18.0f, -22.0f), Vector3(0.0f, 18.5f, 1.0f));
        break;
    case VoxelResearchCameraMode::LodSweepRoute:
    {
        static constexpr CameraKeyframe keyframes[] = {
            {0.0, Vector3(-17.0f, 9.0f, -20.0f), Vector3(-8.0f, 7.0f, -9.0f)},
            {4.0, Vector3(-8.0f, 16.0f, -38.0f), Vector3(0.0f, 12.0f, -2.0f)},
            {8.0, Vector3(10.0f, 24.0f, -70.0f), Vector3(0.0f, 13.0f, 2.0f)},
            {12.0, Vector3(18.0f, 31.0f, -105.0f), Vector3(0.0f, 14.0f, 4.0f)}
        };
        const auto pose = EvaluateRoute(keyframes, std::size(keyframes), LodSweepDurationSeconds);
        ApplyCameraPose(pose.Position, pose.Target);
        break;
    }
    case VoxelResearchCameraMode::BenchmarkRoute:
    {
        static constexpr CameraKeyframe keyframes[] = {
            {0.0, Vector3(0.0f, 19.0f, -58.0f), Vector3(0.0f, 11.0f, -2.0f)},
            {6.0, Vector3(-22.0f, 14.0f, -35.0f), Vector3(-6.0f, 9.5f, -2.0f)},
            {12.0, Vector3(-13.0f, 12.0f, -27.0f), Vector3(-2.5f, 11.5f, -1.0f)},
            {18.0, Vector3(2.0f, 18.0f, -23.0f), Vector3(0.0f, 18.0f, 2.5f)},
            {24.0, Vector3(0.0f, 19.0f, -58.0f), Vector3(0.0f, 11.0f, -2.0f)}
        };
        const auto pose = EvaluateRoute(keyframes, std::size(keyframes), BenchmarkRouteDurationSeconds);
        ApplyCameraPose(pose.Position, pose.Target);
        break;
    }
    case VoxelResearchCameraMode::DemoMixedOverview:
        ApplyCameraPose(Vector3(0.0f, 22.0f, -48.0f), Vector3(0.0f, 17.0f, -1.0f));
        break;
    case VoxelResearchCameraMode::FixedOverview:
    case VoxelResearchCameraMode::Interactive:
    default:
        ApplyCameraPose(Vector3(0.0f, 19.0f, -58.0f), Vector3(0.0f, 11.5f, -2.0f));
        break;
    }
}

void VoxelResearchCameraController::DrainInputBuffers() const
{
    while (!keyboard->CharBufferIsEmpty())
        keyboard->ReadChar();
    while (!keyboard->KeyBufferIsEmpty())
        keyboard->ReadKey();
    while (!mouse->EventBufferIsEmpty())
        mouse->ReadEvent();
}

void VoxelResearchCameraController::ApplyCameraPose(const Vector3& position, const Vector3& target) const
{
    auto transform = gameObject->GetTransform();
    const Matrix view = Matrix::CreateLookAt(position, target, Vector3::Up);
    transform->SetLocalMatrix(view.Invert());
}

void VoxelResearchCameraController::ApplyCameraLens() const
{
    const auto camera = gameObject->GetComponent<Camera>();
    if (!camera)
        return;

    camera->SetFov(fixedFovDegrees);
    camera->SetNearZ(fixedNearPlane);
    camera->SetFarZ(fixedFarPlane);
}

VoxelResearchCameraController::CameraKeyframe VoxelResearchCameraController::EvaluateRoute(
    const CameraKeyframe* keyframes,
    const size_t count,
    const double duration) const
{
    if (!keyframes || count == 0)
        return {};
    if (count == 1 || duration <= 0.0)
        return keyframes[0];

    double localTime = std::fmod(std::max(0.0, deterministicTimeSeconds - routeTimeOffsetSeconds), duration);
    if (localTime < 0.0)
        localTime += duration;

    const CameraKeyframe* left = &keyframes[0];
    const CameraKeyframe* right = &keyframes[count - 1];
    for (size_t i = 0; i + 1 < count; ++i)
    {
        if (localTime >= keyframes[i].Time && localTime <= keyframes[i + 1].Time)
        {
            left = &keyframes[i];
            right = &keyframes[i + 1];
            break;
        }
    }

    const double segmentDuration = std::max(0.0001, right->Time - left->Time);
    const float t = SmoothStep(static_cast<float>((localTime - left->Time) / segmentDuration));
    return {
        localTime,
        LerpVector(left->Position, right->Position, t),
        LerpVector(left->Target, right->Target, t)
    };
}
