#include "SmileEditor/Scene/OceanBridge.h"

#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace SmileEditor {
    namespace {
        constexpr double kRadiansToDegrees = 57.2957795131;
        constexpr double kDegreesToRadians = 0.01745329252;
    }

    OceanBridge::OceanBridge(QObject* _Parent) : QObject(_Parent) {}

    void OceanBridge::SetRenderer(RendererHandle _Renderer) {
        Renderer = std::move(_Renderer);
        emit AvailableChanged();
        emit SettingsChanged();
    }

    bool OceanBridge::IsEnabled() const {
        return Renderer ? Renderer->Settings().GetUseWater() : false;
    }

    double OceanBridge::WindDirectionDegrees() const {
        return Renderer
            ? static_cast<double>(Renderer->Settings().GetWaterWindDirection()) * kRadiansToDegrees
            : 1.0 * kRadiansToDegrees;
    }

    double OceanBridge::WindSpeed() const {
        return Renderer ? Renderer->Settings().GetWaterWindSpeed() : 4.0;
    }

    double OceanBridge::FetchKm() const {
        return Renderer ? Renderer->Settings().GetWaterSpectrumFetch() : 100.0;
    }

    double OceanBridge::DepthM() const {
        return Renderer ? Renderer->Settings().GetWaterOceanDepth() : 100.0;
    }

    double OceanBridge::Swell() const {
        return Renderer ? Renderer->Settings().GetWaterSwell() : 0.25;
    }

    double OceanBridge::WavesAmount() const {
        return Renderer ? Renderer->Settings().GetWaterWavesAmount() : 1.5;
    }

    double OceanBridge::Displacement() const {
        return Renderer ? Renderer->Settings().GetWaterFFTDisplacementScale() : 1.0;
    }

    double OceanBridge::Choppy() const {
        return Renderer ? Renderer->Settings().GetWaterFFTChoppyScale() : 1.5;
    }

    void OceanBridge::SetWindDirectionDegrees(double _Degrees) {
        if (!Renderer || !std::isfinite(_Degrees)) return;
        const double Degrees = std::clamp(_Degrees, 0.0, 360.0);
        Renderer->Settings().SetWaterWindDirection(
            static_cast<Smile::f32>(Degrees * kDegreesToRadians));
        emit SettingsChanged();
    }

    void OceanBridge::SetWindSpeed(double _MetresPerSecond) {
        if (!Renderer || !std::isfinite(_MetresPerSecond)) return;
        Renderer->Settings().SetWaterWindSpeed(
            static_cast<Smile::f32>(std::clamp(_MetresPerSecond, 0.1, 40.0)));
        emit SettingsChanged();
    }

    void OceanBridge::SetFetchKm(double _Kilometres) {
        if (!Renderer || !std::isfinite(_Kilometres)) return;
        Renderer->Settings().SetWaterSpectrumFetch(
            static_cast<Smile::f32>(std::clamp(_Kilometres, 1.0, 1000.0)));
        emit SettingsChanged();
    }

    void OceanBridge::SetDepthM(double _Metres) {
        if (!Renderer || !std::isfinite(_Metres)) return;
        Renderer->Settings().SetWaterOceanDepth(
            static_cast<Smile::f32>(std::clamp(_Metres, 1.0, 5000.0)));
        emit SettingsChanged();
    }

    void OceanBridge::SetSwell(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetWaterSwell(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void OceanBridge::SetWavesAmount(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetWaterWavesAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 4.0)));
        emit SettingsChanged();
    }

    void OceanBridge::SetDisplacement(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetWaterFFTDisplacementScale(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 4.0)));
        emit SettingsChanged();
    }

    void OceanBridge::SetChoppy(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetWaterFFTChoppyScale(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 4.0)));
        emit SettingsChanged();
    }
}
