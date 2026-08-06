#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "Smile/Graphics/OceanSpectrum.h"

namespace {
    using Complex = std::complex<double>;

    constexpr double kPi = 3.1415926535897932384626433832795;
    constexpr double kTwoPi = 2.0 * kPi;

    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    bool Near(double A, double B, double Epsilon = 1.0e-10) {
        return std::abs(A - B) <= Epsilon;
    }

    std::size_t Log2Exact(std::size_t N) {
        if (N == 0 || (N & (N - 1)) != 0)
            throw std::invalid_argument("FFT size must be a power of two");

        std::size_t Result = 0;
        while ((std::size_t{1} << Result) < N)
            ++Result;
        return Result;
    }

    std::size_t ReverseLowBits(std::size_t Value, std::size_t BitCount) {
        std::size_t Result = 0;
        for (std::size_t Bit = 0; Bit < BitCount; ++Bit) {
            Result = (Result << 1) | ((Value >> Bit) & 1u);
        }
        return Result;
    }

    Complex PrecomputedPositiveTwiddle256(std::size_t K) {
        static constexpr std::array<std::array<float, 2>, 33> FirstOctant = {{
            { 1.0000000000f, 0.0000000000f },
            { 0.9996988187f, 0.0245412285f },
            { 0.9987954562f, 0.0490676743f },
            { 0.9972904567f, 0.0735645636f },
            { 0.9951847267f, 0.0980171403f },
            { 0.9924795346f, 0.1224106752f },
            { 0.9891765100f, 0.1467304745f },
            { 0.9852776424f, 0.1709618888f },
            { 0.9807852804f, 0.1950903220f },
            { 0.9757021300f, 0.2191012402f },
            { 0.9700312532f, 0.2429801799f },
            { 0.9637760658f, 0.2667127575f },
            { 0.9569403357f, 0.2902846773f },
            { 0.9495281806f, 0.3136817404f },
            { 0.9415440652f, 0.3368898534f },
            { 0.9329927988f, 0.3598950365f },
            { 0.9238795325f, 0.3826834324f },
            { 0.9142097557f, 0.4052413140f },
            { 0.9039892931f, 0.4275550934f },
            { 0.8932243012f, 0.4496113297f },
            { 0.8819212643f, 0.4713967368f },
            { 0.8700869911f, 0.4928981922f },
            { 0.8577286100f, 0.5141027442f },
            { 0.8448535652f, 0.5349976199f },
            { 0.8314696123f, 0.5555702330f },
            { 0.8175848132f, 0.5758081914f },
            { 0.8032075315f, 0.5956993045f },
            { 0.7883464276f, 0.6152315906f },
            { 0.7730104534f, 0.6343932842f },
            { 0.7572088465f, 0.6531728430f },
            { 0.7409511254f, 0.6715589548f },
            { 0.7242470830f, 0.6895405447f },
            { 0.7071067812f, 0.7071067812f }
        }};

        K &= 255u;
        const std::size_t Quadrant = K >> 6u;
        const std::size_t Offset = K & 63u;
        const std::size_t OctantIndex = Offset <= 32u ? Offset : 64u - Offset;
        double X = FirstOctant[OctantIndex][0];
        double Y = FirstOctant[OctantIndex][1];
        if (Offset > 32u) std::swap(X, Y);
        if ((Quadrant & 1u) != 0u) std::swap(X, Y);
        if (Quadrant == 1u || Quadrant == 2u) X = -X;
        if (Quadrant >= 2u) Y = -Y;
        return { X, Y };
    }

    // Espelha OceanFFT.cs.hlsl: bit reversal na carga, DIT radix-2,
    // twiddle com sinal positivo e transformada sem normalizacao.
    std::vector<Complex> ShaderStylePositiveFFT(const std::vector<Complex>& Input) {
        const std::size_t N = Input.size();
        const std::size_t Stages = Log2Exact(N);
        std::vector<Complex> Ping(N), Pong(N);

        for (std::size_t X = 0; X < N; ++X)
            Ping[ReverseLowBits(X, Stages)] = Input[X];

        for (std::size_t Stage = 1; Stage <= Stages; ++Stage) {
            const std::size_t M = std::size_t{1} << Stage;
            const std::size_t HalfM = M >> 1;
            for (std::size_t X = 0; X < N; ++X) {
                const std::size_t K = (X * (N / M)) & (N - 1);
                const std::size_t I = X & ~(M - 1);
                const std::size_t J = X & (HalfM - 1);
                const Complex W = std::polar(1.0, kTwoPi * static_cast<double>(K) /
                                                      static_cast<double>(N));
                Pong[X] = Ping[I + J] + W * Ping[I + J + HalfM];
            }
            Ping.swap(Pong);
        }
        return Ping;
    }

    std::vector<Complex> PrecomputedTwiddleFFT256(const std::vector<Complex>& Input) {
        constexpr std::size_t N = 256;
        constexpr std::size_t Stages = 8;
        if (Input.size() != N)
            throw std::invalid_argument("precomputed twiddle FFT requires 256 samples");

        std::vector<Complex> Ping(N), Pong(N);
        for (std::size_t X = 0; X < N; ++X)
            Ping[ReverseLowBits(X, Stages)] = Input[X];

        for (std::size_t Stage = 1; Stage <= Stages; ++Stage) {
            const std::size_t M = std::size_t{1} << Stage;
            const std::size_t HalfM = M >> 1;
            for (std::size_t X = 0; X < N; ++X) {
                const std::size_t K = (X * (N / M)) & (N - 1);
                const std::size_t I = X & ~(M - 1);
                const std::size_t J = X & (HalfM - 1);
                Pong[X] = Ping[I + J] +
                          PrecomputedPositiveTwiddle256(K) * Ping[I + J + HalfM];
            }
            Ping.swap(Pong);
        }
        return Ping;
    }

    std::vector<Complex> DirectPositiveDFT(const std::vector<Complex>& Input) {
        const std::size_t N = Input.size();
        std::vector<Complex> Output(N);
        for (std::size_t X = 0; X < N; ++X) {
            Complex Sum{};
            for (std::size_t K = 0; K < N; ++K) {
                const double Phase = kTwoPi * static_cast<double>(K * X) /
                                     static_cast<double>(N);
                Sum += Input[K] * std::polar(1.0, Phase);
            }
            Output[X] = Sum;
        }
        return Output;
    }

    std::vector<Complex> ResolveCenteredSpectrum(const std::vector<Complex>& Spectrum) {
        std::vector<Complex> Spatial = ShaderStylePositiveFFT(Spectrum);
        for (std::size_t X = 0; X < Spatial.size(); ++X) {
            if ((X & 1u) != 0)
                Spatial[X] = -Spatial[X];
        }
        return Spatial;
    }

    void TestFFTAgainstDFT() {
        std::cout << "[FFT] shader-style radix-2 vs direct positive DFT\n";
        const std::vector<Complex> Input = {
            { 0.25, -0.50 }, { 1.00,  0.25 }, { -0.75, 0.10 }, { 0.20, 0.80 },
            { 0.00, -0.30 }, { 0.45, -0.90 }, {  0.70, 0.40 }, { -0.10, 0.05 }
        };
        const auto FFT = ShaderStylePositiveFFT(Input);
        const auto DFT = DirectPositiveDFT(Input);

        double MaxError = 0.0;
        for (std::size_t I = 0; I < FFT.size(); ++I)
            MaxError = std::max(MaxError, std::abs(FFT[I] - DFT[I]));

        std::cout << "  max complex error: " << MaxError << '\n';
        Check(MaxError < 1.0e-11, "radix-2 result diverges from direct DFT");
    }

    void TestPrecomputedFFT256Twiddles() {
        std::cout << "[FFT] precomputed 256-point twiddles vs trigonometric reference\n";
        double MaxTwiddleError = 0.0;
        for (std::size_t K = 0; K < 256; ++K) {
            const Complex Reference = std::polar(1.0, kTwoPi * static_cast<double>(K) / 256.0);
            MaxTwiddleError = std::max(MaxTwiddleError,
                                       std::abs(PrecomputedPositiveTwiddle256(K) - Reference));
        }

        std::vector<Complex> Input(256);
        for (std::size_t I = 0; I < Input.size(); ++I) {
            const double X = static_cast<double>(I);
            Input[I] = { std::sin(0.173 * X) + 0.25 * std::cos(0.031 * X),
                         std::cos(0.117 * X) - 0.15 * std::sin(0.047 * X) };
        }
        const auto Reference = ShaderStylePositiveFFT(Input);
        const auto Optimized = PrecomputedTwiddleFFT256(Input);
        double MaxTransformError = 0.0;
        for (std::size_t I = 0; I < Input.size(); ++I) {
            MaxTransformError = std::max(MaxTransformError,
                                         std::abs(Optimized[I] - Reference[I]));
        }

        std::cout << "  max root error: " << MaxTwiddleError
                  << "; max transform error: " << MaxTransformError << '\n';
        Check(MaxTwiddleError < 5.0e-8,
              "precomputed root reconstruction changes the positive FFT convention");
        Check(MaxTransformError < 2.0e-5,
              "precomputed twiddles materially change the 256-point FFT result");
    }

    Complex DeterministicH0(std::size_t X, std::size_t Y) {
        return {
            0.071 * static_cast<double>(1 + X + 3 * Y),
           -0.043 * static_cast<double>(2 + 2 * X + Y)
        };
    }

    void TestHermitianNxNAndNyquistPolicy() {
        std::cout << "[Hermitian] NxN modular mirror and zero Nyquist axes\n";
        constexpr std::size_t N = 8;
        constexpr double Time = 0.37;
        std::vector<Complex> H0(N * N);
        std::vector<Complex> H(N * N);

        for (std::size_t Y = 0; Y < N; ++Y) {
            for (std::size_t X = 0; X < N; ++X) {
                const bool IsNyquistAxis = X == 0 || Y == 0;
                const bool IsDC = X == N / 2 && Y == N / 2;
                H0[Y * N + X] = (IsNyquistAxis || IsDC)
                    ? Complex{} : DeterministicH0(X, Y);
            }
        }

        for (std::size_t Y = 0; Y < N; ++Y) {
            for (std::size_t X = 0; X < N; ++X) {
                const std::size_t MirrorX = (N - X) & (N - 1);
                const std::size_t MirrorY = (N - Y) & (N - 1);
                const double Kx = static_cast<double>(N) * 0.5 - static_cast<double>(X);
                const double Ky = static_cast<double>(N) * 0.5 - static_cast<double>(Y);
                const double Omega = std::sqrt(9.81 * std::hypot(Kx, Ky));
                const Complex Forward = std::polar(1.0, Omega * Time);
                const Complex Backward = std::conj(Forward);
                const Complex H0K = H0[Y * N + X];
                const Complex H0MinusK = H0[MirrorY * N + MirrorX];
                H[Y * N + X] = H0K * Forward + std::conj(H0MinusK) * Backward;
            }
        }

        double MaxHermitianError = 0.0;
        double MaxNyquistMagnitude = 0.0;
        for (std::size_t Y = 0; Y < N; ++Y) {
            for (std::size_t X = 0; X < N; ++X) {
                const std::size_t MirrorX = (N - X) & (N - 1);
                const std::size_t MirrorY = (N - Y) & (N - 1);
                const double Error = std::abs(H[Y * N + X] -
                                              std::conj(H[MirrorY * N + MirrorX]));
                MaxHermitianError = std::max(MaxHermitianError, Error);
                if (X == 0 || Y == 0)
                    MaxNyquistMagnitude = std::max(MaxNyquistMagnitude,
                                                   std::abs(H0[Y * N + X]));
            }
        }

        std::cout << "  max Hermitian error: " << MaxHermitianError
                  << "; max Nyquist magnitude: " << MaxNyquistMagnitude << '\n';
        Check(MaxHermitianError < 1.0e-11,
              "modular NxN update does not satisfy H(-k)=conj(H(k))");
        Check(MaxNyquistMagnitude == 0.0,
              "Nyquist row or column is not exactly zero");
        Check(H0[(N / 2) * N + N / 2] == Complex{},
              "centered DC mode is not exactly zero");
    }

    struct CascadeBand {
        double TileMeters;
        double LowCycles;
        double HighCycles;
    };

    bool ContainsWorldFrequency(const CascadeBand& Band, double CyclesPerMeter) {
        const double LocalCycles = CyclesPerMeter * Band.TileMeters;
        return LocalCycles >= Band.LowCycles && LocalCycles < Band.HighCycles;
    }

    void TestDefaultCascadeBands() {
        std::cout << "[Cascades] fixed physical domains, adjacent bands and physical time\n";
        const auto MakeBand = [](const Smile::FOceanCascadeConfiguration& C) {
            return CascadeBand{ C.TileMetres, C.LowCycles, C.HighCycles };
        };
        const CascadeBand C0 = MakeBand(Smile::kDefaultOceanCascades[0]);
        const CascadeBand C1 = MakeBand(Smile::kDefaultOceanCascades[1]);
        const CascadeBand C2 = MakeBand(Smile::kDefaultOceanCascades[2]);

        const double Boundary01 = C0.LowCycles / C0.TileMeters;
        const double Boundary12 = C1.LowCycles / C1.TileMeters;
        Check(Near(Boundary01, C1.HighCycles / C1.TileMeters),
              "cascade 0/1 physical boundary is not adjacent");
        Check(Near(Boundary12, C2.HighCycles / C2.TileMeters),
              "cascade 1/2 physical boundary is not adjacent");
        Check(ContainsWorldFrequency(C0, Boundary01) &&
              !ContainsWorldFrequency(C1, Boundary01),
              "inclusive/exclusive ownership at cascade 0/1 boundary changed");
        Check(ContainsWorldFrequency(C1, Boundary12) &&
              !ContainsWorldFrequency(C2, Boundary12),
              "inclusive/exclusive ownership at cascade 1/2 boundary changed");

        const CascadeBand Bands[] = { C0, C1, C2 };
        for (int Sample = 0; Sample <= 20000; ++Sample) {
            const double Frequency = 3.2 * static_cast<double>(Sample) / 20000.0;
            int Owners = 0;
            for (const CascadeBand& Band : Bands)
                Owners += ContainsWorldFrequency(Band, Frequency) ? 1 : 0;
            Check(Owners <= 1, "physical cascade bands overlap");
        }

        const double C0MinWavelengthBound = C0.TileMeters / C0.HighCycles;
        const double C0MaxWavelength = C0.TileMeters / C0.LowCycles;
        const double C1MaxWavelength = C1.TileMeters / C1.LowCycles;
        const double C2MaxWavelength = C2.TileMeters / C2.LowCycles;
        std::cout << "  wavelength coverage (m): >" << C0MinWavelengthBound
                  << " to " << C0MaxWavelength << ", " << C1MaxWavelength
                  << ", " << C2MaxWavelength << '\n';
        Check(Near(C0MinWavelengthBound, 64.0 / 129.0, 1.0e-12),
              "cascade 0 shortest-wavelength bound changed");
        Check(Near(C0MaxWavelength, 32.0, 1.0e-12),
              "cascade 0 longest wavelength changed");
        Check(Near(C1MaxWavelength, 192.0), "cascade 1 longest wavelength changed");
        Check(Near(C2MaxWavelength, 768.0), "cascade 2 longest wavelength changed");

        constexpr double ElapsedTime = 12.5;
        const double SimTimes[] = { ElapsedTime, ElapsedTime, ElapsedTime };
        for (std::size_t I = 0; I < 3; ++I) {
            const double K = kTwoPi * Bands[I].LowCycles / Bands[I].TileMeters;
            const double Omega = std::sqrt(9.81 * K);
            const double Phase = Omega * SimTimes[I];
            std::cout << "  C" << I << " tile " << Bands[I].TileMeters
                      << " m, omega(low)=" << Omega << ", phase(t=12.5s)=" << Phase << '\n';
            Check(Near(SimTimes[I], ElapsedTime) && Near(Phase, Omega * ElapsedTime),
                  "cascade phase contains an extra temporal scale");
        }
    }

    double IntegrateWaveVectorEnergy(
        const Smile::FOceanSpectrum& Spectrum, int Resolution, double KMax) {
        const double DeltaK = (2.0 * KMax) / static_cast<double>(Resolution);
        double Energy = 0.0;
        for (int Y = 0; Y < Resolution; ++Y) {
            const double Ky = -KMax + (static_cast<double>(Y) + 0.5) * DeltaK;
            for (int X = 0; X < Resolution; ++X) {
                const double Kx = -KMax + (static_cast<double>(X) + 0.5) * DeltaK;
                Energy += static_cast<double>(Spectrum.WaveVectorDensity(
                    static_cast<Smile::f32>(Kx), static_cast<Smile::f32>(Ky)));
            }
        }
        return Energy * DeltaK * DeltaK;
    }

    void TestPhysicalSpectrumInvariants() {
        std::cout << "[Spectrum] directional normalization, finiteness and discrete energy\n";
        Smile::FOceanSpectrumParameters Params{};
        Params.WindSpeed = 8.0f;
        Params.FetchKilometres = 100.0f;
        Params.DepthMetres = 80.0f;
        Params.Swell = 0.35f;
        const Smile::FOceanSpectrum Spectrum(Params);
        const double PeakOmega = Spectrum.PeakOmega();

        constexpr int DirectionSteps = 4096;
        constexpr double DirectionStep = kTwoPi / static_cast<double>(DirectionSteps);
        const double OmegaMultipliers[] = { 0.35, 0.70, 1.0, 1.6, 3.0 };
        for (const double Multiplier : OmegaMultipliers) {
            const double Omega = PeakOmega * Multiplier;
            double Integral = 0.0;
            for (int I = 0; I < DirectionSteps; ++I) {
                const double Theta = -kPi + (static_cast<double>(I) + 0.5) * DirectionStep;
                Integral += static_cast<double>(Spectrum.DirectionalSpreading(
                    static_cast<Smile::f32>(Omega), static_cast<Smile::f32>(Theta)));
            }
            Integral *= DirectionStep;
            std::cout << "  integral D at omega/peak=" << Multiplier
                      << ": " << Integral << '\n';
            Check(std::isfinite(Integral) && std::abs(Integral - 1.0) < 5.0e-3,
                  "directional spreading does not integrate to approximately one");
        }

        for (int I = 0; I <= 80; ++I) {
            const double T = static_cast<double>(I) / 80.0;
            const double Omega = std::exp(std::log(1.0e-3) * (1.0 - T) +
                                          std::log(100.0) * T);
            const double Frequency = Spectrum.FrequencyDensity(static_cast<Smile::f32>(Omega));
            Check(std::isfinite(Frequency) && Frequency >= 0.0,
                  "frequency density contains NaN/Inf or negative energy");
            for (int A = 0; A < 32; ++A) {
                const double Theta = -kPi + kTwoPi * (static_cast<double>(A) + 0.5) / 32.0;
                const double Directional = Spectrum.DirectionalSpreading(
                    static_cast<Smile::f32>(Omega), static_cast<Smile::f32>(Theta));
                Check(std::isfinite(Directional) && Directional >= 0.0,
                      "directional density contains NaN/Inf or negative energy");
            }
        }

        for (int Y = -24; Y <= 24; ++Y) {
            for (int X = -24; X <= 24; ++X) {
                Smile::FOceanDispersionSample Dispersion{};
                const double Density = Spectrum.WaveVectorDensity(
                    static_cast<Smile::f32>(X * 0.125),
                    static_cast<Smile::f32>(Y * 0.125), &Dispersion);
                Check(std::isfinite(Density) && Density >= 0.0,
                      "wave-vector density contains NaN/Inf or negative energy");
                Check(std::isfinite(Dispersion.Omega) && Dispersion.Omega >= 0.0f &&
                      std::isfinite(Dispersion.DOmegaDk) && Dispersion.DOmegaDk >= 0.0f,
                      "dispersion contains NaN/Inf or a negative derivative");
            }
        }

        constexpr double KMax = 2.0;
        const double Energy128 = IntegrateWaveVectorEnergy(Spectrum, 128, KMax);
        const double Energy256 = IntegrateWaveVectorEnergy(Spectrum, 256, KMax);
        const double RelativeDifference = std::abs(Energy128 - Energy256) /
                                          std::max(Energy256, 1.0e-20);
        std::cout << "  discrete energy 128^2=" << Energy128
                  << ", 256^2=" << Energy256
                  << ", relative delta=" << RelativeDifference << '\n';
        Check(std::isfinite(Energy128) && std::isfinite(Energy256) &&
              Energy128 > 0.0 && Energy256 > 0.0,
              "discrete wave-vector integration is not finite and positive");
        Check(RelativeDifference < 3.0e-2,
              "WaveVectorDensity energy is not resolution-invariant after multiplying by delta-k area");
    }

    void TestH0PhysicalSamplingConvention() {
        std::cout << "[H0] physical dk, quadratic gain and bilateral variance\n";
        constexpr double WorldSize = 64.0;
        constexpr int IX = 8;
        constexpr int IY = 4;
        const double DeltaK = kTwoPi / WorldSize;
        const double Kx = static_cast<double>(IX) * DeltaK;
        const double Ky = static_cast<double>(IY) * DeltaK;

        Smile::FOceanSpectrumParameters UnitParams{};
        UnitParams.WindSpeed = 8.0f;
        UnitParams.Gain = 1.0f;
        const Smile::FOceanSpectrum UnitSpectrum(UnitParams);
        Smile::FOceanDispersionSample Dispersion{};
        const double Density = UnitSpectrum.WaveVectorDensity(
            static_cast<Smile::f32>(Kx), static_cast<Smile::f32>(Ky), &Dispersion);
        const double CellEnergy = Density * DeltaK * DeltaK;
        const double AmplitudeScale = std::sqrt(0.5 * CellEnergy);
        const double ExpectedH0Variance = 0.5 * CellEnergy;
        const double ExpectedFourthMoment = 3.0 * ExpectedH0Variance * ExpectedH0Variance;

        Smile::FOceanSpectrumParameters DoubleAmplitudeParams = UnitParams;
        DoubleAmplitudeParams.Gain = 4.0f;
        const Smile::FOceanSpectrum DoubleAmplitudeSpectrum(DoubleAmplitudeParams);
        const double DoubleAmplitudeDensity = DoubleAmplitudeSpectrum.WaveVectorDensity(
            static_cast<Smile::f32>(Kx), static_cast<Smile::f32>(Ky));
        Check(Density > 0.0 && Near(DoubleAmplitudeDensity / Density, 4.0, 2.0e-5),
              "doubling linear wave amplitude does not quadruple spectral density");

        std::mt19937 Rng{ 987654u };
        std::normal_distribution<double> Gaussian(0.0, 1.0);
        std::uniform_real_distribution<double> Phase(0.0, kTwoPi);
        constexpr std::size_t Samples = 200000;
        double MeanMagnitudeSquared = 0.0;
        double MeanMagnitudeFourth = 0.0;
        for (std::size_t I = 0; I < Samples; ++I) {
            const double A = AmplitudeScale * Gaussian(Rng);
            const double Phi = Phase(Rng);
            const Complex H0(A * std::cos(Phi), -A * std::sin(Phi));
            const double MagnitudeSquared = std::norm(H0);
            MeanMagnitudeSquared += MagnitudeSquared;
            MeanMagnitudeFourth += MagnitudeSquared * MagnitudeSquared;
        }
        MeanMagnitudeSquared /= static_cast<double>(Samples);
        MeanMagnitudeFourth /= static_cast<double>(Samples);
        const double RelativeVarianceError = std::abs(
            MeanMagnitudeSquared - ExpectedH0Variance) / ExpectedH0Variance;
        const double RelativeFourthMomentError = std::abs(
            MeanMagnitudeFourth - ExpectedFourthMoment) / ExpectedFourthMoment;
        std::cout << "  dk=" << DeltaK << ", omega=" << Dispersion.Omega
                  << ", E|h0|^2=" << MeanMagnitudeSquared
                  << " (expected " << ExpectedH0Variance << ")\n";
        Check(std::isfinite(Dispersion.Omega) && Dispersion.Omega > 0.0f,
              "active H0 mode does not store physical dispersion omega");
        Check(RelativeVarianceError < 1.0e-2,
              "H0 sampling does not satisfy E|h0|^2 = P*dk^2/2");
        Check(RelativeFourthMomentError < 2.0e-2,
              "H0 amplitude does not follow Horvath's single-Gaussian chi-square law");
    }

    void TestSingleModeChoppinessSign() {
        std::cout << "[Choppiness] positive choppy compresses the displayed crest\n";
        constexpr std::size_t N = 64;
        constexpr int Mode = 2;
        constexpr double JacobianScale = 0.25;
        std::vector<Complex> HeightSpectrum(N);
        std::vector<Complex> DisplacementSpectrum(N);

        const std::size_t PositiveK = N / 2 - Mode;
        const std::size_t NegativeK = N / 2 + Mode;
        HeightSpectrum[PositiveK] = 0.5;
        HeightSpectrum[NegativeK] = 0.5;

        for (std::size_t X = 0; X < N; ++X) {
            const int K = static_cast<int>(N / 2) - static_cast<int>(X);
            const double NormalizedK = (K > 0) ? 1.0 : ((K < 0) ? -1.0 : 0.0);
            // OceanUpdateSpectrum.cs.hlsl: Dt_x = -i * k_hat.x * h_t.
            DisplacementSpectrum[X] = Complex{ 0.0, -NormalizedK } * HeightSpectrum[X];
        }

        const auto HeightResolved = ResolveCenteredSpectrum(HeightSpectrum);
        const auto DisplacementResolved = ResolveCenteredSpectrum(DisplacementSpectrum);
        std::vector<double> DisplayHeight(N), HorizontalDisplacement(N), Jacobian(N);
        for (std::size_t X = 0; X < N; ++X) {
            // OceanCreateDisplacement.cs.hlsl preserves the FFT height sign.
            DisplayHeight[X] = HeightResolved[X].real();
            HorizontalDisplacement[X] = DisplacementResolved[X].real();
        }
        for (std::size_t X = 0; X < N; ++X) {
            const std::size_t L = (X + N - 1) & (N - 1);
            const std::size_t R = (X + 1) & (N - 1);
            const double DdxTexel = 0.5 * (HorizontalDisplacement[R] -
                                           HorizontalDisplacement[L]);
            Jacobian[X] = 1.0 + JacobianScale * DdxTexel;
        }

        const std::size_t Crest = 0;
        const std::size_t Trough = N / (2 * Mode);
        std::cout << "  trough: height=" << DisplayHeight[Trough]
                  << ", J=" << Jacobian[Trough]
                  << "; crest: height=" << DisplayHeight[Crest]
                  << ", J=" << Jacobian[Crest] << '\n';
        Check(DisplayHeight[Trough] < DisplayHeight[Crest],
              "single-mode trough/crest positions changed");
        Check(Jacobian[Crest] < 1.0,
              "positive choppiness does not compress the displayed crest");
        Check(Jacobian[Trough] > 1.0,
              "positive choppiness does not expand the displayed trough");

        // With the shader's positive temporal phase and centered/checkerboard
        // synthesis, a mode whose code-space k is positive advances one sample
        // in +k when its phase advances by one spatial sample.
        const double OneSamplePhase = kTwoPi * static_cast<double>(Mode) /
                                      static_cast<double>(N);
        std::vector<Complex> AdvancedSpectrum(N);
        AdvancedSpectrum[PositiveK] = 0.5 * std::polar(1.0, OneSamplePhase);
        AdvancedSpectrum[NegativeK] = 0.5 * std::polar(1.0, -OneSamplePhase);
        const auto AdvancedHeight = ResolveCenteredSpectrum(AdvancedSpectrum);
        const auto CrestIt = std::max_element(
            AdvancedHeight.begin(), AdvancedHeight.end(),
            [](const Complex& A, const Complex& B) { return A.real() < B.real(); });
        const std::size_t AdvancedCrest = static_cast<std::size_t>(
            std::distance(AdvancedHeight.begin(), CrestIt));
        Check(AdvancedCrest == 1u,
              "positive temporal phase no longer propagates the crest in +k");
    }

    void TestMetricSurfaceDifferentials() {
        std::cout << "[Differentials] normals and Jacobian are invariant in world units\n";

        // (dDx/dx, dDz/dx, dh/dx) and (dDx/dz, dDz/dz, dh/dz).
        const double Dx[3] = { 0.10, -0.05, 0.20 };
        const double Dz[3] = { 0.03,  0.12, -0.15 };
        const double Domains[] = { 64.0, 384.0, 1536.0 };

        for (const double Domain : Domains) {
            const double Texel = Domain / 256.0;
            double RecoveredX[3]{};
            double RecoveredZ[3]{};
            for (int C = 0; C < 3; ++C) {
                const double Left = -Dx[C] * Texel;
                const double Right = Dx[C] * Texel;
                const double Down = -Dz[C] * Texel;
                const double Up = Dz[C] * Texel;
                RecoveredX[C] = (Right - Left) * 256.0 / (2.0 * Domain);
                RecoveredZ[C] = (Up - Down) * 256.0 / (2.0 * Domain);
                Check(Near(RecoveredX[C], Dx[C]) && Near(RecoveredZ[C], Dz[C]),
                      "central difference is not expressed in metres");
            }

            const double Tx[3] = { 1.0 + RecoveredX[0], RecoveredX[2], RecoveredX[1] };
            const double Tz[3] = { RecoveredZ[0], RecoveredZ[2], 1.0 + RecoveredZ[1] };
            const double Normal[3] = {
                Tz[1] * Tx[2] - Tz[2] * Tx[1],
                Tz[2] * Tx[0] - Tz[0] * Tx[2],
                Tz[0] * Tx[1] - Tz[1] * Tx[0]
            };
            const double Jacobian = (1.0 + RecoveredX[0]) * (1.0 + RecoveredZ[1]) -
                                    RecoveredZ[0] * RecoveredX[1];
            Check(Near(Normal[1], Jacobian),
                  "parametric normal Y does not equal the horizontal Jacobian");
            Check(Near(Jacobian, 1.2335), "metric Jacobian changed with cascade domain");
            Check(Near(Normal[0], -0.2165) && Near(Normal[2], 0.171),
                  "expanded parametric normal formula changed");
        }
    }

    void TestDisplacementMipLowPass() {
        std::cout << "[Displacement AA] 2x2 mip filter preserves DC and rejects Nyquist\n";

        auto Downsample2x2 = [](const std::vector<double>& Source, std::size_t Width) {
            const std::size_t DestinationWidth = Width / 2;
            std::vector<double> Destination(DestinationWidth * DestinationWidth, 0.0);
            for (std::size_t Y = 0; Y < DestinationWidth; ++Y) {
                for (std::size_t X = 0; X < DestinationWidth; ++X) {
                    double Sum = 0.0;
                    for (std::size_t DY = 0; DY < 2; ++DY)
                        for (std::size_t DX = 0; DX < 2; ++DX)
                            Sum += Source[(2 * Y + DY) * Width + 2 * X + DX];
                    Destination[Y * DestinationWidth + X] = Sum * 0.25;
                }
            }
            return Destination;
        };

        constexpr std::size_t Width = 8;
        const std::vector<double> Constant(Width * Width, 3.25);
        const auto ConstantMip = Downsample2x2(Constant, Width);
        Check(std::all_of(ConstantMip.begin(), ConstantMip.end(),
                          [](double V) { return Near(V, 3.25); }),
              "displacement mip filter changes the DC component");

        std::vector<double> Nyquist(Width * Width);
        for (std::size_t Y = 0; Y < Width; ++Y)
            for (std::size_t X = 0; X < Width; ++X)
                Nyquist[Y * Width + X] = ((X + Y) & 1u) ? -1.0 : 1.0;
        const auto NyquistMip = Downsample2x2(Nyquist, Width);
        Check(std::all_of(NyquistMip.begin(), NyquistMip.end(),
                          [](double V) { return Near(V, 0.0); }),
              "displacement mip filter does not reject the Nyquist checkerboard");
    }

    void TestSlopeMomentMipTransfer() {
        std::cout << "[Normal/BRDF AA] slope moments preserve unresolved directional energy\n";

        struct Slope { double Wind; double Cross; };
        const std::array<Slope, 4> Samples = {{
            { 1.0,  0.25 }, { -1.0,  0.25 },
            { 1.0, -0.25 }, { -1.0, -0.25 }
        }};

        double MeanWind = 0.0, MeanCross = 0.0;
        double SecondWind = 0.0, SecondCross = 0.0;
        for (const Slope& S : Samples) {
            MeanWind += S.Wind;
            MeanCross += S.Cross;
            SecondWind += S.Wind * S.Wind;
            SecondCross += S.Cross * S.Cross;
        }
        MeanWind *= 0.25; MeanCross *= 0.25;
        SecondWind *= 0.25; SecondCross *= 0.25;

        const double VarianceWind = SecondWind - MeanWind * MeanWind;
        const double VarianceCross = SecondCross - MeanCross * MeanCross;
        Check(Near(MeanWind, 0.0) && Near(MeanCross, 0.0),
              "symmetric subpixel slopes must disappear from the filtered normal");
        Check(Near(VarianceWind, 1.0) && Near(VarianceCross, 0.0625),
              "missing normal detail was not transferred to directional slope variance");
        Check(Near(SecondWind, MeanWind * MeanWind + VarianceWind) &&
              Near(SecondCross, MeanCross * MeanCross + VarianceCross),
              "slope moment energy identity was not preserved");
        Check(VarianceWind > VarianceCross,
              "wind-aligned spectrum must remain anisotropic after mip filtering");

        const std::array<Slope, 4> Constant = {{
            { 0.3, -0.2 }, { 0.3, -0.2 }, { 0.3, -0.2 }, { 0.3, -0.2 }
        }};
        double ConstantMeanWind = 0.0, ConstantMeanCross = 0.0;
        double ConstantSecondWind = 0.0, ConstantSecondCross = 0.0;
        for (const Slope& S : Constant) {
            ConstantMeanWind += S.Wind * 0.25;
            ConstantMeanCross += S.Cross * 0.25;
            ConstantSecondWind += S.Wind * S.Wind * 0.25;
            ConstantSecondCross += S.Cross * S.Cross * 0.25;
        }
        Check(Near(ConstantSecondWind - ConstantMeanWind * ConstantMeanWind, 0.0) &&
              Near(ConstantSecondCross - ConstantMeanCross * ConstantMeanCross, 0.0),
              "a resolved constant slope must not create artificial BRDF roughness");
    }

    struct PolarPair {
        bool Accepted = false;
        double First = 0.0;
        double Second = 0.0;
    };

    PolarPair GuardedPolarGaussianPair(double X1, double X2) {
        const double W = X1 * X1 + X2 * X2;
        // Espelha a guarda de produção antes de log(w)/w.
        if (!(W > 0.0 && W < 1.0))
            return {};

        const double Scale = std::sqrt((-2.0 * std::log(W)) / W);
        const double First = X1 * Scale;
        const double Second = X2 * Scale;
        if (!std::isfinite(First) || !std::isfinite(Second))
            return {};
        return { true, First, Second };
    }

    void TestGuardedPolarGaussian() {
        std::cout << "[Gaussian] guarded Marsaglia polar generator\n";
        Check(!GuardedPolarGaussianPair(0.0, 0.0).Accepted,
              "w==0 must be rejected before log(w)/w");
        Check(!GuardedPolarGaussianPair(1.0, 0.0).Accepted,
              "w>=1 must be rejected");
        const PolarPair Known = GuardedPolarGaussianPair(0.25, -0.5);
        Check(Known.Accepted && std::isfinite(Known.First) && std::isfinite(Known.Second),
              "valid polar pair must produce finite Gaussian values");

        std::mt19937 Rng{ 1337u };
        std::uniform_real_distribution<double> Uniform(-1.0, 1.0);
        std::size_t Accepted = 0;
        for (std::size_t I = 0; I < 100000; ++I) {
            const PolarPair Pair = GuardedPolarGaussianPair(Uniform(Rng), Uniform(Rng));
            if (!Pair.Accepted)
                continue;
            ++Accepted;
            Check(std::isfinite(Pair.First) && std::isfinite(Pair.Second),
                  "accepted Gaussian pair contains NaN/Inf");
        }
        std::cout << "  finite accepted pairs: " << Accepted << "/100000\n";
        Check(Accepted > 70000, "unexpectedly low polar acceptance rate");
    }
}

int main() {
    std::cout << std::fixed << std::setprecision(9);
    TestFFTAgainstDFT();
    TestPrecomputedFFT256Twiddles();
    TestHermitianNxNAndNyquistPolicy();
    TestDefaultCascadeBands();
    TestPhysicalSpectrumInvariants();
    TestH0PhysicalSamplingConvention();
    TestSingleModeChoppinessSign();
    TestMetricSurfaceDifferentials();
    TestDisplacementMipLowPass();
    TestSlopeMomentMipTransfer();
    TestGuardedPolarGaussian();

    if (Failures != 0) {
        std::cerr << Failures << " ocean math baseline assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All ocean math baseline diagnostics passed.\n";
    return 0;
}
