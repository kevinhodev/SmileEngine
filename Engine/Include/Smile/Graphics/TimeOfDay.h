#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Vec3.h"
#include <cmath>
#include <algorithm>

namespace Smile {
    // Relogio de Time-of-Day (Fase 1). Dirige a DIRECAO do sol a partir de um relogio de 24h
    // + latitude geografica + dia-do-ano, usando um modelo padrao de posicao solar (declinacao
    // sazonal + angulo horario). Alimenta a atmosfera fisica que ja existe (Hillaire +
    // transmitancia), entao a cor do sol, o ceu e o ambiente seguem AUTOMATICAMENTE — sem curvas
    // de cor pintadas a mao (ao contrario do PARAM_SUN_COLOR do TOD da CryEngine). Inverter a
    // arquitetura assim (relogio dirige fisica, em vez de curvas substituirem fisica) e o ganho:
    // menos autoracao e resultado fisicamente coerente.
    //
    // Fases futuras: F2 lua/estrelas/exposicao noturna; F3 curvas dos knobs nao-fisicos +
    // presets; F4 vento ligado ao relogio.
    class FTimeOfDay {
    public:
        // --- Relogio ---
        bool Enabled     = false;    // off: SunDir e controlado manualmente (comportamento atual)
        bool Running     = true;     // pausa o avanco do tempo (relogio congela, sol parado)
        f32  TimeHours   = 10.0f;    // hora atual no dia, [0,24)
        f32  DayLengthSec = 120.0f;  // segundos reais p/ um ciclo completo de 24h (2min/dia default)

        // --- Geografia / estacao ---
        f32  LatitudeDeg    = 40.0f; // + = hemisferio norte; controla a altura do arco do sol
        i32  DayOfYear      = 172;   // 1..365 (172 ~ solsticio de verao no N); declinacao sazonal
        f32  NorthOffsetDeg = 0.0f;  // gira o "norte" da cena (alinha o nascer/por com a geometria)

        // --- Noite / lua (F2) ---
        bool MoonEnabled        = true;  // lua + luar (2a direcional) + estrelas
        // Deslocamento da lua em horas-equivalentes (governa a FASE): 0 = lua nova (junto do sol,
        // some de noite); 12 = lua cheia (oposta ao sol, alta a meia-noite). A lua segue o mesmo
        // arco do sol deslocado por esse offset — fase emerge do angulo lua-sol.
        f32  MoonPhaseOffsetHours = 12.0f;
        f32  MoonIntensity      = 0.25f; // forca do luar (2a direcional); "artistico", nao fisico
        f32  MoonDiskSize       = 1.5f;  // tamanho angular relativo do disco da lua no ceu
        // Luminancia do disco. Baixa de proposito: >~2 cai no ombro do tonemap e esmaga o
        // contraste dos mares da textura pra branco (a lua real de dia e ~2x o ceu, nao 10x).
        f32  MoonDiskBrightness = 1.6f;
        f32  StarIntensity      = 1.0f;  // brilho das estrelas procedurais

        // Avanca o relogio pelo dt real (so quando Enabled && Running). Faz wrap em 24h.
        void Tick(f32 _DeltaSeconds) {
            if (!Enabled || !Running || DayLengthSec <= 1e-3f) return;
            TimeHours += (_DeltaSeconds / DayLengthSec) * 24.0f;
            TimeHours = std::fmod(TimeHours, 24.0f);
            if (TimeHours < 0.0f) TimeHours += 24.0f;
        }

        // Direcao PARA o sol (mundo, +Y up). Modelo de posicao solar padrao no horario corrente.
        // Abaixo do horizonte (sinEl<0) e noite: a transmitancia atmosferica zera a luz direta.
        Vec3 SunDirection() const { return DirAtHour(TimeHours); }

        // Direcao PARA a lua: o mesmo arco celeste deslocado por MoonPhaseOffsetHours. Em offset
        // 12 a lua fica oposta ao sol (cheia, alta a meia-noite); em 0 acompanha o sol (nova).
        Vec3 MoonDirection() const { return DirAtHour(TimeHours + MoonPhaseOffsetHours); }

        // Direcao celeste (mundo, +Y up) na convencao do Renderer::SetSunAzimuthElevation:
        // dir = (cosEl*sinAz, sinEl, cosEl*cosAz), Az do +Z (norte) p/ +X (leste). Compartilhado
        // por sol e lua (a lua e o mesmo modelo com a hora deslocada). Publico: o painel TOD do
        // editor amostra a curva do dia inteiro p/ desenhar o arco do ceu.
        Vec3 DirAtHour(f32 _Hours) const {
            const f32 d2r = 3.14159265358979f / 180.0f;

            // Declinacao solar pela epoca do ano (~ -23.45 a +23.45 graus).
            const f32 decl = (23.45f * std::sin(d2r * 360.0f * (284.0f + (f32)DayOfYear) / 365.0f)) * d2r;
            const f32 lat  = LatitudeDeg * d2r;

            // Angulo horario: 0 ao meio-dia solar, 15 graus por hora (negativo de manha).
            const f32 H = (_Hours - 12.0f) * 15.0f * d2r;

            // Elevacao (altura acima do horizonte).
            const f32 sinEl = std::clamp(std::sin(lat) * std::sin(decl)
                                       + std::cos(lat) * std::cos(decl) * std::cos(H), -1.0f, 1.0f);
            const f32 el    = std::asin(sinEl);
            const f32 cosEl = std::cos(el);

            // Azimute a partir do norte (sentido leste), + offset da cena.
            f32 az;
            if (cosEl > 1e-4f) {
                const f32 sinAz = -std::cos(decl) * std::sin(H) / cosEl;
                const f32 cosAz = (std::sin(decl) - std::sin(lat) * sinEl) / (std::cos(lat) * cosEl);
                az = std::atan2(std::clamp(sinAz, -1.0f, 1.0f), std::clamp(cosAz, -1.0f, 1.0f));
            } else {
                az = 0.0f; // astro no zenite: azimute indefinido
            }
            az += NorthOffsetDeg * d2r;

            return Vec3{ cosEl * std::sin(az), sinEl, cosEl * std::cos(az) };
        }
    };
}
