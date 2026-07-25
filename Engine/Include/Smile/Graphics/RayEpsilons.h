#pragma once

#include "Smile/Core/Types.h"

// Espelho C++ dos epsilons de raio que nascem na CPU. O perfil completo (as tres familias e o
// porque de nao calibra-las juntas) vive em Shaders/RayEpsilons.hlsli — este header existe so
// para os valores que sao escritos num constant buffer em vez de definidos no shader.
//
// Manter os dois em sincronia: um sweep de calibracao que mexa so num dos lados deixa metade do
// renderer no valor antigo, que foi exatamente o bug que motivou a centralizacao.

namespace Smile {

    // Familia (1) — ORIGEM. Deslocamento da origem dos shadow rays disparados no ponto de hit
    // (sol e luzes puntuais), em metros. Chega ao HitShading.hlsli por TraceParams.w em TODOS os
    // passes que sombreiam um hit: DDGI, reflexoes e ReSTIR GI.
    //
    // E independente do piso do OffsetRayGBuffer (kRayOriginFloorMin): baixar so o piso deixa
    // este valor dominando o contato. Os dois precisam descer juntos no sweep.
    constexpr f32 kHitShadowRayBias = 0.2f;

} // namespace Smile
