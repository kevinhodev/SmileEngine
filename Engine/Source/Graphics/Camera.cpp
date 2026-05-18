#include "Smile/Graphics/Camera.h"
#include "Smile/Math/MathUtils.h"
#include <cmath>

namespace Smile {
    void FCamera::Update(const CameraInput& _Input, f32 _DeltaTime) {
        Yaw   += _Input.Look.X;
        Pitch  = Clamp(Pitch + _Input.Look.Y, -89.0f, 89.0f);

        f32 YawRad   = Yaw   * ToRad;
        f32 PitchRad = Pitch * ToRad;

        Vec3 forward = {
            std::cos(PitchRad) * std::sin(YawRad),
            std::sin(PitchRad),
            std::cos(PitchRad) * std::cos(YawRad)
        };

        Vec3 right = { std::cos(YawRad), 0.0f, -std::sin(YawRad) };

        constexpr f32 kBaseSpeed = 3.0f;
        f32 speed = kBaseSpeed * _Input.Speed * _DeltaTime;
        Position += (forward * _Input.Move.Z
               +  right   * _Input.Move.X
               +  Vec3::UnitY() * _Input.Move.Y) * speed;
    }

    Mat44 FCamera::GetViewMatrix() const {
        f32 PitchRad = Pitch * ToRad;
        f32 YawRad   = Yaw   * ToRad;
        Vec3 forward = {
            std::cos(PitchRad) * std::sin(YawRad),
            std::sin(PitchRad),
            std::cos(PitchRad) * std::cos(YawRad)
        };
        return Mat44::LookAtLH(Position, Position + forward, Vec3::UnitY());
    }
}
