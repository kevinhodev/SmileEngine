#include "SmileEditor/RenderSettingsBridge.h"
#include "SmileEditor/ViewportWidget.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/RenderSettings.h"

#include <QFileDialog>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

// Corpos movidos do ViewportWidget.cpp, sem reescrita: mesmos clamps, mesmos defaults de
// "antes do renderer existir" e mesmos sinais. Este passo e de ORGANIZACAO — se um valor
// mudar, e erro de digitacao no move.
namespace SmileEditor {
    namespace {
        constexpr double kRadiansToDegrees = 57.2957795131;
        constexpr double kDegreesToRadians = 0.01745329252;
    }

    RenderSettingsBridge::RenderSettingsBridge(QObject* _Parent) : QObject(_Parent) {}

    void RenderSettingsBridge::SetRenderer(RendererHandle _R) {
        Renderer = std::move(_R);
        emit AvailableChanged();
        // Uma emissao por dominio: o QML reavalia os cards com os valores reais do motor, que
        // podem divergir dos defaults compilados na bridge.
        emit OceanSettingsChanged();
        emit WeatherSettingsChanged();
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
        emit RenderSettingsChanged();
        emit RendererInitialized();
    }

    // === Oceano / agua ==================================================================

    bool RenderSettingsBridge::IsOceanEnabled() const {
        return Renderer ? Renderer->Settings().GetUseWater() : false;
    }

    double RenderSettingsBridge::GetOceanWindDirectionDegrees() const {
        return Renderer
            ? static_cast<double>(Renderer->Settings().GetWaterWindDirection()) * kRadiansToDegrees
            : 1.0 * kRadiansToDegrees;
    }

    double RenderSettingsBridge::GetOceanWindSpeed() const {
        return Renderer ? Renderer->Settings().GetWaterWindSpeed() : 4.0;
    }

    double RenderSettingsBridge::GetOceanFetchKm() const {
        return Renderer ? Renderer->Settings().GetWaterSpectrumFetch() : 100.0;
    }

    double RenderSettingsBridge::GetOceanDepthM() const {
        return Renderer ? Renderer->Settings().GetWaterOceanDepth() : 100.0;
    }

    double RenderSettingsBridge::GetOceanSwell() const {
        return Renderer ? Renderer->Settings().GetWaterSwell() : 0.25;
    }

    double RenderSettingsBridge::GetOceanWavesAmount() const {
        return Renderer ? Renderer->Settings().GetWaterWavesAmount() : 1.5;
    }

    double RenderSettingsBridge::GetOceanFFTDisplacement() const {
        return Renderer ? Renderer->Settings().GetWaterFFTDisplacementScale() : 1.0;
    }

    double RenderSettingsBridge::GetOceanFFTChoppy() const {
        return Renderer ? Renderer->Settings().GetWaterFFTChoppyScale() : 1.5;
    }

    void RenderSettingsBridge::SetOceanEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseWater(_Enabled);
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanWindDirectionDegrees(double _Degrees) {
        if (!Renderer) return;
        const double Degrees = std::isfinite(_Degrees)
            ? std::clamp(_Degrees, 0.0, 360.0) : 1.0 * kRadiansToDegrees;
        Renderer->Settings().SetWaterWindDirection(
            static_cast<Smile::f32>(Degrees * kDegreesToRadians));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanWindSpeed(double _MetresPerSecond) {
        if (!Renderer) return;
        const double Value = std::isfinite(_MetresPerSecond)
            ? std::clamp(_MetresPerSecond, 0.1, 40.0) : 4.0;
        Renderer->Settings().SetWaterWindSpeed(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanFetchKm(double _Kilometres) {
        if (!Renderer) return;
        const double Value = std::isfinite(_Kilometres)
            ? std::clamp(_Kilometres, 1.0, 1000.0) : 100.0;
        Renderer->Settings().SetWaterSpectrumFetch(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanDepthM(double _Metres) {
        if (!Renderer) return;
        const double Value = std::isfinite(_Metres)
            ? std::clamp(_Metres, 1.0, 5000.0) : 100.0;
        Renderer->Settings().SetWaterOceanDepth(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanSwell(double _Value) {
        if (!Renderer) return;
        const double Value = std::isfinite(_Value) ? std::clamp(_Value, 0.0, 1.0) : 0.25;
        Renderer->Settings().SetWaterSwell(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanWavesAmount(double _Value) {
        if (!Renderer) return;
        const double Value = std::isfinite(_Value) ? std::clamp(_Value, 0.0, 4.0) : 1.5;
        Renderer->Settings().SetWaterWavesAmount(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanFFTDisplacement(double _Value) {
        if (!Renderer) return;
        const double Value = std::isfinite(_Value) ? std::clamp(_Value, 0.0, 4.0) : 1.0;
        Renderer->Settings().SetWaterFFTDisplacementScale(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }

    void RenderSettingsBridge::SetOceanFFTChoppy(double _Value) {
        if (!Renderer) return;
        const double Value = std::isfinite(_Value) ? std::clamp(_Value, 0.0, 4.0) : 1.5;
        Renderer->Settings().SetWaterFFTChoppyScale(static_cast<Smile::f32>(Value));
        emit OceanSettingsChanged();
    }
    // === Clima (FWeather; defaults espelham o struct p/ antes do renderer existir) =====

    double RenderSettingsBridge::GetRainAmount() const {
        return Renderer ? Renderer->Settings().GetRainAmount() : 0.0;
    }
    void RenderSettingsBridge::SetRainAmount(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetRainAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit WeatherSettingsChanged();
    }

    double RenderSettingsBridge::GetPuddleAmount() const {
        return Renderer ? Renderer->Settings().GetRainPuddleAmount() : 0.65;
    }
    void RenderSettingsBridge::SetPuddleAmount(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetRainPuddleAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit WeatherSettingsChanged();
    }

    double RenderSettingsBridge::GetPuddleScale() const {
        return Renderer ? Renderer->Settings().GetRainPuddleScale() : 8.0;
    }
    void RenderSettingsBridge::SetPuddleScale(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetRainPuddleScale(
            static_cast<Smile::f32>(std::clamp(_Value, 1.0, 64.0)));
        emit WeatherSettingsChanged();
    }

    double RenderSettingsBridge::GetRippleStrength() const {
        return Renderer ? Renderer->Settings().GetRainRippleStrength() : 1.0;
    }
    void RenderSettingsBridge::SetRippleStrength(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetRainRippleStrength(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 2.0)));
        emit WeatherSettingsChanged();
    }

    double RenderSettingsBridge::GetWetDarkening() const {
        return Renderer ? Renderer->Settings().GetRainWetDarkening() : 0.85;
    }
    void RenderSettingsBridge::SetWetDarkening(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetRainWetDarkening(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit WeatherSettingsChanged();
    }

    double RenderSettingsBridge::GetCurtainAmount() const {
        return Renderer ? Renderer->Settings().GetRainCurtainAmount() : 1.0;
    }
    void RenderSettingsBridge::SetCurtainAmount(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetRainCurtainAmount(
            static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0)));
        emit WeatherSettingsChanged();
    }

    bool RenderSettingsBridge::IsRainOcclusion() const {
        return Renderer ? Renderer->Settings().GetRainOcclusion() : true;
    }
    void RenderSettingsBridge::SetRainOcclusion(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetRainOcclusion(_Enabled);
        emit WeatherSettingsChanged();
    }

    bool RenderSettingsBridge::AreRainParticles() const {
        return Renderer ? Renderer->Settings().GetRainParticles() : true;
    }
    void RenderSettingsBridge::SetRainParticles(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetRainParticles(_Enabled);
        emit WeatherSettingsChanged();
    }

    bool RenderSettingsBridge::IsWeatherDriveSky() const {
        return Renderer ? Renderer->Settings().GetRainDriveSky() : true;
    }
    void RenderSettingsBridge::SetWeatherDriveSky(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetRainDriveSky(_Enabled);
        emit WeatherSettingsChanged();
    }

    // === Nuvens volumetricas ============================================================
    // Os knobs que recriam recurso (half-res) ou re-bakeiam o weather map (seed/cells/textura)
    // vao por job na fila do viewport, que os serializa com os frames.

    bool RenderSettingsBridge::AreCloudsEnabled() const {
        return Renderer ? Renderer->Settings().GetUseClouds() : false;
    }

    void RenderSettingsBridge::SetCloudsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseClouds(_Enabled);
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::AreCloudsHalfRes() const {
        return Renderer ? Renderer->Settings().GetCloudsHalfRes() : true;
    }

    bool RenderSettingsBridge::AreCloudsTemporal() const {
        return Renderer ? Renderer->Settings().GetCloudsTemporal() : true;
    }

    void RenderSettingsBridge::SetCloudsTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudsTemporal(_Enabled);
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetCloudsHalfRes(bool _HalfRes) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("cloud-half-res"),
            [_HalfRes](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetCloudsHalfRes(_HalfRes);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit CloudSettingsChanged();
                    // Recriar o RT das nuvens remapeia a lista de alvos de debug (o registro e
                    // por nome): o visualizador precisa reler. Fica no viewport, que e o dono
                    // dessa lista.
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }

    double RenderSettingsBridge::GetCloudCoverage() const {
        return Renderer ? Renderer->Settings().GetCloudCoverage() : 0.45;
    }

    void RenderSettingsBridge::SetCloudCoverage(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudCoverage(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudDensity() const {
        return Renderer ? Renderer->Settings().GetCloudDensityScale() : 1.6;
    }

    void RenderSettingsBridge::SetCloudDensity(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudDensityScale(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudWindSpeed() const {
        return Renderer ? Renderer->Settings().GetCloudWindSpeed() : 0.01;
    }

    void RenderSettingsBridge::SetCloudWindSpeed(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudWindSpeed(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudErosion() const {
        return Renderer ? Renderer->Settings().GetCloudErosion() : 0.45;
    }

    void RenderSettingsBridge::SetCloudErosion(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudErosion(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudPhaseG() const {
        return Renderer ? Renderer->Settings().GetCloudPhaseG() : 0.8;
    }

    void RenderSettingsBridge::SetCloudPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudPhaseG(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudPowder() const {
        return Renderer ? Renderer->Settings().GetCloudPowder() : 0.5;
    }

    void RenderSettingsBridge::SetCloudPowder(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudPowder(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudAmbient() const {
        return Renderer ? Renderer->Settings().GetCloudAmbientScale() : 1.0;
    }

    void RenderSettingsBridge::SetCloudAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudAmbientScale(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudTypeBias() const {
        return Renderer ? Renderer->Settings().GetCloudTypeBias() : 0.0;
    }

    void RenderSettingsBridge::SetCloudTypeBias(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudTypeBias(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudPeakVariation() const {
        return Renderer ? Renderer->Settings().GetCloudPeakVariation() : 1.0;
    }

    void RenderSettingsBridge::SetCloudPeakVariation(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudPeakVariation(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    int RenderSettingsBridge::GetCloudWeatherSeed() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetCloudWeatherSeed()) : 1337;
    }

    void RenderSettingsBridge::SetCloudWeatherSeed(int _Seed) {
        if (!Renderer) return;
        const Smile::u32 Seed = static_cast<Smile::u32>(_Seed < 0 ? 0 : _Seed);
        Viewport->EnqueueRendererJob(
            QStringLiteral("cloud-weather-seed"),
            [Seed](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetCloudWeatherSeed(Seed);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
    }

    int RenderSettingsBridge::GetCloudWeatherCells() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetCloudWeatherCells()) : 3;
    }

    void RenderSettingsBridge::SetCloudWeatherCells(int _Mult) {
        if (!Renderer) return;
        const Smile::u32 Cells = static_cast<Smile::u32>(_Mult < 1 ? 1 : _Mult);
        Viewport->EnqueueRendererJob(
            QStringLiteral("cloud-weather-cells"),
            [Cells](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetCloudWeatherCells(Cells);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
    }

    bool RenderSettingsBridge::IsCloudWeatherAuthored() const {
        return Renderer ? Renderer->Settings().CloudWeatherAuthored() : false;
    }

    void RenderSettingsBridge::LoadCloudWeatherTexture() {
        if (!Renderer) return;
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
                if (_Success) emit CloudSettingsChanged();
            });
    }

    void RenderSettingsBridge::ClearCloudWeatherTexture() {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                _Renderer.Settings().ClearCloudWeatherTexture();
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
    }

    bool RenderSettingsBridge::AreCloudShadowsEnabled() const {
        return Renderer ? Renderer->Settings().GetCloudShadowsEnabled() : true;
    }

    void RenderSettingsBridge::SetCloudShadowsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudShadowsEnabled(_Enabled);
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudShadowStrength() const {
        return Renderer ? Renderer->Settings().GetCloudShadowStrength() : 1.0;
    }

    void RenderSettingsBridge::SetCloudShadowStrength(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudShadowStrength(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetCloudBottomKm() const {
        return Renderer ? Renderer->Settings().GetCloudBottomAltitude() : 2.0;
    }

    double RenderSettingsBridge::GetCloudThicknessKm() const {
        return Renderer ? Renderer->Settings().GetCloudThickness() : 3.0;
    }

    void RenderSettingsBridge::SetCloudAltitude(double _BottomKm, double _ThicknessKm) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudAltitude(static_cast<Smile::f32>(_BottomKm),
                                                    static_cast<Smile::f32>(_ThicknessKm));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    int RenderSettingsBridge::GetCloudMarchSteps() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetCloudMarchSteps()) : 128;
    }

    void RenderSettingsBridge::SetCloudMarchSteps(int _Steps) {
        if (!Renderer) return;
        Renderer->Settings().SetCloudMarchSteps(static_cast<Smile::f32>(_Steps));
        emit CloudSettingsChanged();
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }
    // === Sun shafts e fog volumetrico ===================================================

    bool RenderSettingsBridge::AreSunShaftsEnabled() const {
        return Renderer ? Renderer->Settings().GetUseSunShafts() : true;
    }

    void RenderSettingsBridge::SetSunShaftsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseSunShafts(_Enabled);
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsIntensity() const {
        return Renderer ? Renderer->Settings().GetShaftsIntensity() : 1.0;
    }

    void RenderSettingsBridge::SetSunShaftsIntensity(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsIntensity(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsDust() const {
        return Renderer ? Renderer->Settings().GetShaftsDust() : 8.0;
    }

    void RenderSettingsBridge::SetSunShaftsDust(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsDust(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsPhaseG() const {
        return Renderer ? Renderer->Settings().GetShaftsPhaseG() : 0.7;
    }

    void RenderSettingsBridge::SetSunShaftsPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsPhaseG(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    int RenderSettingsBridge::GetSunShaftsSteps() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetShaftsSteps()) : 32;
    }

    void RenderSettingsBridge::SetSunShaftsSteps(int _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsSteps(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsRange() const {
        return Renderer ? Renderer->Settings().GetShaftsMaxDist() : 128.0;
    }

    void RenderSettingsBridge::SetSunShaftsRange(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsMaxDist(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    bool RenderSettingsBridge::AreSunShaftsTemporal() const {
        return Renderer ? Renderer->Settings().GetShaftsTemporal() : true;
    }

    void RenderSettingsBridge::SetSunShaftsTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsTemporal(_Enabled);
        emit SunShaftsSettingsChanged();
    }

    bool RenderSettingsBridge::IsVolFogEnabled() const {
        return Renderer ? Renderer->Settings().GetUseVolumetricFog() : true;
    }

    void RenderSettingsBridge::SetVolFogEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseVolumetricFog(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogDistance() const {
        return Renderer ? Renderer->Settings().GetVolFogMaxDistance() : 100.0;
    }

    void RenderSettingsBridge::SetVolFogDistance(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogMaxDistance(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogPhaseG() const {
        return Renderer ? Renderer->Settings().GetVolFogPhaseG() : 0.3;
    }

    void RenderSettingsBridge::SetVolFogPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogPhaseG(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogDensity() const {
        return Renderer ? Renderer->Settings().GetVolFogExtinctionScale() : 1.0;
    }

    void RenderSettingsBridge::SetVolFogDensity(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogExtinctionScale(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogAmbient() const {
        return Renderer ? Renderer->Settings().GetVolFogAmbientIntensity() : 1.0;
    }

    void RenderSettingsBridge::SetVolFogAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogAmbientIntensity(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetHeightFogSkyContribution() const {
        return Renderer ? Renderer->Settings().GetFogHeightSkyContribution() : 1.0;
    }

    void RenderSettingsBridge::SetHeightFogSkyContribution(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetFogHeightSkyContribution(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsPerPixelAtmoTransmittance() const {
        return Renderer ? Renderer->Settings().GetPerPixelAtmoTransmittance() : true;
    }

    void RenderSettingsBridge::SetPerPixelAtmoTransmittance(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetPerPixelAtmoTransmittance(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsSkyAmbientSH() const {
        return Renderer ? Renderer->Settings().GetSkyAmbientSH() : true;
    }

    void RenderSettingsBridge::SetSkyAmbientSH(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetSkyAmbientSH(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsVolFogTemporal() const {
        return Renderer ? Renderer->Settings().GetVolFogTemporal() : true;
    }

    void RenderSettingsBridge::SetVolFogTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogTemporal(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogLights() const {
        return Renderer ? Renderer->Settings().GetVolFogLightsIntensity() : 1.0;
    }

    void RenderSettingsBridge::SetVolFogLights(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogLightsIntensity(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsVolFogConsDepth() const {
        return Renderer ? Renderer->Settings().GetVolFogConservativeDepth() : true;
    }

    void RenderSettingsBridge::SetVolFogConsDepth(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogConservativeDepth(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    // === Sombras do sol =================================================================

    bool RenderSettingsBridge::AreSunShadowsEnabled() const {
        return Renderer && Renderer->Settings().GetUseSunShadows();
    }

    bool RenderSettingsBridge::IsShadowCacheEnabled() const {
        return Renderer && Renderer->Settings().GetShadowCascadeCache();
    }

    bool RenderSettingsBridge::IsShadowDebugCascades() const {
        return Renderer && Renderer->Settings().GetShadowDebugCascades();
    }

    double RenderSettingsBridge::GetShadowMaxDistance() const {
        return Renderer ? Renderer->Settings().GetShadowMaxDistance() : 800.0;
    }

    double RenderSettingsBridge::GetShadowDepthBias() const {
        return Renderer ? Renderer->Settings().GetShadowDepthBias() : 2.0; // texels da cascata
    }

    double RenderSettingsBridge::GetShadowNormalOffset() const {
        return Renderer ? Renderer->Settings().GetShadowNormalOffset() : 2.5;
    }


    double RenderSettingsBridge::GetShadowMinCasterTexels() const {
        return Renderer ? Renderer->Settings().GetShadowMinCasterTexels() : 2.0;
    }

    QVariantList RenderSettingsBridge::GetShadowCascadeBias() const {
        QVariantList List;
        for (Smile::u32 c = 0; c < Smile::FSunShadows::kNumCascades; ++c)
            List.append(Renderer ? Renderer->Settings().GetShadowCascadeBiasScale(c) : 1.0);
        return List;
    }

    void RenderSettingsBridge::SetSunShadowsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseSunShadows(_Enabled);
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowCacheEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowCascadeCache(_Enabled);
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowDebugCascades(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowDebugCascades(_Enabled);
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowMaxDistance(double _Distance) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowMaxDistance(static_cast<Smile::f32>(_Distance));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowDepthBias(double _Bias) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowDepthBias(static_cast<Smile::f32>(_Bias));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowNormalOffset(double _Texels) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowNormalOffset(static_cast<Smile::f32>(_Texels));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowMinCasterTexels(double _Texels) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowMinCasterTexels(static_cast<Smile::f32>(_Texels));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowCascadeBiasScale(int _Cascade, double _Scale) {
        if (!Renderer || _Cascade < 0) return;
        Renderer->Settings().SetShadowCascadeBiasScale(static_cast<Smile::u32>(_Cascade),
                                                      static_cast<Smile::f32>(_Scale));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetShadowSunAngle() const {
        return Renderer ? Renderer->Settings().GetSunAngularSize() : 0.53;
    }

    void RenderSettingsBridge::SetShadowSunAngle(double _Degrees) {
        if (!Renderer) return;
        Renderer->Settings().SetSunAngularSize(static_cast<Smile::f32>(_Degrees));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    // === Iluminacao global ==============================================================

    bool RenderSettingsBridge::IsDDGIEnabled() const {
        return Renderer && Renderer->Settings().GetUseGI();
    }

    bool RenderSettingsBridge::IsReSTIRGIEnabled() const {
        return Renderer && Renderer->Settings().GetUseReSTIRGI();
    }

    bool RenderSettingsBridge::IsReGIREnabled() const {
        return Renderer && Renderer->Settings().GetUseReGIR();
    }

    bool RenderSettingsBridge::IsReSTIRGIVisibilityEnabled() const {
        return Renderer && Renderer->Settings().GetGIVisibility();
    }

    bool RenderSettingsBridge::AreGIFoliageShadowsEnabled() const {
        return Renderer && Renderer->Settings().GetGIFoliageShadows();
    }

    bool RenderSettingsBridge::IsGIAdaptiveHysteresisEnabled() const {
        return Renderer && Renderer->Settings().GetGIAdaptiveHysteresis();
    }

    bool RenderSettingsBridge::IsGIAdaptiveRaysEnabled() const {
        return Renderer && Renderer->Settings().GetGIAdaptiveRays();
    }

    int RenderSettingsBridge::GetGICascadeCount() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetGICascadeCount()) : 1;
    }

    bool RenderSettingsBridge::IsGIMeasureTerminatorOff() const {
        return Renderer && Renderer->Settings().GetGIMeasureTerminatorOff();
    }

    bool RenderSettingsBridge::IsReflectionsCullBackfaceEnabled() const {
        return Renderer && Renderer->Settings().GetReflectionsCullBackface();
    }

    bool RenderSettingsBridge::IsReSTIRDIEnabled() const {
        return Renderer && Renderer->Settings().GetUseReSTIRDI();
    }

    bool RenderSettingsBridge::IsGIBackfacePolicyEnabled() const {
        return Renderer && Renderer->Settings().GetGIBackfacePolicy();
    }

    double RenderSettingsBridge::GetGISurfaceBiasMax() const {
        return Renderer ? static_cast<double>(Renderer->Settings().GetGISurfaceBiasMax()) : 0.0;
    }

    double RenderSettingsBridge::GetGIVolumeFadeProbes() const {
        return Renderer ? static_cast<double>(Renderer->Settings().GetGIVolumeFadeProbes()) : 0.0;
    }


    void RenderSettingsBridge::ToggleDDGI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseGI(!RendererAccess->Settings().GetUseGI());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReSTIRGI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReSTIRGI(!RendererAccess->Settings().GetUseReSTIRGI());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReGIR() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReGIR(!RendererAccess->Settings().GetUseReGIR());
        emit GISettingsChanged();
    }

    // --- World radiance cache -----------------------------------------------------------
    bool RenderSettingsBridge::IsRadianceCacheEnabled() const {
        return Renderer && Renderer->Settings().GetRadianceCacheEnabled();
    }
    bool RenderSettingsBridge::IsRadianceCacheQuery() const {
        return Renderer && Renderer->Settings().GetRadianceCacheQuery();
    }
    bool RenderSettingsBridge::IsRadianceCacheStats() const {
        return Renderer && Renderer->Settings().GetRadianceCacheStatsEnabled();
    }
    bool RenderSettingsBridge::IsRadianceCacheDedicatedUpdate() const {
        return Renderer && Renderer->Settings().GetRadianceCacheDedicatedUpdate();
    }
    double RenderSettingsBridge::GetRadianceCacheUpdateFraction() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheUpdateFraction() : 0.04;
    }
    bool RenderSettingsBridge::IsRadianceCachePrevTerminal() const {
        return Renderer && Renderer->Settings().GetRadianceCacheUsePrevTerminal();
    }
    double RenderSettingsBridge::GetRadianceCacheCellSize() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheCellSize() : 0.50;
    }
    double RenderSettingsBridge::GetRadianceCacheLodDistance() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheLodDistance() : 6.0;
    }
    int RenderSettingsBridge::GetRadianceCacheDebugMode() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetRadianceCacheDebugMode()) : 0;
    }
    double RenderSettingsBridge::GetRadianceCacheOccupancy() const {
        if (!Renderer) return 0.0;
        const Smile::u32 Cap = Renderer->Settings().RadianceCacheCapacity();
        if (Cap == 0) return 0.0;
        return 100.0 * static_cast<double>(Renderer->Settings().RadianceCacheStats().Occupied) /
               static_cast<double>(Cap);
    }
    double RenderSettingsBridge::GetRadianceCacheHitRate() const {
        if (!Renderer) return 0.0;
        const auto& S = Renderer->Settings().RadianceCacheStats();
        if (S.Queries == 0) return 0.0;
        return 100.0 * static_cast<double>(S.Hits) / static_cast<double>(S.Queries);
    }
    double RenderSettingsBridge::GetRadianceCacheConvergence() const {
        if (!Renderer) return 0.0;
        const auto& S = Renderer->Settings().RadianceCacheStats();
        if (S.Valid == 0) return 0.0;
        return static_cast<double>(S.Samples) / static_cast<double>(S.Valid);
    }
    double RenderSettingsBridge::GetRadianceCacheMemoryMB() const {
        if (!Renderer) return 0.0;
        return static_cast<double>(Renderer->Settings().RadianceCacheBytes()) / (1024.0 * 1024.0);
    }
    QString RenderSettingsBridge::GetRadianceCacheSummary() const {
        if (!Renderer) return QString();
        const auto& S = Renderer->Settings().RadianceCacheStats();
        const Smile::u32 Cap = Renderer->Settings().RadianceCacheCapacity();
        if (Cap == 0) return QStringLiteral("cache não montado");
        // Queries = 0 nao e o mesmo que taxa de acerto 0: sem a instrumentacao ligada o
        // contador nem roda, e mostrar "0%" ali seria mentira.
        const QString Hit = Renderer->Settings().GetRadianceCacheStatsEnabled()
            ? QString::number(GetRadianceCacheHitRate(), 'f', 1) + QStringLiteral("%")
            : QStringLiteral("— (instrumentação off)");
        return QStringLiteral("%1 / %2 células · acerto %3 · %4 amostras/célula · despejadas %5")
            .arg(S.Occupied).arg(Cap).arg(Hit)
            .arg(QString::number(GetRadianceCacheConvergence(), 'f', 1))
            .arg(S.Evicted);
    }

    void RenderSettingsBridge::ToggleRadianceCache() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheEnabled(!A->Settings().GetRadianceCacheEnabled());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheQuery() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheQuery(!A->Settings().GetRadianceCacheQuery());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheStats() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheStatsEnabled(!A->Settings().GetRadianceCacheStatsEnabled());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheDedicatedUpdate() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheDedicatedUpdate(
            !A->Settings().GetRadianceCacheDedicatedUpdate());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheUpdateFraction(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheUpdateFraction(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCachePrevTerminal() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheUsePrevTerminal(
            !A->Settings().GetRadianceCacheUsePrevTerminal());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheCellSize(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheCellSize(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheLodDistance(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheLodDistance(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheDebugMode(int V) {
        if (!Renderer || V < 0) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheDebugMode(static_cast<Smile::u32>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ResetRadianceCache() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().ResetRadianceCache();
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::RefreshRadianceCacheStats() { emit StatsChanged(); }

    void RenderSettingsBridge::ToggleReSTIRGIVisibility() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIVisibility(!Settings.GetGIVisibility());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIFoliageShadows() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // O fan-out p/ os 3 consumidores do HitShading mora no FRenderSettings — era aqui.
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIFoliageShadows(!Settings.GetGIFoliageShadows());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIAdaptiveHysteresis() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // O reset do atlas (e de tudo que se apoia nele) mora no FRenderSettings, dominio
        // GIAccumulation — sem ele o lado "ligado" do A/B comecaria com sondas convergidas
        // pelo lado "desligado".
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIAdaptiveHysteresis(!Settings.GetGIAdaptiveHysteresis());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIAdaptiveRays() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIAdaptiveRays(!Settings.GetGIAdaptiveRays());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetGICascadeCount(int _V) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // Recria o volume do DDGI (atlas, ProbesTrace, buffers, dispatch). O lock e o mesmo dos
        // outros knobs; a realocacao acontece fora da gravacao do frame.
        RendererAccess->Settings().SetGICascadeCount(static_cast<Smile::u32>(_V < 1 ? 1 : _V));
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIMeasureTerminatorOff() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIMeasureTerminatorOff(!Settings.GetGIMeasureTerminatorOff());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReflectionsCullBackface() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetReflectionsCullBackface(
            !RendererAccess->Settings().GetReflectionsCullBackface());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReSTIRDI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReSTIRDI(!RendererAccess->Settings().GetUseReSTIRDI());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIBackfacePolicy() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // Pelo Renderer: alem dos reservoirs, o NRD/RR/TAA acumulam sobre o resultado e
        // precisam cair juntos, senao o A/B denoisado compara estado misturado.
        RendererAccess->Settings().SetGIBackfacePolicy(!RendererAccess->Settings().GetGIBackfacePolicy());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetGISurfaceBiasMax(double _Meters) {
        if (!Renderer) return;
        Renderer->Settings().SetGISurfaceBiasMax(static_cast<float>(qBound(0.0, _Meters, 2.0)));
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetGIVolumeFadeProbes(double _Probes) {
        if (!Renderer) return;
        Renderer->Settings().SetGIVolumeFadeProbes(static_cast<float>(qBound(0.0, _Probes, 3.0)));
        emit GISettingsChanged();
    }


    namespace {
        // Tabela unica dos knobs de epsilon: dirige leitura, escrita e a UI. O ponteiro-p/-membro
        // evita 9 getters e 9 setters quase identicos, e a UI e um Repeater sobre esta lista.
        //
        // UiScale = quantas unidades de UI por unidade de mundo (metro). Os offsets aparecem em
        // MILIMETROS porque o sweep interessante e sub-milimetrico (o Lumen usa 0,1-0,5 mm) e ler
        // "0,0002 m" num slider e inutil. MaxAge e contagem de frames, entao escala 1.
        struct FEpsKnob {
            const char* Key;
            const char* Label;
            const char* Unit;
            const char* Hint;
            float Smile::FRayEpsilonProfile::* Field;
            double UiScale;
            double UiMin;
            double UiMax;
            int    Decimals;
        };

        const FEpsKnob kEpsKnobs[] = {
            { "originFloorMin", "Piso do offset de origem", "mm",
              "Somado ao offset ULP em todo raio que sai do G-buffer. 200 mm e o modo legado e a "
              "causa medida das manchas nas cortinas. Varrer ate 0.",
              &Smile::FRayEpsilonProfile::OriginFloorMin, 1000.0, 0.0, 250.0, 2 },
            { "originFloorPerMeter", "Offset por metro de camera", "mm/m",
              "Termo proporcional a distancia da camera. Heuristico: a 50 m ele sozinho da 10 mm. "
              "Varrer ate 0 — a quantizacao do depth justifica ~0,003 mm.",
              &Smile::FRayEpsilonProfile::OriginFloorPerMeter, 1000.0, 0.0, 1.0, 3 },
            { "originAngularMax", "Bias angular (raio rasante)", "mm",
              "Estilo Lumen: raio rasante leva este valor, perpendicular leva 20% dele. 0 = "
              "desligado. O Lumen usa 0,5 mm — mas em coordenadas camera-relative.",
              &Smile::FRayEpsilonProfile::OriginAngularMax, 1000.0, 0.0, 5.0, 3 },
            { "hitShadowRayBias", "Bias do shadow ray (2o hit)", "mm",
              "Desloca a origem das sombras de sol e puntuais no ponto de hit. Independente do "
              "piso acima: baixar so o piso deixa este dominando o contato.",
              &Smile::FRayEpsilonProfile::HitShadowRayBias, 1000.0, 0.0, 250.0, 2 },
            { "shadowRayBiasMin", "Piso do bias do shadow ray", "mm",
              "ATENCAO: em 1 mm ele mascara qualquer sweep do bias acima abaixo disso — o A/B "
              "pareceria nao fazer diferenca pelo motivo errado.",
              &Smile::FRayEpsilonProfile::ShadowRayBiasMin, 1000.0, 0.0, 10.0, 3 },
            { "shadowRayTMin", "TMin do shadow ray", "mm",
              "Soma com o bias: em 10 mm, oclusor a menos de 1 cm do hit fica invisivel mesmo "
              "depois de o bias cair.",
              &Smile::FRayEpsilonProfile::ShadowRayTMin, 1000.0, 0.0, 50.0, 2 },
            { "visRayTMin", "TMin do visibility ray", "mm",
              "Reuso espacial do ReSTIR. Entra no comprimento minimo da conexao, que e derivado "
              "de TMin + margem + folga.",
              &Smile::FRayEpsilonProfile::VisRayTMin, 1000.0, 0.0, 100.0, 2 },
            { "visRayEndMargin", "Margem final do visibility ray", "mm",
              "Para o raio antes da superficie do x2. Tambem entra no comprimento minimo.",
              &Smile::FRayEpsilonProfile::VisRayEndMargin, 1000.0, 0.0, 100.0, 2 },
            { "maxAge", "Idade maxima do reservoir", "frames",
              "Expira a amostra selecionada (RTXDI usa 30). 0 = sem expiracao, que e o estado "
              "atual. O MCap limita o PESO do historico, nao a vida da amostra.",
              &Smile::FRayEpsilonProfile::MaxAge, 1.0, 0.0, 60.0, 0 },
        };
    }

    QVariantList RenderSettingsBridge::GetRayEpsilons() const {
        QVariantList Out;
        if (!Renderer) return Out;
        auto RendererAccess = Renderer.Lock();
        const Smile::FRayEpsilonProfile& P = Renderer->Settings().GetRayEpsilons();
        for (const FEpsKnob& K : kEpsKnobs) {
            QVariantMap M;
            M["key"]      = QString::fromLatin1(K.Key);
            M["label"]    = QString::fromUtf8(K.Label);
            M["unit"]     = QString::fromUtf8(K.Unit);
            M["hint"]     = QString::fromUtf8(K.Hint);
            M["value"]    = static_cast<double>(P.*(K.Field)) * K.UiScale;
            M["min"]      = K.UiMin;
            M["max"]      = K.UiMax;
            M["decimals"] = K.Decimals;
            Out.append(M);
        }
        return Out;
    }

    void RenderSettingsBridge::SetRayEpsilon(const QString& _Key, double _UiValue) {
        if (!Renderer) return;
        for (const FEpsKnob& K : kEpsKnobs) {
            if (_Key != QLatin1String(K.Key)) continue;
            auto RendererAccess = Renderer.Lock();
            Smile::FRayEpsilonProfile P = RendererAccess->Settings().GetRayEpsilons();
            P.*(K.Field) = static_cast<float>(
                qBound(K.UiMin, _UiValue, K.UiMax) / K.UiScale);
            RendererAccess->Settings().SetRayEpsilons(P); // invalida reservoirs + historico do denoiser
            emit GISettingsChanged();
            return;
        }
    }

    void RenderSettingsBridge::ResetRayEpsilons() {
        if (!Renderer) return;
        Renderer->Settings().SetRayEpsilons(Smile::FRayEpsilonProfile{}); // volta aos defaults do header
        emit GISettingsChanged();
    }
    // === Render, upscaler e denoiser ====================================================

    bool RenderSettingsBridge::IsGTAOEnabled() const {
        return Renderer && Renderer->Settings().GetUseAO();
    }

    bool RenderSettingsBridge::IsGTAOHalfRes() const {
        return Renderer && Renderer->Settings().GetAOHalfRes();
    }

    bool RenderSettingsBridge::AreReflectionsEnabled() const {
        return Renderer && Renderer->Settings().GetUseReflections();
    }

    bool RenderSettingsBridge::IsNrdEnabled() const {
        return Renderer && Renderer->Settings().GetUseNrdDenoise();
    }

    int RenderSettingsBridge::GetDenoiserMode() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetDenoiser()) : 0;  // 0=None 1=NRD 2=DLSS RR
    }

    bool RenderSettingsBridge::IsRRAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Settings().RRAvailable();
    }

    int RenderSettingsBridge::GetUpscalerMode() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetUpscaler()) : 0;  // 0=None 1=FSR 2=DLSS
    }

    bool RenderSettingsBridge::IsFsrAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Settings().UpscalerAvailable(Smile::EUpscaler::FSR);
    }

    bool RenderSettingsBridge::IsDlssAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Settings().UpscalerAvailable(Smile::EUpscaler::DLSS);
    }

    int RenderSettingsBridge::GetUpscalerQuality() const {
        return Renderer ? Renderer->Settings().GetUpscalerQuality() : 0;  // qualidade compartilhada FSR/DLSS
    }

    int RenderSettingsBridge::GetRecommendedUpscalerMode() const {
        if (IsDlssAvailable()) return 2;   // DLSS preferido em NVIDIA
        if (IsFsrAvailable())  return 1;
        return 0;
    }

    QString RenderSettingsBridge::GetRecommendedUpscalerName() const {
        if (IsDlssAvailable()) return QStringLiteral("DLSS");
        if (IsFsrAvailable())  return QStringLiteral("FSR 3.1");
        return QStringLiteral("Sem upscaling");
    }

    double RenderSettingsBridge::GetRenderScale() const {
        return Renderer ? static_cast<double>(Renderer->Settings().GetRenderScale()) : 1.0;
    }

    bool RenderSettingsBridge::IsTAAEnabled() const {
        return Renderer && Renderer->Settings().GetUseTAA();
    }

    bool RenderSettingsBridge::IsFrustumCullingEnabled() const {
        return Renderer && Renderer->Settings().GetFrustumCulling();
    }

    bool RenderSettingsBridge::IsOcclusionCullingEnabled() const {
        return Renderer && Renderer->Settings().GetOcclusionCulling();
    }

    bool RenderSettingsBridge::IsDepthPrepassEnabled() const {
        return Renderer && Renderer->Settings().GetDepthPrepass();
    }

    void RenderSettingsBridge::ToggleGTAO() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseAO(!RendererAccess->Settings().GetUseAO());
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ToggleGTAOHalfRes() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetAOHalfRes(!Settings.GetAOHalfRes());
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ToggleReflections() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReflections(!RendererAccess->Settings().GetUseReflections());
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ToggleNrd() {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetUseNrdDenoise(!_Renderer.Settings().GetUseNrdDenoise());
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }

    void RenderSettingsBridge::SetDenoiserMode(int _Mode) {
        if (!Renderer) return;
        // 0=Nenhum 1=NRD 2=DLSS RR. Selecionar RR forca e trava o upscaler em DLSS (o RR faz o upscale);
        // o Renderer cai p/ NRD se o RR nao estiver disponivel (sem NVIDIA/SDK).
        Viewport->EnqueueRendererJob(
            QStringLiteral("denoiser"),
            [_Mode](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetDenoiser(static_cast<Smile::EDenoiser>(_Mode));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }

    void RenderSettingsBridge::SetUpscalerMode(int _Mode) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("upscaler-mode"),
            [_Mode](Smile::Renderer& _Renderer) {
                // 0=None 1=FSR 2=DLSS; cai p/ None se indisponivel.
                _Renderer.Settings().SetUpscaler(static_cast<Smile::EUpscaler>(_Mode));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }

    void RenderSettingsBridge::SetUpscalerQuality(int _Quality) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("upscaler-quality"),
            [_Quality](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetUpscalerQuality(_Quality); // qualidade compartilhada FSR/DLSS
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }

    void RenderSettingsBridge::SetRenderScale(double _Scale) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("render-scale"),
            [_Scale](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetRenderScale(static_cast<float>(_Scale));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }

    void RenderSettingsBridge::SetTAAEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseTAA(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::SetFrustumCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetFrustumCulling(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::SetOcclusionCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetOcclusionCulling(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::SetDepthPrepassEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetDepthPrepass(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ResetRenderSettings() {
        if (!Renderer) return;
        // O padrao segue o backend recomendado (DLSS em NVIDIA, senao FSR, senao nativo), mas em
        // escala 1:1 (tier 0 = 100%): reconstroi/faz AA sem upscale. Bate com o default do Renderer.
        Viewport->EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                const Smile::EUpscaler Recommended =
                    _Renderer.Settings().UpscalerAvailable(Smile::EUpscaler::DLSS)
                        ? Smile::EUpscaler::DLSS
                        : (_Renderer.Settings().UpscalerAvailable(Smile::EUpscaler::FSR)
                               ? Smile::EUpscaler::FSR
                               : Smile::EUpscaler::None);
                _Renderer.Settings().SetUpscalerQuality(0);
                _Renderer.Settings().SetUpscaler(Recommended);
                _Renderer.Settings().SetUseTAA(true);
                _Renderer.Settings().SetFrustumCulling(true);
                _Renderer.Settings().SetDepthPrepass(false);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyDebugTargetsChanged();
                }
            });
    }
}
