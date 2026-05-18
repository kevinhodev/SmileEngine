#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Input/CameraInput.h"

namespace Smile {
    class FCamera {
    public:
        FCamera() = default;

        void  Update(const CameraInput& Input, f32 DeltaTime);
        Mat44 GetViewMatrix() const;

        Vec3 GetPosition() const { return Position; }
        f32  GetPitch()    const { return Pitch; }
        f32  GetYaw()      const { return Yaw; }

    private:
        Vec3 Position   = { 0.0f, 1.0f, -2.5f };
        f32  Pitch = -22.0f;
        f32  Yaw   = 0.0f;
    };
} 
