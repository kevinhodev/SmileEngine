#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    // Parametros do gather COMPLETO do DDGI (Chebyshev + bias de superficie + skip de sonda
    // inativa) usado no 2o bounce, dentro do ShadeSurfaceHit (Shaders/GI/HitShading.hlsli).
    //
    // Vive num header proprio, e nao no DDGI.h, porque os TRES passes que incluem o HitShading
    // precisam dele — DDGI, reflexoes e ReSTIR — e nao faz sentido os dois ultimos passarem a
    // depender do header do DDGI so por causa de seis floats.
    //
    // Mesmo regime do FRayEpsilonProfile: o dono e o Renderer, que empurra a copia todo frame
    // para os tres. Sem isso cada passe teria a propria copia e um knob mexeria em metade deles
    // — e o bounce pesaria as sondas com uma politica enquanto a tela usa outra.
    struct FGIHitSampling {
        f32 DistTile   = 14.0f; // texels do tile de distancia (interior, sem borda)
        f32 DistAtlasW = 1.0f;
        f32 DistAtlasH = 1.0f;
        f32 SkipMode   = 0.0f;  // 0 = nao pula sonda inativa, 1 = pula, 2 = procura substituta
        f32 BiasScale  = 0.2f;  // o `bias` do GetDDGISurfaceBias do Flax
        f32 BiasMax    = 0.0f;  // teto do bias em metros; 0 = sem teto (comportamento historico)
    };
}
