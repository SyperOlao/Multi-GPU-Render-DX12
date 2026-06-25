#pragma once

#include "Component.h"
#include "Source/Voxels/VoxelTypes.h"

class Camera;
class KeyboardDevice;
class Mousepad;

class VoxelResearchCameraController final : public Component
{
public:
    VoxelResearchCameraController();

    void SetMode(VoxelResearchCameraMode mode);
    VoxelResearchCameraMode GetMode() const { return mode; }
    void SetDeterministicTime(double fixedSimulationTimeSeconds);
    void ResetRoute();
    void SetInputBlocked(bool blocked);
    bool IsInputBlocked() const { return inputBlocked; }
    const char* GetCameraPathName() const;
    float GetFixedFovDegrees() const { return fixedFovDegrees; }
    float GetFixedNearPlane() const { return fixedNearPlane; }
    float GetFixedFarPlane() const { return fixedFarPlane; }

    void Update() override;

private:
    struct CameraKeyframe
    {
        double Time = 0.0;
        DirectX::SimpleMath::Vector3 Position = DirectX::SimpleMath::Vector3::Zero;
        DirectX::SimpleMath::Vector3 Target = DirectX::SimpleMath::Vector3::Zero;
    };

    VoxelResearchCameraMode mode = VoxelResearchCameraMode::FixedOverview;
    KeyboardDevice* keyboard = nullptr;
    Mousepad* mouse = nullptr;
    double deterministicTimeSeconds = 0.0;
    double routeTimeOffsetSeconds = 0.0;
    bool inputBlocked = true;
    float moveSpeed = 28.0f;
    float xMouseSpeed = 80.0f;
    float yMouseSpeed = 60.0f;
    float fixedFovDegrees = 58.0f;
    float fixedNearPlane = 0.25f;
    float fixedFarPlane = 900.0f;

    void UpdateInteractive();
    void UpdateDeterministic();
    void DrainInputBuffers() const;
    void ApplyCameraPose(const DirectX::SimpleMath::Vector3& position,
                         const DirectX::SimpleMath::Vector3& target) const;
    void ApplyCameraLens() const;
    CameraKeyframe EvaluateRoute(const CameraKeyframe* keyframes, size_t count, double duration) const;
};
