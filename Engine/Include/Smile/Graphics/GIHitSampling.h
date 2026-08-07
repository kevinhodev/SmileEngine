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
        f32 FadeProbes = 0.0f;  // celulas de fade na borda do volume; 0 = desligado
        // Piso de roughness do hit secundario (`minSecondaryRoughness` da RTXDI, 0.5 no sample
        // app dela; Doc/RestirGI.md manda clampar "to higher values"). 0 desliga = comportamento
        // anterior. Motivo no ShadeSurfaceHit: DDGI e ReSTIR GI guardam a radiancia do hit num
        // cache NAO-direcional e a reusam de outras direcoes, entao lobo GGX estreito no hit vira
        // firefly no reuso — e quem paga e o firefly clamp, tirando energia.
        //
        // UNICO campo desta struct que NAO vale para os tres passes: as reflexoes passam 0 na
        // unha (Shaders/Reflections/*Trace.cs.hlsl). O hit delas e sombreado p/ uma visada
        // conhecida e consumido uma vez so — clampar borraria espelho-no-espelho. A regra da
        // struct ("um knob nao pode mexer em metade dos passes") continua valendo para o resto:
        // a excecao esta escrita nos dois lados, aqui e no shader.
        // 0.5 = valor da RTXDI (`minSecondaryRoughness` do sample app dela). Esteve em 0.0
        // durante o run de baseline de 2026-08-07, que inocentou tudo desta sessao; restaurado
        // depois disso. ⚠️ AINDA SEM A/B PROPRIO — e a unica mudanca da sessao com impacto visual
        // garantido, e nunca foi olhada isolada. 0.0 devolve o comportamento anterior.
        f32 SecondaryRoughnessMin = 0.5f;
    };
}
