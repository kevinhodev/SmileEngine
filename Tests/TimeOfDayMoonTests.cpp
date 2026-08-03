#include <algorithm>
#include <cmath>
#include <iostream>
#include <string_view>

#include "Smile/Graphics/TimeOfDay.h"

namespace {
    constexpr double kPi = 3.14159265358979323846;
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    double Length(const Smile::Vec3& V) {
        return std::sqrt(static_cast<double>(V.X) * V.X +
                         static_cast<double>(V.Y) * V.Y +
                         static_cast<double>(V.Z) * V.Z);
    }

    double Dot(const Smile::Vec3& A, const Smile::Vec3& B) {
        return static_cast<double>(A.X) * B.X +
               static_cast<double>(A.Y) * B.Y +
               static_cast<double>(A.Z) * B.Z;
    }

    double Distance(const Smile::Vec3& A, const Smile::Vec3& B) {
        const double X = static_cast<double>(A.X) - B.X;
        const double Y = static_cast<double>(A.Y) - B.Y;
        const double Z = static_cast<double>(A.Z) - B.Z;
        return std::sqrt(X * X + Y * Y + Z * Z);
    }

    void TestCanonicalPhasesAcrossCalendar() {
        std::cout << "[Moon] canonical phases across latitude, season and time\n";
        const int Days[] = { 1, 80, 172, 266, 355 };
        const float Latitudes[] = { -60.0f, -23.5f, 0.0f, 40.0f, 60.0f };
        const float Hours[] = { 0.0f, 5.5f, 12.0f, 18.25f, 23.9f };
        const float NorthOffsets[] = { 0.0f, 37.0f, 181.0f };
        const float PhaseHours[] = { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f,
                                     15.0f, 18.0f, 21.0f, 24.0f };

        Smile::FTimeOfDay Tod;
        double MaxAngularDotError = 0.0;
        double MaxUnitError = 0.0;

        for (int Day : Days) {
            Tod.DayOfYear = Day;
            for (float Latitude : Latitudes) {
                Tod.LatitudeDeg = Latitude;
                for (float North : NorthOffsets) {
                    Tod.NorthOffsetDeg = North;
                    for (float Hour : Hours) {
                        Tod.TimeHours = Hour;
                        const Smile::Vec3 Sun = Tod.SunDirection();
                        for (float PhaseHour : PhaseHours) {
                            Tod.MoonPhaseOffsetHours = PhaseHour;
                            const Smile::Vec3 Moon = Tod.MoonDirection();
                            const double Expected = std::cos(PhaseHour * 15.0 * kPi / 180.0);
                            MaxAngularDotError = std::max(
                                MaxAngularDotError, std::abs(Dot(Sun, Moon) - Expected));
                            MaxUnitError = std::max(MaxUnitError, std::abs(Length(Moon) - 1.0));
                        }

                        Tod.MoonPhaseOffsetHours = 0.0f;
                        Check(Distance(Tod.MoonDirection(), Sun) < 2.0e-5,
                              "new moon is not coincident with the sun");
                        Tod.MoonPhaseOffsetHours = 12.0f;
                        Check(Distance(Tod.MoonDirection(), -Sun) < 2.0e-5,
                              "full moon is not antipodal to the sun");
                    }
                }
            }
        }

        std::cout << "  max phase dot error=" << MaxAngularDotError
                  << "; max unit error=" << MaxUnitError << '\n';
        Check(MaxAngularDotError < 2.0e-5,
              "moon phase angle changes with latitude, season or time");
        Check(MaxUnitError < 2.0e-6, "moon direction is not normalized");
    }

    void TestTimelineAndDiskScaleSemantics() {
        std::cout << "[Moon] timeline sampling and physical disk multiplier\n";
        Smile::FTimeOfDay Tod;
        Tod.TimeHours = 3.75f;
        Tod.MoonPhaseOffsetHours = 8.0f;
        Check(Distance(Tod.MoonDirection(), Tod.MoonDirectionAtHour(Tod.TimeHours)) < 1.0e-7,
              "timeline moon direction disagrees with the current-time direction");
        Check(std::abs(Tod.MoonDiskSize - 3.0f) < 1.0e-6f,
              "default moon disk size is not 3x");

        Tod.MoonDiskSize = 1.0f;
        Check(std::abs(Tod.MoonDiskAngularDiameterDeg() - 0.518f) < 1.0e-6f,
              "1x moon size is not the mean physical angular diameter");
        Tod.MoonDiskSize = 1.5f;
        Check(std::abs(Tod.MoonDiskAngularDiameterDeg() - 0.777f) < 1.0e-6f,
              "1.5x moon size is not a multiplier");
        Tod.MoonDiskSize = -2.0f;
        Check(Tod.MoonDiskAngularDiameterDeg() == 0.0f,
              "negative moon size must clamp to zero diameter");
    }
}

int main() {
    TestCanonicalPhasesAcrossCalendar();
    TestTimelineAndDiskScaleSemantics();

    if (Failures != 0) {
        std::cerr << Failures << " time-of-day moon assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All time-of-day moon diagnostics passed.\n";
    return 0;
}
