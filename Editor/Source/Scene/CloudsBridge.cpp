#include "SmileEditor/Scene/CloudsBridge.h"

#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"

#include <QFileDialog>

#include <algorithm>
#include <cmath>
#include <utility>

namespace SmileEditor {
    CloudsBridge::CloudsBridge(QObject* _Parent) : QObject(_Parent) {}

    void CloudsBridge::SetViewport(ViewportWidget* _Viewport) {
        Viewport = _Viewport;
    }

    void CloudsBridge::SetRenderer(RendererHandle _Renderer) {
        Renderer = std::move(_Renderer);
        emit AvailableChanged();
        emit SettingsChanged();
    }

    bool CloudsBridge::IsEnabled() const {
        return Renderer ? Renderer->Settings().GetUseClouds() : false;
    }

    bool CloudsBridge::IsHalfResolution() const {
        return Renderer ? Renderer->Settings().GetCloudsHalfRes() : true;
    }

    bool CloudsBridge::IsTemporal() const {
        return Renderer ? Renderer->Settings().GetCloudsTemporal() : true;
    }

    double CloudsBridge::Coverage() const {
        return Renderer ? Renderer->Settings().GetCloudCoverage() : 0.45;
    }

    double CloudsBridge::Density() const {
        return Renderer ? Renderer->Settings().GetCloudDensityScale() : 1.6;
    }

    double CloudsBridge::WindSpeed() const {
        return Renderer ? Renderer->Settings().GetCloudWindSpeed() : 0.01;
    }

    double CloudsBridge::Erosion() const {
        return Renderer ? Renderer->Settings().GetCloudErosion() : 0.45;
    }

    double CloudsBridge::PhaseG() const {
        return Renderer ? Renderer->Settings().GetCloudPhaseG() : 0.8;
    }

    double CloudsBridge::Powder() const {
        return Renderer ? Renderer->Settings().GetCloudPowder() : 0.5;
    }

    double CloudsBridge::Ambient() const {
        return Renderer ? Renderer->Settings().GetCloudAmbientScale() : 1.0;
    }

    double CloudsBridge::TypeBias() const {
        return Renderer ? Renderer->Settings().GetCloudTypeBias() : 0.0;
    }

    double CloudsBridge::PeakVariation() const {
        return Renderer ? Renderer->Settings().GetCloudPeakVariation() : 1.0;
    }

    int CloudsBridge::WeatherSeed() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetCloudWeatherSeed()) : 1337;
    }

    int CloudsBridge::WeatherCells() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetCloudWeatherCells()) : 3;
    }

    bool CloudsBridge::IsWeatherAuthored() const {
        return Renderer ? Renderer->Settings().CloudWeatherAuthored() : false;
    }

    bool CloudsBridge::AreShadowsEnabled() const {
        return Renderer ? Renderer->Settings().GetCloudShadowsEnabled() : true;
    }

    double CloudsBridge::ShadowStrength() const {
        return Renderer ? Renderer->Settings().GetCloudShadowStrength() : 1.0;
    }

    double CloudsBridge::BottomKm() const {
        return Renderer ? Renderer->Settings().GetCloudBottomAltitude() : 2.0;
    }

    double CloudsBridge::ThicknessKm() const {
        return Renderer ? Renderer->Settings().GetCloudThickness() : 3.0;
    }

    int CloudsBridge::MarchSteps() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetCloudMarchSteps()) : 128;
    }

    void CloudsBridge::SetEnabled(bool _Enabled) {
        if (!Renderer || IsEnabled() == _Enabled) return;
        Renderer->Settings().SetUseClouds(_Enabled);
        emit SettingsChanged();
    }

    void CloudsBridge::SetTemporal(bool _Enabled) {
        if (!Renderer || IsTemporal() == _Enabled) return;
        Renderer->Settings().SetCloudsTemporal(_Enabled);
        emit SettingsChanged();
    }

    void CloudsBridge::SetHalfResolution(bool _HalfResolution) {
        if (!Renderer || !Viewport || IsHalfResolution() == _HalfResolution) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("cloud-half-res"),
            [_HalfResolution](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetCloudsHalfRes(_HalfResolution);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (!_Success) return;
                emit SettingsChanged();
                if (Viewport) Viewport->NotifyRendererResourcesChanged();
            });
    }

    void CloudsBridge::SetCoverage(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudCoverage(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetDensity(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudDensityScale(
            static_cast<Smile::f32>(std::clamp(_Value, 0.1, 10.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetWindSpeed(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudWindSpeed(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 0.05)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetErosion(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudErosion(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetPhaseG(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudPhaseG(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 0.95)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetPowder(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudPowder(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetAmbient(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudAmbientScale(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 3.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetTypeBias(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudTypeBias(
            static_cast<Smile::f32>(std::clamp(_Value, -0.5, 0.5)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetPeakVariation(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudPeakVariation(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetWeatherSeed(int _Seed) {
        if (!Renderer || !Viewport) return;
        const Smile::u32 Seed = static_cast<Smile::u32>(std::max(_Seed, 0));
        Viewport->EnqueueRendererJob(
            QStringLiteral("cloud-weather-seed"),
            [Seed](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetCloudWeatherSeed(Seed);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit SettingsChanged();
            });
    }

    void CloudsBridge::SetWeatherCells(int _Cells) {
        if (!Renderer || !Viewport) return;
        const Smile::u32 Cells = static_cast<Smile::u32>(std::clamp(_Cells, 1, 8));
        Viewport->EnqueueRendererJob(
            QStringLiteral("cloud-weather-cells"),
            [Cells](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetCloudWeatherCells(Cells);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit SettingsChanged();
            });
    }

    void CloudsBridge::LoadWeatherTexture() {
        if (!Renderer || !Viewport) return;
        const QString Path = QFileDialog::getOpenFileName(
            Viewport, tr("Weather map (R=cobertura, G=tipo, B=altura de topo)"), QString(),
            tr("Imagens (*.png *.jpg *.jpeg *.tga *.bmp)"));
        if (Path.isEmpty()) return;
        const std::wstring NativePath = Path.toStdWString();
        Viewport->EnqueueRendererJob(
            {},
            [NativePath](Smile::Renderer& _Renderer) {
                RenderThread::JobCompletion Result;
                Result.Success = _Renderer.Settings().LoadCloudWeatherTexture(NativePath);
                if (!Result.Success) Result.Error = "falha ao carregar weather map";
                return Result;
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit SettingsChanged();
            });
    }

    void CloudsBridge::ClearWeatherTexture() {
        if (!Renderer || !Viewport) return;
        Viewport->EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                _Renderer.Settings().ClearCloudWeatherTexture();
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit SettingsChanged();
            });
    }

    void CloudsBridge::SetShadowsEnabled(bool _Enabled) {
        if (!Renderer || AreShadowsEnabled() == _Enabled) return;
        Renderer->Settings().SetCloudShadowsEnabled(_Enabled);
        emit SettingsChanged();
    }

    void CloudsBridge::SetShadowStrength(double _Value) {
        if (!Renderer || !std::isfinite(_Value)) return;
        Renderer->Settings().SetCloudShadowStrength(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetAltitude(double _BottomKm, double _ThicknessKm) {
        if (!Renderer || !std::isfinite(_BottomKm) || !std::isfinite(_ThicknessKm)) return;
        Renderer->Settings().SetCloudAltitude(
            static_cast<Smile::f32>(std::clamp(_BottomKm, 0.5, 8.0)),
            static_cast<Smile::f32>(std::clamp(_ThicknessKm, 0.5, 8.0)));
        emit SettingsChanged();
    }

    void CloudsBridge::SetMarchSteps(int _Steps) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudMarchSteps(
            static_cast<Smile::f32>(std::clamp(_Steps, 32, 256)));
        emit SettingsChanged();
    }
}
