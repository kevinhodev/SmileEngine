#include "Smile/Graphics/Scene/Camera.h"
#include "Smile/Math/MathUtils.h"
#include <cmath>

namespace Smile {
    Vec3 FCamera::GetForward() const {
        const f32 PitchRad = Pitch * ToRad;
        const f32 YawRad   = Yaw   * ToRad;
        return {
            std::cos(PitchRad) * std::sin(YawRad),
            std::sin(PitchRad),
            std::cos(PitchRad) * std::cos(YawRad)
        };
    }

    void FCamera::Update(const CameraInput& _Input, f32 _DeltaTime) {
        Yaw   += _Input.Look.X;
        Pitch  = Clamp(Pitch + _Input.Look.Y, -89.0f, 89.0f);

        const f32 YawRad = Yaw * ToRad;
        const Vec3 Forward = GetForward();

        Vec3 Right = { std::cos(YawRad), 0.0f, -std::sin(YawRad) };

        constexpr f32 kBaseSpeed = 3.0f;
        f32 Speed = kBaseSpeed * _Input.Speed * _DeltaTime;
        Position += (Forward * _Input.Move.Z
               +  Right   * _Input.Move.X
               +  Vec3::UnitY() * _Input.Move.Y) * Speed;
    }

    Mat44 FCamera::GetViewMatrix() const {
        const Vec3 Forward = GetForward();
        return Mat44::LookAtLH(Position, Position + Forward, Vec3::UnitY());
    }
}
