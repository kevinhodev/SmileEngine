#pragma once

#include <array>
#include <unordered_map>

#include "Smile/Core/Types.h"

namespace Smile {
    struct FOceanCascadeConfiguration {
        u32 Seed       = 0;
        f32 TileMetres = 0.0f;
        f32 LowCycles  = 0.0f;
        f32 HighCycles = 0.0f;
    };

    // Single source of truth shared by renderer setup and mathematical tests.
    inline constexpr std::array<FOceanCascadeConfiguration, 3> kDefaultOceanCascades{{
        { 1337u,   64.0f, 2.0f, 129.0f },
        { 1338u,  384.0f, 2.0f,  12.0f },
        { 1339u, 1536.0f, 2.0f,   8.0f },
    }};

    // Physical sea-state parameters used by the spectral bake. Distances are in metres,
    // fetch is in kilometres at the public API boundary, and angles are in radians.
    struct FOceanSpectrumParameters {
        f32 WindSpeed       = 4.0f;
        f32 WindDirection   = 0.0f;
        f32 FetchKilometres = 100.0f;
        f32 DepthMetres     = 100.0f;
        f32 Swell           = 0.25f;
        f32 Gain            = 1.0f;
        f32 Gravity         = 9.81f;
        f32 SurfaceTension  = 0.074f;
        f32 WaterDensity    = 1000.0f;
    };

    struct FOceanDispersionSample {
        f32 Omega     = 0.0f;
        f32 DOmegaDk  = 0.0f;
    };

    // Horvath 2015: TMA/JONSWAP with frequency-dependent Donelan-Banner spreading.
    // The returned density is expressed in wave-vector space and still needs to be
    // multiplied by the discrete cell area DeltaKx * DeltaKy before drawing a mode.
    class FOceanSpectrum final {
    public:
        explicit FOceanSpectrum(const FOceanSpectrumParameters& Parameters);

        const FOceanSpectrumParameters& Parameters() const { return Params; }
        f32 PeakOmega() const { return OmegaPeak; }

        FOceanDispersionSample Dispersion(f32 K) const;
        f32 FrequencyDensity(f32 Omega) const;
        f32 DirectionalSpreading(f32 Omega, f32 Theta) const;
        f32 WaveVectorDensity(f32 Kx, f32 Ky, FOceanDispersionSample* OutDispersion = nullptr) const;

    private:
        static f32 WrapAngle(f32 Radians);
        f32 UnnormalizedDirectional(f32 Omega, f32 Theta) const;
        f32 DirectionalNormalization(f32 Omega) const;

        FOceanSpectrumParameters Params{};
        f32 FetchMetres = 100000.0f;
        f32 DimensionlessFetch = 1.0f;
        f32 Alpha = 0.0f;
        f32 OmegaPeak = 1.0f;
        // omega depends only on |k|, so a 2D FFT grid revisits the same radii many
        // times. Cache the numerical directional normalization per exact float
        // value to avoid integrating 64 angular samples for every texel.
        mutable std::unordered_map<u32, f32> DirectionalNormalizationCache;
    };
}
