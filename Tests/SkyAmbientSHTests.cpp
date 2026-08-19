#include <array>
#include <cmath>
#include <cstdio>

namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kCoefficientCount = 9;

    struct Vec3 {
        double X;
        double Y;
        double Z;
    };

    using SH9 = std::array<double, kCoefficientCount>;

    SH9 Basis(const Vec3& n) {
        return {
            0.282095,
            0.488603 * n.Y,
            0.488603 * n.Z,
            0.488603 * n.X,
            1.092548 * n.X * n.Y,
            1.092548 * n.Y * n.Z,
            0.315392 * (3.0 * n.Z * n.Z - 1.0),
            1.092548 * n.X * n.Z,
            0.546274 * (n.X * n.X - n.Y * n.Y),
        };
    }

    template <typename F>
    SH9 Project(F&& radiance) {
        constexpr int kElevationSamples = 192;
        constexpr int kAzimuthSamples = 384;
        constexpr double dTheta = kPi / kElevationSamples;
        constexpr double dPhi = 2.0 * kPi / kAzimuthSamples;

        SH9 coefficients{};
        for (int i = 0; i < kElevationSamples; ++i) {
            const double theta = (static_cast<double>(i) + 0.5) * dTheta;
            const double ct = std::cos(theta);
            const double st = std::sin(theta);
            for (int j = 0; j < kAzimuthSamples; ++j) {
                const double phi = (static_cast<double>(j) + 0.5) * dPhi;
                const Vec3 n{ st * std::cos(phi), ct, st * std::sin(phi) };
                const SH9 basis = Basis(n);
                const double weight = st * dTheta * dPhi;
                const double value = radiance(n);
                for (int c = 0; c < kCoefficientCount; ++c)
                    coefficients[c] += value * basis[c] * weight;
            }
        }
        return coefficients;
    }

    double EvaluateIrradianceOverPi(const SH9& coefficients, const Vec3& n, int bands) {
        const SH9 basis = Basis(n);
        double result = coefficients[0] * basis[0];
        if (bands >= 2) {
            for (int c = 1; c <= 3; ++c)
                result += (2.0 / 3.0) * coefficients[c] * basis[c];
        }
        if (bands >= 3) {
            for (int c = 4; c < kCoefficientCount; ++c)
                result += 0.25 * coefficients[c] * basis[c];
        }
        return result;
    }

    bool Near(double actual, double expected, double tolerance, const char* label) {
        if (std::abs(actual - expected) <= tolerance) return true;
        std::fprintf(stderr, "%s: esperado %.8f, obtido %.8f\n", label, expected, actual);
        return false;
    }
}

int main() {
    bool ok = true;

    // Um ceu uniforme deve continuar uniforme depois da projecao e da convolucao Lambertiana.
    const SH9 uniform = Project([](const Vec3&) { return 1.0; });
    ok &= Near(EvaluateIrradianceOverPi(uniform, { 1.0, 0.0, 0.0 }, 3),
               1.0, 2.0e-4, "ceu uniforme / X");
    ok &= Near(EvaluateIrradianceOverPi(uniform, { 0.0, 1.0, 0.0 }, 3),
               1.0, 2.0e-4, "ceu uniforme / Y");
    ok &= Near(EvaluateIrradianceOverPi(uniform, { 0.0, 0.0, 1.0 }, 3),
               1.0, 2.0e-4, "ceu uniforme / Z");
    for (int c = 1; c < kCoefficientCount; ++c)
        ok &= Near(uniform[c], 0.0, 2.0e-4, "coeficiente AC do ceu uniforme");

    // Este campo tem apenas DC + banda l=2. L1 necessariamente perde a diferenca entre zenite
    // e horizonte; L2 deve reconstruir sua irradiancia analitica:
    //   L(w) = 1 + 0.5 P2(w.y)
    //   E(n)/PI = 1 + (1/4) 0.5 P2(n.y)
    const SH9 quadraticSky = Project([](const Vec3& w) {
        const double p2 = 0.5 * (3.0 * w.Y * w.Y - 1.0);
        return 1.0 + 0.5 * p2;
    });

    const Vec3 zenith{ 0.0, 1.0, 0.0 };
    const Vec3 horizon{ 1.0, 0.0, 0.0 };
    const double expectedZenith = 1.125;
    const double expectedHorizon = 0.9375;
    const double l1Zenith = EvaluateIrradianceOverPi(quadraticSky, zenith, 2);
    const double l1Horizon = EvaluateIrradianceOverPi(quadraticSky, horizon, 2);
    const double l2Zenith = EvaluateIrradianceOverPi(quadraticSky, zenith, 3);
    const double l2Horizon = EvaluateIrradianceOverPi(quadraticSky, horizon, 3);

    ok &= std::abs(l1Zenith - expectedZenith) > 0.10;
    ok &= std::abs(l1Horizon - expectedHorizon) > 0.05;
    ok &= Near(l2Zenith, expectedZenith, 3.0e-4, "SH-L2 / zenite");
    ok &= Near(l2Horizon, expectedHorizon, 3.0e-4, "SH-L2 / horizonte");

    if (!ok) {
        std::fputs("Falha na validacao numerica da SH-L2 do ambiente.\n", stderr);
        return 1;
    }

    std::puts("Sky ambient SH-L2: projecao e convolucao validadas.");
    return 0;
}
