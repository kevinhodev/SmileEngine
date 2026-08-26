#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Vec3.h"
#include <algorithm>
#include <cmath>

namespace Smile {
    // Relogio solar simplificado que orienta sol, lua e estrelas em hora solar local.
    class FTimeOfDay {
    public:
        bool Enabled      = false;   // false preserva o controle manual do sol
        bool Running      = true;
        f32  TimeHours    = 10.0f;   // [0, 24)
        f32  DayLengthSec = 120.0f;  // segundos reais por ciclo

        f32 LatitudeDeg    = 40.0f;  // norte positivo
        i32 DayOfYear      = 172;    // [1, 365]
        f32 NorthOffsetDeg = 0.0f;

        bool MoonEnabled = true;
        // 0h = lua nova, 6/18h = quartos e 12h = lua cheia.
        f32  MoonPhaseOffsetHours = 12.0f;
        f32  MoonIntensity      = 0.25f;
        f32  MoonDiskSize       = 3.0f;  // multiplica o diametro angular medio
        // Valores altos saturam o disco no tonemap e apagam o contraste da textura.
        f32  MoonDiskBrightness = 1.6f;
        f32  StarIntensity      = 1.0f;

        // Razao entre o dia solar medio e o sideral.
        static constexpr f64 kSiderealRate = 1.00273790935;
        static constexpr i32 kSiderealReferenceDay = 80;

        // Avanca o relogio e o calendario, inclusive para passos maiores que um dia.
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

        // Hora sideral local aproximada em [0, 24); TimeHours ja e hora solar local.
        f32 LocalSiderealTimeHours() const {
            const f64 SolarDaysFromReference =
                static_cast<f64>(DayOfYear - kSiderealReferenceDay) +
                (static_cast<f64>(TimeHours) - 12.0) / 24.0;
            f64 Hours = std::fmod(SolarDaysFromReference * kSiderealRate * 24.0, 24.0);
            if (Hours < 0.0) Hours += 24.0;
            return static_cast<f32>(Hours);
        }

        // RA=0 cruza o meridiano em -90 graus na matriz catalogo->mundo.
        f32 StarRotationAngleRad() const {
            constexpr f64 kPi64 = 3.14159265358979323846;
            const f64 Angle = -(static_cast<f64>(LocalSiderealTimeHours()) + 6.0) *
                              15.0 * kPi64 / 180.0;
            return static_cast<f32>(std::remainder(Angle, 2.0 * kPi64));
        }

        // Direcao para o sol em mundo (+Y para cima).
        Vec3 SunDirection() const { return DirAtHour(TimeHours); }

        static constexpr f32 kMeanMoonAngularDiameterDeg = 0.518f;

        // Orbita lunar artistica em grande circulo, com fase independente da declinacao solar.
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

        // Azimute parte de +Z (norte) para +X (leste), na convencao do Renderer.
        Vec3 DirAtHour(f32 _Hours) const {
            const f32 d2r = 3.14159265358979f / 180.0f;

            const f32 decl = (23.45f * std::sin(d2r * 360.0f * (284.0f + (f32)DayOfYear) / 365.0f)) * d2r;
            const f32 lat  = LatitudeDeg * d2r;

            const f32 H = (_Hours - 12.0f) * 15.0f * d2r;

            const f32 sinEl = std::clamp(std::sin(lat) * std::sin(decl)
                                       + std::cos(lat) * std::cos(decl) * std::cos(H), -1.0f, 1.0f);
            const f32 el    = std::asin(sinEl);
            const f32 cosEl = std::cos(el);

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
