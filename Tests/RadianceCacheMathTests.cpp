#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
    using u32 = std::uint32_t;

    constexpr u32   SampleMask    = 0xFFFFu;
    constexpr u32   FrameMask     = 0xFFu;
    constexpr u32   AccumShift    = 16u;
    constexpr u32   StaleShift    = 24u;
    constexpr u32   MaxFrames     = 64u;
    constexpr float Sqrt3         = 1.7320508075688772f;
    constexpr float NormalBias    = 1e-3f;
    constexpr float LodBlendWidth = 0.125f;
    constexpr float RadianceScale = 5.0e3f;
    constexpr float RadianceMax   = 6.5e3f;

    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "FAIL: " << Message << '\n';
        }
    }

    bool Near(float A, float B, float Epsilon = 1e-5f) {
        return std::fabs(A - B) <= Epsilon;
    }

    struct Vec3 {
        float X = 0.0f, Y = 0.0f, Z = 0.0f;
    };

    Vec3 operator+(Vec3 A, Vec3 B) { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
    Vec3 operator*(Vec3 A, float S) { return { A.X * S, A.Y * S, A.Z * S }; }
    float Dot(Vec3 A, Vec3 B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
    float Length(Vec3 V) { return std::sqrt(Dot(V, V)); }
    Vec3 Normalize(Vec3 V) {
        const float L = Length(V);
        return L > 1e-6f ? V * (1.0f / L) : Vec3{ 0.0f, 0.0f, 1.0f };
    }
    float RadianceY(Vec3 C) { return 0.25f * (C.X + 2.0f * C.Y + C.Z); }

    struct DirectionalRadiance {
        Vec3 L1;
        float L0 = 0.0f;
        float Co = 0.0f;
        float Cg = 0.0f;
    };

    DirectionalRadiance Encode(Vec3 RGB, Vec3 Direction, float Weight) {
        const float Y = RadianceY(RGB);
        return { Normalize(Direction) * (Y * std::clamp(Weight, 0.0f, 1.0f)), Y,
                 RGB.X - RGB.Z, RGB.Y - 0.5f * (RGB.X + RGB.Z) };
    }

    DirectionalRadiance Add(DirectionalRadiance A, DirectionalRadiance B) {
        return { A.L1 + B.L1, A.L0 + B.L0, A.Co + B.Co, A.Cg + B.Cg };
    }

    Vec3 Decode(DirectionalRadiance D, Vec3 Direction) {
        const float DirectionalY = std::min(Length(D.L1), std::max(D.L0, 0.0f));
        const float DiffuseY = std::max(D.L0 - DirectionalY, 0.0f);
        const float Y = DiffuseY + std::max(Dot(D.L1, Normalize(Direction)), 0.0f);
        const float ChromaScale = D.L0 > 1e-6f ? Y / D.L0 : 0.0f;
        const float Co = D.Co * ChromaScale;
        const float Cg = D.Cg * ChromaScale;
        return { std::max(Y + 0.5f * (Co - Cg), 0.0f),
                 std::max(Y + 0.5f * Cg, 0.0f),
                 std::max(Y - 0.5f * (Co + Cg), 0.0f) };
    }

    bool Near(Vec3 A, Vec3 B, float Epsilon = 1e-4f) {
        return Near(A.X, B.X, Epsilon) && Near(A.Y, B.Y, Epsilon) &&
               Near(A.Z, B.Z, Epsilon);
    }

    u32 PackMetadata(u32 Samples, u32 AccumulatedFrames, u32 StaleFrames) {
        return std::min(Samples, SampleMask)
             | (std::min(AccumulatedFrames, FrameMask) << AccumShift)
             | (std::min(StaleFrames, FrameMask) << StaleShift);
    }

    struct TemporalState {
        float Radiance = 0.0f;
        u32 Samples = 0;
        u32 AccumulatedFrames = 0;
        u32 StaleFrames = 0;
    };

    TemporalState Resolve(TemporalState Previous, float CurrentMean, u32 CurrentSamples) {
        TemporalState Result = Previous;
        Result.AccumulatedFrames = std::min(Previous.AccumulatedFrames + 1u, FrameMask);
        if (CurrentSamples == 0u) {
            Result.StaleFrames = std::min(Previous.StaleFrames + 1u, FrameMask);
            return Result;
        }

        float HistorySamples = static_cast<float>(Previous.Samples);
        if (Result.AccumulatedFrames > MaxFrames) {
            HistorySamples *= static_cast<float>(MaxFrames)
                            / static_cast<float>(Result.AccumulatedFrames);
            Result.AccumulatedFrames = MaxFrames;
        }

        const float Total = HistorySamples + static_cast<float>(CurrentSamples);
        Result.Radiance = (Previous.Radiance * HistorySamples
                         + CurrentMean * static_cast<float>(CurrentSamples)) / Total;
        Result.Samples = std::min(static_cast<u32>(Total + 0.5f), SampleMask);
        Result.StaleFrames = 0u;
        return Result;
    }

    struct LodBlend {
        u32 Primary = 0;
        u32 Adjacent = 0;
        float PrimaryWeight = 1.0f;
        bool Active = false;
    };

    float SmoothStep(float A, float B, float X) {
        const float T = std::clamp((X - A) / (B - A), 0.0f, 1.0f);
        return T * T * (3.0f - 2.0f * T);
    }

    LodBlend BlendAt(float ContinuousLevel) {
        LodBlend R{};
        R.Primary = static_cast<u32>(std::floor(ContinuousLevel));
        const float Fraction = ContinuousLevel - std::floor(ContinuousLevel);
        R.Adjacent = R.Primary;
        if (R.Primary > 0u && Fraction < LodBlendWidth) {
            R.Adjacent = R.Primary - 1u;
            R.PrimaryWeight = 0.5f + 0.5f * SmoothStep(0.0f, LodBlendWidth, Fraction);
            R.Active = true;
        } else if (Fraction > 1.0f - LodBlendWidth) {
            R.Adjacent = R.Primary + 1u;
            R.PrimaryWeight = 1.0f
                            - 0.5f * SmoothStep(1.0f - LodBlendWidth, 1.0f, Fraction);
            R.Active = true;
        }
        return R;
    }

    float WeightOfLevel(const LodBlend& Blend, u32 Level) {
        if (Level == Blend.Primary) return Blend.PrimaryWeight;
        if (Blend.Active && Level == Blend.Adjacent) return 1.0f - Blend.PrimaryWeight;
        return 0.0f;
    }

    bool FindFixedWindow(const std::array<u32, 8>& Entries, u32 Checksum) {
        for (u32 Stored : Entries)
            if (Stored == Checksum) return true;
        return false;
    }

    void TestMetadataLayout() {
        const u32 Packed = PackMetadata(4096u, 64u, 17u);
        Check((Packed & SampleMask) == 4096u, "metadata preserves 64 samples x 64 frames");
        Check(((Packed >> AccumShift) & FrameMask) == 64u, "metadata preserves accumulated frames");
        Check(((Packed >> StaleShift) & FrameMask) == 17u, "metadata preserves stale age");
    }

    void TestSelfReferenceDiagonal() {
        const auto Eligible = [](float Segment, float Cell) { return Segment > Cell * Sqrt3; };
        Check(!Eligible(1.5f, 1.0f), "same-voxel diagonal range is rejected");
        Check(!Eligible(Sqrt3, 1.0f), "diagonal boundary is rejected");
        Check(Eligible(Sqrt3 + 1e-4f, 1.0f), "segment beyond voxel diagonal is eligible");
    }

    void TestTemporalDensityInvariance() {
        TemporalState Sparse{};
        TemporalState Dense{};
        for (u32 Frame = 0; Frame < MaxFrames; ++Frame) {
            Sparse = Resolve(Sparse, 1.0f, 1u);
            Dense  = Resolve(Dense, 1.0f, 64u);
        }

        Sparse = Resolve(Sparse, 0.0f, 1u);
        Dense  = Resolve(Dense, 0.0f, 64u);
        Check(Near(Sparse.Radiance, Dense.Radiance, 2e-4f),
              "temporal response is independent of steady path density");
        Check(Sparse.Radiance > 0.98f && Sparse.Radiance < 0.99f,
              "64-frame history has the expected one-frame response");
    }

    void TestLodBoundaryContinuity() {
        const LodBlend Below = BlendAt(0.999f);
        const LodBlend Above = BlendAt(1.001f);
        Check(Below.Active && Above.Active, "adjacent LOD blend is active around boundary");
        Check(Near(WeightOfLevel(Below, 0u) + WeightOfLevel(Below, 1u), 1.0f),
              "LOD weights below boundary sum to one");
        Check(Near(WeightOfLevel(Above, 0u) + WeightOfLevel(Above, 1u), 1.0f),
              "LOD weights above boundary sum to one");
        Check(std::fabs(WeightOfLevel(Below, 1u) - WeightOfLevel(Above, 1u)) < 0.02f,
              "LOD weights remain continuous while the integer level changes");
        Check(!BlendAt(0.5f).Active, "second lookup is skipped away from LOD boundaries");
    }

    void TestFixedWindowSurvivesRecycledHole() {
        std::array<u32, 8> Entries{};
        Entries[1] = 0x12345678u;
        Entries[0] = 0u; // slot anterior foi reciclado por eviccao
        Check(FindFixedWindow(Entries, 0x12345678u),
              "invalid slot does not hide a later collision in the fixed window");
    }

    void TestNormalBias() {
        const auto PositiveBucket = [](float Component) {
            return Component + NormalBias >= 0.0f;
        };
        Check(PositiveBucket(-5e-4f) == PositiveBucket(5e-4f),
              "normal noise inside bias stays in one octant");
        Check(!PositiveBucket(-2e-3f), "normal outside bias keeps the opposite octant");
    }

    void TestDirectionalEncoding() {
        const Vec3 Color{ 4.0f, 2.0f, 1.0f };
        const Vec3 Direction{ 1.0f, 0.0f, 0.0f };
        Check(Near(Decode(Encode(Color, Direction, 0.0f), Vec3{ -1.0f, 0.0f, 0.0f }), Color),
              "diffuse L0 radiance is direction independent");
        Check(Near(Decode(Encode(Color, Direction, 1.0f), Direction), Color),
              "directional reconstruction preserves radiance in the sampled direction");
        const Vec3 Opposite = Decode(Encode(Color, Direction, 1.0f),
                                     Vec3{ -1.0f, 0.0f, 0.0f });
        Check(RadianceY(Opposite) < 1e-5f,
              "fully directional radiance does not leak into the opposite direction");
    }

    void TestDirectionalContributionMixing() {
        const Vec3 Direction{ 0.0f, 1.0f, 0.0f };
        const Vec3 Local{ 2.0f, 1.0f, 0.5f };
        const Vec3 Bounced{ 0.5f, 2.0f, 1.0f };
        constexpr float LocalWeight = 0.2f;
        constexpr float BouncedWeight = 0.8f;
        const float TotalY = RadianceY(Local) + RadianceY(Bounced);
        const float MixedWeight = (RadianceY(Local) * LocalWeight
                                 + RadianceY(Bounced) * BouncedWeight) / TotalY;
        const DirectionalRadiance Separate = Add(Encode(Local, Direction, LocalWeight),
                                                  Encode(Bounced, Direction, BouncedWeight));
        const DirectionalRadiance Combined = Encode(Local + Bounced, Direction, MixedWeight);
        Check(Near(Separate.L1, Combined.L1) && Near(Separate.L0, Combined.L0) &&
              Near(Separate.Co, Combined.Co) && Near(Separate.Cg, Combined.Cg),
              "luminance-weighted directionality equals separate contribution encoding");
    }

    void TestDirectionalAccumulatorRangeAndMemory() {
        const double Worst = 64.0 * static_cast<double>(RadianceMax)
                           * static_cast<double>(RadianceScale);
        Check(Worst < static_cast<double>(std::numeric_limits<std::int32_t>::max()),
              "signed SH accumulator cannot overflow at the per-frame sample cap");
        constexpr std::uint64_t Capacity = 1ull << 17u;
        constexpr std::uint64_t BytesPerCell = 4u + 32u + 16u;
        Check(Capacity * BytesPerCell == 6815744ull,
              "directional cache layout is 6.5 MiB at default capacity");
    }
}

int main() {
    TestMetadataLayout();
    TestSelfReferenceDiagonal();
    TestTemporalDensityInvariance();
    TestLodBoundaryContinuity();
    TestFixedWindowSurvivesRecycledHole();
    TestNormalBias();
    TestDirectionalEncoding();
    TestDirectionalContributionMixing();
    TestDirectionalAccumulatorRangeAndMemory();

    if (Failures == 0) {
        std::cout << "Radiance cache math tests passed\n";
        return 0;
    }
    std::cerr << Failures << " radiance cache test(s) failed\n";
    return 1;
}
