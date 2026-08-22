#include "Smile/Graphics/Water/OceanSpectrum.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Smile {
    namespace {
        constexpr f32 kPi    = 3.14159265358979323846f;
        constexpr f32 kTau   = 6.28318530717958647692f;
        constexpr f32 kEps   = 1.0e-6f;

        f32 SechSquared(f32 X) {
            // cosh overflows for large arguments even though sech^2 tends to zero.
            if (std::abs(X) >= 20.0f) return 0.0f;
            const f32 C = std::cosh(X);
            return 1.0f / (C * C);
        }
    }

    FOceanSpectrum::FOceanSpectrum(const FOceanSpectrumParameters& _Parameters)
        : Params(_Parameters) {
        auto FiniteOr = [](f32 Value, f32 Fallback) {
            return std::isfinite(Value) ? Value : Fallback;
        };
        Params.WindSpeed       = std::max(FiniteOr(Params.WindSpeed, 4.0f), 0.1f);
        Params.WindDirection   = WrapAngle(FiniteOr(Params.WindDirection, 0.0f));
        Params.FetchKilometres = std::max(FiniteOr(Params.FetchKilometres, 100.0f), 0.001f);
        Params.DepthMetres     = std::max(FiniteOr(Params.DepthMetres, 100.0f), 0.01f);
        // Smile intentionally exposes Horvath's perceptually useful elongation range only.
        // Negative values in EncinoWaves interpolate toward an isotropic sea instead.
        Params.Swell           = std::clamp(FiniteOr(Params.Swell, 0.25f), 0.0f, 1.0f);
        Params.Gain            = std::max(FiniteOr(Params.Gain, 1.0f), 0.0f);
        Params.Gravity         = std::max(FiniteOr(Params.Gravity, 9.81f), 0.01f);
        Params.WaterDensity    = std::max(FiniteOr(Params.WaterDensity, 1000.0f), 0.01f);
        Params.SurfaceTension  = std::max(FiniteOr(Params.SurfaceTension, 0.074f), 0.0f);

        FetchMetres = Params.FetchKilometres * 1000.0f;
        DimensionlessFetch = std::max(
            Params.Gravity * FetchMetres / (Params.WindSpeed * Params.WindSpeed), kEps);
        Alpha = 0.076f * std::pow(DimensionlessFetch, -0.22f);
        OmegaPeak = kTau * 3.5f * (Params.Gravity / Params.WindSpeed) *
                    std::pow(DimensionlessFetch, -1.0f / 3.0f);
        OmegaPeak = std::max(OmegaPeak, kEps);
        DirectionalNormalizationCache.reserve(8192);
    }

    f32 FOceanSpectrum::WrapAngle(f32 _Radians) {
        f32 A = std::fmod(_Radians + kPi, kTau);
        if (A < 0.0f) A += kTau;
        return A - kPi;
    }

    FOceanDispersionSample FOceanSpectrum::Dispersion(f32 _K) const {
        FOceanDispersionSample Result{};
        if (!std::isfinite(_K)) return Result;
        const f64 K = std::max(static_cast<f64>(_K), 0.0);
        if (K <= kEps) return Result;

        const f64 Capillary = static_cast<f64>(Params.SurfaceTension) / Params.WaterDensity;
        const f64 K2 = K * K;
        const f64 K3 = K2 * K;
        const f64 Kh = K * Params.DepthMetres;
        const f64 TanhKh = std::tanh(Kh);
        const f64 Base = static_cast<f64>(Params.Gravity) * K + Capillary * K3;
        const f64 OmegaSquared = std::max(Base * TanhKh, 0.0);
        const f64 Omega = std::sqrt(OmegaSquared);
        if (!std::isfinite(Omega) || Omega <= kEps) return Result;

        const f64 DBaseDk = Params.Gravity + 3.0 * Capillary * K2;
        const f64 CoshKh = Kh >= 20.0 ? 0.0 : std::cosh(Kh);
        const f64 Sech2Kh = Kh >= 20.0 ? 0.0 : 1.0 / (CoshKh * CoshKh);
        const f64 DOmegaSquaredDk = DBaseDk * TanhKh + Base * Params.DepthMetres * Sech2Kh;
        const f64 DOmegaDk = std::max(DOmegaSquaredDk / (2.0 * Omega), 0.0);
        if (!std::isfinite(DOmegaDk) ||
            Omega > std::numeric_limits<f32>::max() ||
            DOmegaDk > std::numeric_limits<f32>::max()) return Result;
        Result.Omega = static_cast<f32>(Omega);
        Result.DOmegaDk = static_cast<f32>(DOmegaDk);
        return Result;
    }

    f32 FOceanSpectrum::FrequencyDensity(f32 _Omega) const {
        const f32 Omega = std::max(_Omega, kEps);
        const f32 Ratio = OmegaPeak / Omega;
        const f32 Sigma = Omega <= OmegaPeak ? 0.07f : 0.09f;
        const f32 PeakDistance = (Omega - OmegaPeak) / (Sigma * OmegaPeak);
        const f32 R = std::exp(-0.5f * PeakDistance * PeakDistance);
        const f32 PeakSharpening = std::pow(3.3f, R);
        const f32 Jonswap = Alpha * Params.Gravity * Params.Gravity /
                            std::pow(Omega, 5.0f) *
                            std::exp(-1.25f * std::pow(Ratio, 4.0f)) * PeakSharpening;

        // Canonical Thompson-Vincent piecewise approximation of the Kitaigorodskii
        // attenuation. EncinoWaves deliberately replaces this curve with a tanh fit.
        const f32 OmegaH = Omega * std::sqrt(Params.DepthMetres / Params.Gravity);
        f32 DepthAttenuation = 1.0f;
        if (OmegaH <= 1.0f) {
            DepthAttenuation = 0.5f * OmegaH * OmegaH;
        } else if (OmegaH < 2.0f) {
            const f32 D = 2.0f - OmegaH;
            DepthAttenuation = 1.0f - 0.5f * D * D;
        }
        const f32 Result = Jonswap * DepthAttenuation * Params.Gain;
        return std::isfinite(Result) ? std::max(Result, 0.0f) : 0.0f;
    }

    f32 FOceanSpectrum::UnnormalizedDirectional(f32 _Omega, f32 _Theta) const {
        const f32 OmegaRatio = std::max(_Omega / OmegaPeak, kEps);
        f32 Beta = 0.0f;
        if (OmegaRatio < 0.95f) {
            Beta = 2.61f * std::pow(OmegaRatio, 1.3f);
        } else if (OmegaRatio < 1.6f) {
            Beta = 2.28f * std::pow(OmegaRatio, -1.3f);
        } else {
            const f32 Epsilon = -0.4f + 0.8393f *
                std::exp(-0.567f * std::log(OmegaRatio * OmegaRatio));
            Beta = std::pow(10.0f, Epsilon);
        }

        const f32 Theta = WrapAngle(_Theta);
        const f32 Base = SechSquared(Beta * Theta);
        // Horvath's paper specifies 16.0; EncinoWaves later uses 16.1.
        const f32 SwellShape = 16.0f * std::tanh(OmegaPeak / std::max(_Omega, kEps)) *
                               Params.Swell * Params.Swell;
        const f32 Swell = std::pow(std::abs(std::cos(0.5f * Theta)), 2.0f * SwellShape);
        const f32 Result = Base * Swell;
        return std::isfinite(Result) ? std::max(Result, 0.0f) : 0.0f;
    }

    f32 FOceanSpectrum::DirectionalNormalization(f32 _Omega) const {
        const u32 Key = std::bit_cast<u32>(_Omega);
        if (const auto It = DirectionalNormalizationCache.find(Key);
            It != DirectionalNormalizationCache.end()) {
            return It->second;
        }

        // Midpoint integration is deterministic, smooth in omega, and follows the paper's
        // full [-pi, pi] normalization rather than EncinoWaves' narrower implementation range.
        constexpr int kIntegrationSteps = 64;
        constexpr f32 Step = kTau / static_cast<f32>(kIntegrationSteps);
        f32 Integral = 0.0f;
        for (int I = 0; I < kIntegrationSteps; ++I) {
            const f32 Theta = -kPi + (static_cast<f32>(I) + 0.5f) * Step;
            Integral += UnnormalizedDirectional(_Omega, Theta);
        }
        const f32 Normalization = std::max(Integral * Step, kEps);
        DirectionalNormalizationCache.emplace(Key, Normalization);
        return Normalization;
    }

    f32 FOceanSpectrum::DirectionalSpreading(f32 _Omega, f32 _Theta) const {
        return UnnormalizedDirectional(_Omega, _Theta) / DirectionalNormalization(_Omega);
    }

    f32 FOceanSpectrum::WaveVectorDensity(
        f32 _Kx, f32 _Ky, FOceanDispersionSample* _OutDispersion) const {
        const f32 K = std::sqrt(_Kx * _Kx + _Ky * _Ky);
        const FOceanDispersionSample D = Dispersion(K);
        if (_OutDispersion) *_OutDispersion = D;
        if (K <= kEps || D.Omega <= kEps || D.DOmegaDk <= 0.0f) return 0.0f;

        const f32 Theta = std::atan2(_Ky, _Kx) - Params.WindDirection;
        const f32 Frequency = FrequencyDensity(D.Omega);
        const f32 Directional = DirectionalSpreading(D.Omega, Theta);
        return std::max(Frequency * Directional * D.DOmegaDk / K, 0.0f);
    }
}
