#include "SmileEditor/TimeOfDayBridge.h"
#include "Smile/Graphics/Renderer.h"

#include <algorithm>
#include <cmath>

namespace SmileEditor {
    namespace {
        constexpr double kToDeg = 180.0 / 3.14159265358979;

        // FTimeOfDay de fallback: mantem o painel funcional (bindings validos) antes do renderer
        // inicializar; os valores sao os defaults da engine e sao descartados no SetRenderer.
        Smile::FTimeOfDay& FallbackTod() {
            static Smile::FTimeOfDay Tod;
            return Tod;
        }

        // Idem p/ o clima (secao Clima do painel).
        Smile::FWeather& FallbackWeather() {
            static Smile::FWeather W;
            return W;
        }
    }

    TimeOfDayBridge::TimeOfDayBridge(QObject* _Parent) : QObject(_Parent) {}

    void TimeOfDayBridge::SetRenderer(Smile::Renderer* _Renderer) {
        Renderer = _Renderer;
        SyncManualFromRenderer();
        emit AvailableChanged();
        emit StateChanged();
        emit TimeChanged();
    }

    // ---- Acesso ao estado ----
    static Smile::FTimeOfDay& TodOf(Smile::Renderer* _R) {
        return _R ? _R->GetTimeOfDay() : FallbackTod();
    }
    static const Smile::FTimeOfDay& TodOfConst(const Smile::Renderer* _R) {
        return _R ? _R->GetTimeOfDay() : FallbackTod();
    }
    static Smile::FWeather& WeatherOf(Smile::Renderer* _R) {
        return _R ? _R->GetWeather() : FallbackWeather();
    }
    static const Smile::FWeather& WeatherOfConst(const Smile::Renderer* _R) {
        return _R ? _R->GetWeather() : FallbackWeather();
    }

    bool   TimeOfDayBridge::Enabled() const        { return TodOfConst(Renderer).Enabled; }
    bool   TimeOfDayBridge::Running() const        { return TodOfConst(Renderer).Running; }
    double TimeOfDayBridge::TimeHours() const      { return TodOfConst(Renderer).TimeHours; }
    double TimeOfDayBridge::DayLengthSec() const   { return TodOfConst(Renderer).DayLengthSec; }
    double TimeOfDayBridge::LatitudeDeg() const    { return TodOfConst(Renderer).LatitudeDeg; }
    int    TimeOfDayBridge::DayOfYear() const      { return TodOfConst(Renderer).DayOfYear; }
    double TimeOfDayBridge::NorthOffsetDeg() const { return TodOfConst(Renderer).NorthOffsetDeg; }
    bool   TimeOfDayBridge::MoonEnabled() const    { return TodOfConst(Renderer).MoonEnabled; }
    double TimeOfDayBridge::MoonPhaseOffsetHours() const { return TodOfConst(Renderer).MoonPhaseOffsetHours; }
    double TimeOfDayBridge::MoonIntensity() const  { return TodOfConst(Renderer).MoonIntensity; }
    double TimeOfDayBridge::MoonDiskSize() const   { return TodOfConst(Renderer).MoonDiskSize; }
    double TimeOfDayBridge::MoonDiskBrightness() const { return TodOfConst(Renderer).MoonDiskBrightness; }
    double TimeOfDayBridge::StarIntensity() const  { return TodOfConst(Renderer).StarIntensity; }
    double TimeOfDayBridge::RainAmount() const     { return WeatherOfConst(Renderer).RainAmount; }
    double TimeOfDayBridge::PuddleAmount() const   { return WeatherOfConst(Renderer).PuddleAmount; }
    double TimeOfDayBridge::PuddleScale() const    { return WeatherOfConst(Renderer).PuddleScale; }
    double TimeOfDayBridge::RippleStrength() const { return WeatherOfConst(Renderer).RippleStrength; }
    double TimeOfDayBridge::WetDarkening() const   { return WeatherOfConst(Renderer).WetDarkening; }
    double TimeOfDayBridge::CurtainAmount() const  { return WeatherOfConst(Renderer).CurtainAmount; }
    bool   TimeOfDayBridge::WeatherDriveSky() const{ return WeatherOfConst(Renderer).DriveSky; }
    bool   TimeOfDayBridge::RainParticles() const  { return WeatherOfConst(Renderer).RainParticles; }

    double TimeOfDayBridge::SunElevationDeg() const {
        // TOD off: le a direcao manual corrente do renderer (a que de fato ilumina a cena).
        if (Renderer && !Enabled()) return std::asin(std::clamp((double)Renderer->GetSunDirection().Y, -1.0, 1.0)) * kToDeg;
        return sunElevationAt(TimeHours());
    }
    double TimeOfDayBridge::SunAzimuthDeg() const {
        if (Renderer && !Enabled()) {
            const auto D = Renderer->GetSunDirection();
            double Az = std::atan2((double)D.X, (double)D.Z) * kToDeg;
            return Az < 0.0 ? Az + 360.0 : Az;
        }
        const auto D = TodOfConst(Renderer).SunDirection();
        double Az = std::atan2((double)D.X, (double)D.Z) * kToDeg;
        return Az < 0.0 ? Az + 360.0 : Az;
    }
    double TimeOfDayBridge::MoonElevationDeg() const {
        const auto D = TodOfConst(Renderer).MoonDirection();
        return std::asin(std::clamp((double)D.Y, -1.0, 1.0)) * kToDeg;
    }
    double TimeOfDayBridge::MoonPhaseFraction() const {
        const auto& Tod = TodOfConst(Renderer);
        const auto S = Tod.SunDirection();
        const auto M = Tod.MoonDirection();
        const double C = (double)(S.X * M.X + S.Y * M.Y + S.Z * M.Z);
        return (1.0 - std::clamp(C, -1.0, 1.0)) * 0.5; // 0 = nova, 1 = cheia
    }

    double TimeOfDayBridge::sunElevationAt(double _Hours) const {
        const auto D = TodOfConst(Renderer).DirAtHour((float)_Hours);
        return std::asin(std::clamp((double)D.Y, -1.0, 1.0)) * kToDeg;
    }
    double TimeOfDayBridge::moonElevationAt(double _Hours) const {
        const auto& Tod = TodOfConst(Renderer);
        const auto D = Tod.DirAtHour((float)(_Hours + Tod.MoonPhaseOffsetHours));
        return std::asin(std::clamp((double)D.Y, -1.0, 1.0)) * kToDeg;
    }

    // ---- Setters ----
    void TimeOfDayBridge::SetEnabled(bool _V) {
        auto& Tod = TodOf(Renderer);
        if (Tod.Enabled == _V) return;
        // Desligando: congela o sol onde esta e re-sincroniza os sliders manuais nele.
        Tod.Enabled = _V;
        if (!_V) SyncManualFromRenderer();
        emit StateChanged();
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetRunning(bool _V) {
        auto& Tod = TodOf(Renderer);
        if (Tod.Running == _V) return;
        Tod.Running = _V;
        emit StateChanged();
    }
    void TimeOfDayBridge::SetTimeHours(double _V) {
        auto& Tod = TodOf(Renderer);
        double Wrapped = std::fmod(_V, 24.0);
        if (Wrapped < 0.0) Wrapped += 24.0;
        if (std::abs(Tod.TimeHours - (float)Wrapped) < 1e-4) return;
        Tod.TimeHours = (float)Wrapped;
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetDayLengthSec(double _V) {
        TodOf(Renderer).DayLengthSec = (float)std::max(_V, 1.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetLatitudeDeg(double _V) {
        TodOf(Renderer).LatitudeDeg = (float)std::clamp(_V, -66.0, 66.0);
        emit StateChanged();
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetDayOfYear(int _V) {
        TodOf(Renderer).DayOfYear = std::clamp(_V, 1, 365);
        emit StateChanged();
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetNorthOffsetDeg(double _V) {
        TodOf(Renderer).NorthOffsetDeg = (float)_V;
        emit StateChanged();
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetMoonEnabled(bool _V) {
        TodOf(Renderer).MoonEnabled = _V;
        emit StateChanged();
    }
    void TimeOfDayBridge::SetMoonPhaseOffsetHours(double _V) {
        TodOf(Renderer).MoonPhaseOffsetHours = (float)std::clamp(_V, 0.0, 24.0);
        emit StateChanged();
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetMoonIntensity(double _V) {
        TodOf(Renderer).MoonIntensity = (float)std::max(_V, 0.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetMoonDiskSize(double _V) {
        TodOf(Renderer).MoonDiskSize = (float)std::clamp(_V, 0.1, 8.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetMoonDiskBrightness(double _V) {
        TodOf(Renderer).MoonDiskBrightness = (float)std::clamp(_V, 0.1, 6.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetStarIntensity(double _V) {
        TodOf(Renderer).StarIntensity = (float)std::max(_V, 0.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetRainAmount(double _V) {
        WeatherOf(Renderer).RainAmount = (float)std::clamp(_V, 0.0, 1.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetPuddleAmount(double _V) {
        WeatherOf(Renderer).PuddleAmount = (float)std::clamp(_V, 0.0, 1.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetPuddleScale(double _V) {
        WeatherOf(Renderer).PuddleScale = (float)std::clamp(_V, 1.0, 64.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetRippleStrength(double _V) {
        WeatherOf(Renderer).RippleStrength = (float)std::clamp(_V, 0.0, 2.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetWetDarkening(double _V) {
        WeatherOf(Renderer).WetDarkening = (float)std::clamp(_V, 0.0, 1.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetCurtainAmount(double _V) {
        WeatherOf(Renderer).CurtainAmount = (float)std::clamp(_V, 0.0, 1.0);
        emit StateChanged();
    }
    void TimeOfDayBridge::SetWeatherDriveSky(bool _V) {
        WeatherOf(Renderer).DriveSky = _V;
        emit StateChanged();
    }
    void TimeOfDayBridge::SetRainParticles(bool _V) {
        WeatherOf(Renderer).RainParticles = _V;
        emit StateChanged();
    }
    void TimeOfDayBridge::SetManualAzimuthDeg(double _V) {
        ManualAz = _V;
        if (Renderer) Renderer->SetSunAzimuthElevation((float)ManualAz, (float)ManualEl);
        emit StateChanged();
        emit TimeChanged();
    }
    void TimeOfDayBridge::SetManualElevationDeg(double _V) {
        ManualEl = std::clamp(_V, -20.0, 90.0);
        if (Renderer) Renderer->SetSunAzimuthElevation((float)ManualAz, (float)ManualEl);
        emit StateChanged();
        emit TimeChanged();
    }

    void TimeOfDayBridge::Refresh() {
        if (!Renderer) return;
        const double Now = TodOfConst(Renderer).TimeHours;
        if (std::abs(Now - LastEmittedHours) > 1e-5) {
            LastEmittedHours = Now;
            emit TimeChanged();
        }
    }

    void TimeOfDayBridge::SyncManualFromRenderer() {
        if (!Renderer) return;
        const auto D = Renderer->GetSunDirection();
        ManualEl = std::asin(std::clamp((double)D.Y, -1.0, 1.0)) * kToDeg;
        ManualAz = std::atan2((double)D.X, (double)D.Z) * kToDeg;
        if (ManualAz < 0.0) ManualAz += 360.0;
    }
}
