#pragma once

#include "Smile/Core/Types.h"

namespace Smile {
    // Estado de CLIMA (F1 do sistema de chuva). Header-only como o FTimeOfDay; o Renderer e o
    // dono. RainAmount e o knob mestre — 0 = seco, 1 = temporal — e dirige o passe de wetness
    // deferred (RainWetness); os demais sao proporcoes/estetica por cima dele.
    //
    // Os campos eram PUBLICOS e o editor escrevia por atribuicao direta. Nao havia onde pendurar
    // invalidacao, e ela faz falta: RainAmount e DriveSky mudam a radiancia do ceu e a cor da
    // luz-chave vistas pelos passes de trace (DDGI, ReSTIR GI, reflexoes), ou seja, entram no
    // atlas do DDGI, que tem hysteresis 0,99 — o valor velho sobrevive por centenas de updates.
    // Ver Docs/KNOBS-AUDIT.md. Agora a escrita passa pelo FRenderSettings, que e o unico lugar
    // onde a politica de invalidacao mora.
    //
    // Fases futuras (mesmo plano do CSM/nuvens): F2 occlusion map top-down (interior seco),
    // F3 cortina de gotas (cones estilo Cry), F4 splashes + mist + acoplamento nuvens/ceu,
    // F5 particulas GPU near-field como opcao de qualidade sobre o MESMO estado — a decisao
    // "chove aqui?" (occlusion) e compartilhada por qualquer visual de gota.
    class FWeather {
    public:
        f32  GetRainAmount() const     { return RainAmount; }
        void SetRainAmount(f32 V)      { RainAmount = V; }
        f32  GetPuddleAmount() const   { return PuddleAmount; }
        void SetPuddleAmount(f32 V)    { PuddleAmount = V; }
        f32  GetPuddleScale() const    { return PuddleScale; }
        void SetPuddleScale(f32 V)     { PuddleScale = V; }
        f32  GetRippleStrength() const { return RippleStrength; }
        void SetRippleStrength(f32 V)  { RippleStrength = V; }
        f32  GetWetDarkening() const   { return WetDarkening; }
        void SetWetDarkening(f32 V)    { WetDarkening = V; }
        bool GetRainOcclusion() const  { return RainOcclusion; }
        void SetRainOcclusion(bool V)  { RainOcclusion = V; }
        f32  GetCurtainAmount() const  { return CurtainAmount; }
        void SetCurtainAmount(f32 V)   { CurtainAmount = V; }
        bool GetRainParticles() const  { return RainParticles; }
        void SetRainParticles(bool V)  { RainParticles = V; }
        bool GetDriveSky() const       { return DriveSky; }
        void SetDriveSky(bool V)       { DriveSky = V; }

        // Runtime, escrito pelo Renderer por frame — nao e knob, nao passa pela fachada.
        f32  GetWetness() const  { return Wetness; }
        void SetWetness(f32 V)   { Wetness = V; }

        bool Raining() const { return RainAmount > 0.001f; }
        // Ainda ha trabalho pro passe de wetness (chovendo agora OU chao secando).
        bool Active()  const { return RainAmount > 0.001f || Wetness > 0.005f; }

    private:
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

        // F5: gotas por particula no near-field (opcao de qualidade). ON = quads procedurais
        // com colisao pelo occlusion map substituem os 2 cilindros proximos da cortina (que
        // segue cobrindo o medio/longe). OFF = so a cortina (barato, estilo Cry pura).
        bool RainParticles = true;

        // F4: chuva dirige o ceu — cobertura de nuvem sobe pra um piso nublado, key light
        // (sol/lua) e ambient escurecem, height fog engrossa (mist). Os knobs do usuario
        // (pagina de nuvens/fog) nao sao alterados: o override e por-frame.
        bool DriveSky = true;

        // F4 RUNTIME (Renderer escreve por frame — nao e knob): molhado ACUMULADO do chao.
        // Sobe em ~5 s de chuva e seca em ~30 s depois que para; wetness/pocas usam isto,
        // cortina e aneis de gota usam o RainAmount instantaneo.
        f32 Wetness = 0.0f;
    };
}
