#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    // Estado de CLIMA (F1 do sistema de chuva). Header-only como o FTimeOfDay: o Renderer e o
    // dono, o editor le/escreve direto (painel TOD, secao Clima). RainAmount e o knob mestre —
    // 0 = seco, 1 = temporal — e dirige o passe de wetness deferred (RainWetness); os demais
    // sao proporcoes/estetica por cima dele.
    //
    // Fases futuras (mesmo plano do CSM/nuvens): F2 occlusion map top-down (interior seco),
    // F3 cortina de gotas (cones estilo Cry), F4 splashes + mist + acoplamento nuvens/ceu,
    // F5 particulas GPU near-field como opcao de qualidade sobre o MESMO estado — a decisao
    // "chove aqui?" (occlusion) e compartilhada por qualquer visual de gota.
    struct FWeather {
        f32 RainAmount     = 0.0f;  // knob mestre [0,1]: intensidade da chuva/molhado
        f32 PuddleAmount   = 0.65f; // [0,1] quanto do chao up-facing empoca com RainAmount=1
        f32 PuddleScale    = 8.0f;  // tamanho caracteristico das pocas (m) — escala do noise XZ
        f32 RippleStrength = 1.0f;  // forca dos aneis de gota nas pocas (0 = espelho parado)
        f32 WetDarkening   = 0.85f; // [0,1] escurecimento do albedo poroso molhado (Cry
                                    // fDiffuseDarkening: porosidade alta escurece ate ~0.2x)

        // F2: mapa de oclusao top-down — so molha o que ve o ceu (interior/marquise secos).
        bool RainOcclusion = true;

        // F3: cortina de gotas (streaks na frente da camera). Escala a densidade/opacidade
        // dos streaks por cima do RainAmount; 0 = so wetness, sem gota visivel no ar.
        f32 CurtainAmount = 1.0f;

        // F4: chuva dirige o ceu — cobertura de nuvem sobe pra um piso nublado, key light
        // (sol/lua) e ambient escurecem, height fog engrossa (mist). Os knobs do usuario
        // (pagina de nuvens/fog) nao sao alterados: o override e por-frame.
        bool DriveSky = true;

        // F4 RUNTIME (Renderer escreve por frame — nao e knob): molhado ACUMULADO do chao.
        // Sobe em ~5 s de chuva e seca em ~30 s depois que para; wetness/pocas usam isto,
        // cortina e aneis de gota usam o RainAmount instantaneo.
        f32 Wetness = 0.0f;

        bool Raining() const { return RainAmount > 0.001f; }
        // Ainda ha trabalho pro passe de wetness (chovendo agora OU chao secando).
        bool Active()  const { return RainAmount > 0.001f || Wetness > 0.005f; }
    };
}
