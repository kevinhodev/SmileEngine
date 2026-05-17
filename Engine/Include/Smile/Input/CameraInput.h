#pragma once

#include "Smile/Math/Math.h"

namespace Smile {

struct CameraInput {
    Vec3 Move;   // Espaço local da câmera: X=right, Y=up, Z=forward
    Vec2 Look;   // Delta de rotação em graus: X=yaw, Y=pitch
    f32  Speed;  // Multiplicador (1.0 normal, 4.0 com Shift)
};

} // namespace Smile
