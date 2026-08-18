#include "SmileEditor/Scene/WeatherBridge.h"

#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace SmileEditor {
    WeatherBridge::WeatherBridge(QObject* _Parent) : QObject(_Parent) {}

    void WeatherBridge::SetRenderer(RendererHandle _Renderer) {
        Renderer = std::move(_Renderer);
        emit AvailableChanged();
        emit SettingsChanged();
    }

    bool WeatherBridge::IsRaining() const {
        return RainAmount() > 0.001;
    }

    double WeatherBridge::RainAmount() const {
        return Renderer ? Renderer->Settings().GetRainAmount() : 0.0;
    }

    double WeatherBridge::CurtainAmount() const {
        return Renderer ? Renderer->Settings().GetRainCurtainAmount() : 1.0;
    }

    double WeatherBridge::PuddleAmount() const {
        return Renderer ? Renderer->Settings().GetRainPuddleAmount() : 0.65;
    }

    double WeatherBridge::PuddleScale() const {
        return Renderer ? Renderer->Settings().GetRainPuddleScale() : 8.0;
    }

    double WeatherBridge::RippleStrength() const {
        return Renderer ? Renderer->Settings().GetRainRippleStrength() : 1.0;
    }

    double WeatherBridge::WetDarkening() const {
        return Renderer ? Renderer->Settings().GetRainWetDarkening() : 0.85;
    }

    bool WeatherBridge::IsRainOcclusionEnabled() const {
        return Renderer ? Renderer->Settings().GetRainOcclusion() : true;
    }

    bool WeatherBridge::DrivesSky() const {
        return Renderer ? Renderer->Settings().GetRainDriveSky() : true;
    }

    void WeatherBridge::SetRainAmount(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetRainAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void WeatherBridge::SetCurtainAmount(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetRainCurtainAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void WeatherBridge::SetPuddleAmount(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetRainPuddleAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void WeatherBridge::SetPuddleScale(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetRainPuddleScale(
            static_cast<Smile::f32>(std::clamp(_Value, 1.0, 64.0)));
        emit SettingsChanged();
    }

    void WeatherBridge::SetRippleStrength(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetRainRippleStrength(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 2.0)));
        emit SettingsChanged();
    }

    void WeatherBridge::SetWetDarkening(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetRainWetDarkening(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void WeatherBridge::SetRainOcclusionEnabled(bool _Enabled) {
        if (!Renderer || IsRainOcclusionEnabled() == _Enabled) return;
        Renderer->Settings().SetRainOcclusion(_Enabled);
        emit SettingsChanged();
    }

    void WeatherBridge::SetDrivesSky(bool _Enabled) {
        if (!Renderer || DrivesSky() == _Enabled) return;
        Renderer->Settings().SetRainDriveSky(_Enabled);
        emit SettingsChanged();
    }
}
