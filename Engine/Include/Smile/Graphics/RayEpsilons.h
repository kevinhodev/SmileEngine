#pragma once

#include "Smile/Core/Types.h"

// Perfil de epsilons dos raios de GI / reflexao, em METROS (unidade de mundo da engine).
//
// UM perfil para a engine inteira: ReSTIR GI, reflexoes e DDGI compartilham os mesmos raios de
// gather e de sombra, e ter copias por passe foi exatamente o bug que a centralizacao fechou (o
// DDGI ficava em 20 cm enquanto os outros desciam). O Renderer e o dono; as tres classes recebem
// o perfil no UpdatePerFrame e escrevem nos proprios constant buffers.
//
// A documentacao das TRES FAMILIAS (e do porque nao devem ser calibradas juntas) vive em
// Shaders/RayEpsilons.hlsli, junto dos valores que so o shader usa.
//
// Estes campos sao KNOBS DE CALIBRACAO expostos na pagina "Iluminacao global" das configuracoes.
// Os defaults reproduzem exatamente o comportamento anterior a exposicao — mudar qualquer um
// invalida os reservoirs do ReSTIR e o historico do NRD (ver Renderer::SetRayEpsilons).

namespace Smile {

    struct FRayEpsilonProfile {
        // --- Familia (1): ORIGEM do raio que sai do G-buffer (OffsetRayGBuffer) ---------------

        // Piso constante somado ao offset ULP.
        //
        // 2e-2 (2 cm) e o resultado do sweep de 2026-07-25, medido no Bistro:
        //   - 2e-1 (o legado) alem das manchas ILUMINAVA a cena errado: os raios de gather
        //     nasciam fora do predio, viam a rua e traziam a energia de volta. Com o piso baixo o
        //     interior escurece — e escurecer e o correto.
        //   - manchas ja limpas em 2e-2.
        //   - abaixo de ~9e-3 os quadros embutidos na parede ficam PRETOS. Isso e bug de ASSET,
        //     nao de renderer: confirmado movendo o prop p/ frente com o piso em 0 — volta a
        //     iluminar. Quando os assets forem corrigidos, este piso pode cair de verdade.
        //
        // NAO tentar substituir por bias angular: ele da o maximo ao raio RASANTE e 20% ao
        // perpendicular, e o gather cosseno e quase todo perpendicular — precisaria de 45 mm no
        // maximo p/ entregar 9 mm onde importa. Fica pior que um piso liso.
        f32 OriginFloorMin      = 2e-2f;

        // Termo proporcional a distancia da camera. Heuristico: a quantizacao do depth (D32
        // reverse-Z, near 0.1 / far 4000) da ~0,003 mm a 50 m, e este termo da 10 mm na mesma
        // distancia. Cobre shading normal e geometria coplanar, nao precisao. Varrer ate 0.
        f32 OriginFloorPerMeter = 2e-4f;

        // Bias modulado por angulo, estilo Lumen (ApplyPositionBias): raio rasante leva o maximo,
        // raio perpendicular leva OriginAngularMinRatio * esse maximo. O Lumen usa 0,5 mm / 0,1 mm
        // (razao 0,2) com a shading normal e coordenadas camera-relative. 0 = desligado, que e o
        // default p/ nao mudar comportamento antes do A/B.
        f32 OriginAngularMax    = 0.0f;

        // Deslocamento da origem dos shadow rays no 2o HIT (sol e puntuais) — familia (1)
        // tambem, so que a superficie de partida e o ponto de hit em vez do G-buffer. Vai por
        // TraceParams.w -> FHitShadeParams::ShadowRayBias. Independente do piso acima: baixar so
        // o piso deixa este dominando o contato.
        f32 HitShadowRayBias    = 2e-1f;
        // Piso do valor acima. ATENCAO: em 1 mm ele mascara qualquer sweep de HitShadowRayBias
        // abaixo disso — por isso e knob, nao constante.
        f32 ShadowRayBiasMin    = 1e-3f;

        // --- Familia (2): INTERVALO dos raios ------------------------------------------------

        // TMin dos shadow rays. Soma com o bias de origem: em 1 cm, oclusor a menos de 1 cm do
        // hit e invisivel mesmo depois de o bias cair.
        f32 ShadowRayTMin       = 1e-2f;

        // Visibility rays do reuso espacial (correcao de bias MIS e shading visibility).
        f32 VisRayTMin          = 2e-2f;
        f32 VisRayEndMargin     = 5e-2f; // para antes da superficie do x2

        // --- Frescor temporal ------------------------------------------------------------------

        // Idade maxima da amostra no reservoir, em frames (RTXDI maxReservoirAge = 30).
        // 0 = expiracao desligada, que e o estado atual.
        f32 MaxAge              = 0.0f;

        // Razao entre o bias angular perpendicular e o rasante (Lumen: 0.01/0.05). Nao e knob de
        // UI — muda a FORMA da curva, nao a magnitude; fica aqui p/ nao virar literal no shader.
        static constexpr f32 kOriginAngularMinRatio = 0.2f;
    };

} // namespace Smile
