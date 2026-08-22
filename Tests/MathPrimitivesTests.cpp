#include "Smile/Math/Math.h"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {
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

    bool Near(const Smile::Vec2& A, const Smile::Vec2& B, float Epsilon = 1e-5f) {
        return Near(A.X, B.X, Epsilon) && Near(A.Y, B.Y, Epsilon);
    }

    bool Near(const Smile::Vec3& A, const Smile::Vec3& B, float Epsilon = 1e-5f) {
        return Near(A.X, B.X, Epsilon) && Near(A.Y, B.Y, Epsilon)
            && Near(A.Z, B.Z, Epsilon);
    }

    bool NearRotation(const Smile::Mat44& A, const Smile::Mat44& B,
                      float Epsilon = 2e-5f) {
        for (int Row = 0; Row < 3; ++Row)
            for (int Column = 0; Column < 3; ++Column)
                if (!Near(A.M[Row][Column], B.M[Row][Column], Epsilon)) return false;
        return true;
    }

    void TestClosestPointOnSegment() {
        float Parameter = -1.0f;
        const Smile::Vec2 Middle = Smile::ClosestPointOnSegment(
            Smile::Vec2{ 3.0f, 4.0f }, Smile::Vec2{ 1.0f, 1.0f },
            Smile::Vec2{ 5.0f, 1.0f }, &Parameter);
        Check(Near(Middle, Smile::Vec2{ 3.0f, 1.0f }), "segment projection");
        Check(Near(Parameter, 0.5f), "segment parameter");

        const Smile::Vec2 Clamped = Smile::ClosestPointOnSegment(
            Smile::Vec2{ -2.0f, 4.0f }, Smile::Vec2{ 1.0f, 1.0f },
            Smile::Vec2{ 5.0f, 1.0f });
        Check(Near(Clamped, Smile::Vec2{ 1.0f, 1.0f }), "segment endpoint clamp");

        const Smile::Vec2 Degenerate = Smile::ClosestPointOnSegment(
            Smile::Vec2{ 8.0f, 9.0f }, Smile::Vec2{ 2.0f, 3.0f },
            Smile::Vec2{ 2.0f, 3.0f });
        Check(Near(Degenerate, Smile::Vec2{ 2.0f, 3.0f }), "degenerate segment");
    }

    void TestRowVectorRotation() {
        const Smile::Mat44 Rotation =
            Smile::Mat44::RotationAxis(Smile::Vec3::UnitZ(), Smile::HalfPi);
        const Smile::Vec3 Rotated = Rotation.TransformVectorRow(Smile::Vec3::UnitX());
        Check(Near(Rotated, Smile::Vec3::UnitY()), "axis-angle row-vector convention");

        const Smile::Vec3 Source{ 2.0f, -3.0f, 4.0f };
        const Smile::Vec3 Restored =
            Rotation.TransformVectorRowTransposed(Rotation.TransformVectorRow(Source));
        Check(Near(Restored, Source), "transposed rotation inverse");

        const Smile::Mat44 Identity =
            Smile::Mat44::RotationAxis(Smile::Vec3::Zero(), 1.0f);
        Check(NearRotation(Identity, Smile::Mat44::Identity()), "zero rotation axis");
    }

    void TestEulerRoundTrip() {
        const Smile::Vec3 Samples[] = {
            { 0.2f, -0.4f, 0.7f },
            { -1.1f, 0.6f, 2.2f },
            { 0.4f, Smile::HalfPi, -0.8f }
        };
        for (const Smile::Vec3& Euler : Samples) {
            const Smile::Mat44 Rotation = Smile::Mat44::RotationEulerXYZ(Euler);
            const Smile::Mat44 Rebuilt = Smile::Mat44::RotationEulerXYZ(Rotation.ToEulerXYZ());
            Check(NearRotation(Rotation, Rebuilt), "Euler XYZ round-trip");
        }
    }

    void TestSnapToStep() {
        Check(Near(Smile::SnapToStep(0.14f, 0.1f), 0.1f), "positive snap");
        Check(Near(Smile::SnapToStep(-0.16f, 0.1f), -0.2f), "negative snap");
        Check(Near(Smile::SnapToStep(0.14f, 0.0f), 0.14f), "disabled snap");
    }
}

int main() {
    TestClosestPointOnSegment();
    TestRowVectorRotation();
    TestEulerRoundTrip();
    TestSnapToStep();

    if (Failures == 0) {
        std::cout << "Math primitive tests passed\n";
        return 0;
    }
    std::cerr << Failures << " math primitive test(s) failed\n";
    return 1;
}
