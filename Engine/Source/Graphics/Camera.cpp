#include "Smile/Graphics/Camera.h"
#include "Smile/Math/MathUtils.h"
#include <cmath>

namespace Smile {

void Camera::Update(const CameraInput& input, f32 dt) {
    m_yaw   += input.Look.X;
    m_pitch  = Clamp(m_pitch + input.Look.Y, -89.0f, 89.0f);

    f32 yawRad   = m_yaw   * ToRad;
    f32 pitchRad = m_pitch * ToRad;

    // W/S seguem a direção exata da câmera (inclui pitch)
    Vec3 forward = {
        std::cos(pitchRad) * std::sin(yawRad),
        std::sin(pitchRad),
        std::cos(pitchRad) * std::cos(yawRad)
    };
    // A/D sempre horizontal — strafe não inclina com o olhar
    Vec3 right = { std::cos(yawRad), 0.0f, -std::sin(yawRad) };

    constexpr f32 kBaseSpeed = 3.0f;
    f32 speed = kBaseSpeed * input.Speed * dt;
    m_pos += (forward * input.Move.Z
           +  right   * input.Move.X
           +  Vec3::UnitY() * input.Move.Y) * speed;
}

Mat44 Camera::GetViewMatrix() const {
    f32 pitchRad = m_pitch * ToRad;
    f32 yawRad   = m_yaw   * ToRad;
    Vec3 forward = {
        std::cos(pitchRad) * std::sin(yawRad),
        std::sin(pitchRad),
        std::cos(pitchRad) * std::cos(yawRad)
    };
    return Mat44::LookAtLH(m_pos, m_pos + forward, Vec3::UnitY());
}

} // namespace Smile
