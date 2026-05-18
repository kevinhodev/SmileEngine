#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Input/CameraInput.h"

namespace Smile {

class Camera {
public:
    Camera() = default;

    void  Update(const CameraInput& input, f32 dt);
    Mat44 GetViewMatrix() const;

    Vec3 GetPosition() const { return m_pos; }
    f32  GetPitch()    const { return m_pitch; }
    f32  GetYaw()      const { return m_yaw; }

private:
    Vec3 m_pos   = { 0.0f, 1.0f, -2.5f };
    f32  m_pitch = -22.0f;
    f32  m_yaw   = 0.0f;
};

} // namespace Smile
