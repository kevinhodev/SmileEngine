#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Core/Types.h"
#include <string>

namespace Smile {
    enum class ELightType : u32 { Point = 0, Spot = 1 };

    // Luz puntual da cena (point/spot). Unidades: Intensity = radiancia a 1 unidade de mundo
    // (inverse-square dali em diante, estilo candela). AttenuationRadius corta a contribuicao
    // a zero via janela (1-(d/r)^4)^2 (Unreal) — a luz morre exatamente no raio, o que tambem
    // limita o culling. SourceRadius e o "bulb" (Cry): piso da distancia no inverse-square,
    // superficie encostada na luz nao estoura a branco. Na F4 ele vira tambem o raio da fonte
    // p/ o especular de area (representative point).
    struct FLight {
        std::string Name = "Luz";
        ELightType  Type = ELightType::Point;

        Vec3 Position  = { 0.0f, 2.0f, 0.0f };
        Vec3 Direction = { 0.0f, -1.0f, 0.0f }; // eixo do cone (spot); ignorado no point

        Vec3 Color     = { 1.0f, 1.0f, 1.0f };
        f32  Intensity = 10.0f;

        f32  AttenuationRadius = 10.0f;
        f32  SourceRadius      = 0.05f; // bulb: distancia minima do inverse-square

        f32  InnerConeDeg = 25.0f;      // spot: onde o falloff angular comeca
        f32  OuterConeDeg = 40.0f;      // spot: cone externo (contribuicao zero)

        bool Enabled = true;
    };
}
