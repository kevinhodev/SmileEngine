#pragma once

#include "Smile/Core/Types.h"

// Espelho C++ dos SMILE_RT_MASK_* de Shaders/GI/DDGICommon.hlsli — a documentacao de cada bit
// vive la. Aqui ficam so os valores que o lado CPU precisa: a mask gravada por instancia na TLAS
// (FRaytracingScene::CollectInstances) e a mask dos shadow rays, que viaja nos constant buffers
// do DDGI / ReSTIR / reflexoes como float.
//
// Cada instancia carrega UM bit de categoria; cada PASSE escolhe quais categorias enxerga.

namespace Smile {

    constexpr u32 kRTMaskOpaque      = 0x01u;
    constexpr u32 kRTMaskAlphaTest   = 0x02u;
    constexpr u32 kRTMaskTranslucent = 0x04u;
    constexpr u32 kRTMaskAll         = 0xFFu;

    // Gather difuso / visibilidade do GI: tudo menos translucido.
    constexpr u32 kRTMaskGather      = kRTMaskOpaque | kRTMaskAlphaTest;

    // Shadow rays no 2o hit. O toggle "folhagem sombreia GI" escolhe entre os dois; NENHUM dos
    // dois inclui translucido — vidro que deixa passar a luz do gather nao pode projetar sombra
    // dura no mesmo frame. (A UE tem um bit TRANSLUCENT_SHADOW proprio p/ quando isso for
    // desejavel; aqui a escolha e simplesmente nao sombrear.)
    constexpr u32 kRTMaskShadowFast  = kRTMaskOpaque;  // pula folhagem: traversal pura
    constexpr u32 kRTMaskShadowFull  = kRTMaskGather;  // folhagem sombreia via alpha-test

} // namespace Smile
