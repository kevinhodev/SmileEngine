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
        // some de noite); 12 = lua cheia (exatamente oposta ao sol, alta a meia-noite). O offset
        // gira a direcao solar num grande circulo: nova/quartos/cheia ficam exatos mesmo fora do
        // equinocio, sem herdar a declinacao solar do lado errado do ceu.
        f32  MoonPhaseOffsetHours = 12.0f;
        f32  MoonIntensity      = 0.25f; // forca do luar (2a direcional); "artistico", nao fisico
        f32  MoonDiskSize       = 3.0f;  // multiplicador do diametro angular medio real (0.518 grau)
        // Luminancia do disco. Baixa de proposito: >~2 cai no ombro do tonemap e esmaga o
        // contraste dos mares da textura pra branco (a lua real de dia e ~2x o ceu, nao 10x).
        f32  MoonDiskBrightness = 1.6f;
        f32  StarIntensity      = 1.0f;  // brilho do catalogo (ou fallback procedural)

        // Um dia sideral medio dura ~23h56m solares. A referencia (dia 80, 12h) aproxima o
        // equinocio de marco com o ponto vernal no meridiano local.
        static constexpr f64 kSiderealRate = 1.00273790935;
        static constexpr i32 kSiderealReferenceDay = 80;

        // Avanca o relogio pelo dt real (so quando Enabled && Running). Ao cruzar meia-noite,
        // tambem avanca o calendario: isso mantem sol, estacao e tempo sideral continuos.
        void Tick(f32 _DeltaSeconds) {
            if (!Enabled || !Running || DayLengthSec <= 1e-3f) return;
            const f64 DeltaHours = (static_cast<f64>(_DeltaSeconds) /
                                    static_cast<f64>(DayLengthSec)) * 24.0;
            const f64 UnwrappedHours = static_cast<f64>(TimeHours) + DeltaHours;
            const i64 DayDelta = static_cast<i64>(std::floor(UnwrappedHours / 24.0));

            TimeHours = static_cast<f32>(UnwrappedHours -
                                         static_cast<f64>(DayDelta) * 24.0);

            const i64 ZeroBasedDay = static_cast<i64>(DayOfYear - 1) + DayDelta;
            const i64 WrappedDay = ((ZeroBasedDay % 365) + 365) % 365;
            DayOfYear = static_cast<i32>(WrappedDay + 1);
        }

        // Hora sideral local em [0,24). Como TimeHours representa hora SOLAR local, longitude
        // nao entra: NorthOffsetDeg continua sendo apenas a orientacao do norte no mundo.
        f32 LocalSiderealTimeHours() const {
            const f64 SolarDaysFromReference =
                static_cast<f64>(DayOfYear - kSiderealReferenceDay) +
                (static_cast<f64>(TimeHours) - 12.0) / 24.0;
            f64 Hours = std::fmod(SolarDaysFromReference * kSiderealRate * 24.0, 24.0);
            if (Hours < 0.0) Hours += 24.0;
            return static_cast<f32>(Hours);
        }

        // Convencao da matriz catalogo->mundo: RA=0 esta no meridiano quando Angle=-90 graus;
        // portanto a hora sideral vira -(LST+6h). remainder mantem o seno/cosseno numericamente
        // bem condicionado sem mudar a orientacao.
        f32 StarRotationAngleRad() const {
            constexpr f64 kPi64 = 3.14159265358979323846;
            const f64 Angle = -(static_cast<f64>(LocalSiderealTimeHours()) + 6.0) *
                              15.0 * kPi64 / 180.0;
            return static_cast<f32>(std::remainder(Angle, 2.0 * kPi64));
        }

        // Direcao PARA o sol (mundo, +Y up). Modelo de posicao solar padrao no horario corrente.
        // Abaixo do horizonte (sinEl<0) e noite: a transmitancia atmosferica zera a luz direta.
        Vec3 SunDirection() const { return DirAtHour(TimeHours); }

        static constexpr f32 kMeanMoonAngularDiameterDeg = 0.518f;

        // Direcao PARA a lua numa orbita simplificada de grande circulo. A tangente vem do arco
        // solar seis horas adiante, mas e ortogonalizada contra o sol atual. Portanto o produto
        // dot(sol,lua) e cos(fase): 0h = nova, 6/18h = quartos e 12h = cheia exata para qualquer
        // latitude, estacao e hora.
        Vec3 MoonDirection() const { return MoonDirectionAtHour(TimeHours); }

        Vec3 MoonDirectionAtHour(f32 _Hours) const {
            constexpr f32 DegToRad = 3.14159265358979f / 180.0f;
            const Vec3 Sun = DirAtHour(_Hours);
            const Vec3 QuarterArc = DirAtHour(_Hours + 6.0f);
            const Vec3 Tangent = (QuarterArc - Sun * Sun.Dot(QuarterArc))
                .NormalizedSafe(Sun.GetOrthogonal());
            const f32 Phase = MoonPhaseOffsetHours * 15.0f * DegToRad;
            return (Sun * std::cos(Phase) + Tangent * std::sin(Phase)).NormalizedSafe(Sun);
        }

        f32 MoonDiskAngularDiameterDeg() const {
            return kMeanMoonAngularDiameterDeg * std::max(MoonDiskSize, 0.0f);
        }

        // Direcao celeste (mundo, +Y up) na convencao do Renderer::SetSunAzimuthElevation:
        // dir = (cosEl*sinAz, sinEl, cosEl*cosAz), Az do +Z (norte) p/ +X (leste). Compartilhado
        // pelos modelos de sol e lua. Publico: o painel TOD amostra a curva do dia inteiro p/
        // desenhar os arcos do ceu.
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
