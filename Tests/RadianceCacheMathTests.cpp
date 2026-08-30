#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
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
}

int main() {
    TestMetadataLayout();
    TestSelfReferenceDiagonal();
    TestTemporalDensityInvariance();
    TestLodBoundaryContinuity();
    TestFixedWindowSurvivesRecycledHole();
    TestNormalBias();

    if (Failures == 0) {
        std::cout << "Radiance cache math tests passed\n";
        return 0;
    }
    std::cerr << Failures << " radiance cache test(s) failed\n";
    return 1;
}
