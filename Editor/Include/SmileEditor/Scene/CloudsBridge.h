#pragma once

#include <QObject>
#include <QPointer>

#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    class ViewportWidget;

    // API autoral das nuvens enquanto o estado ainda vive em FRenderSettings.
    class CloudsBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(bool enabled READ IsEnabled NOTIFY SettingsChanged)
        Q_PROPERTY(bool halfResolution READ IsHalfResolution NOTIFY SettingsChanged)
        Q_PROPERTY(bool temporal READ IsTemporal NOTIFY SettingsChanged)
        Q_PROPERTY(double coverage READ Coverage NOTIFY SettingsChanged)
        Q_PROPERTY(double density READ Density NOTIFY SettingsChanged)
        Q_PROPERTY(double windSpeed READ WindSpeed NOTIFY SettingsChanged)
        Q_PROPERTY(double erosion READ Erosion NOTIFY SettingsChanged)
        Q_PROPERTY(double phaseG READ PhaseG NOTIFY SettingsChanged)
        Q_PROPERTY(double powder READ Powder NOTIFY SettingsChanged)
        Q_PROPERTY(double ambient READ Ambient NOTIFY SettingsChanged)
        Q_PROPERTY(double typeBias READ TypeBias NOTIFY SettingsChanged)
        Q_PROPERTY(double peakVariation READ PeakVariation NOTIFY SettingsChanged)
        Q_PROPERTY(int weatherSeed READ WeatherSeed NOTIFY SettingsChanged)
        Q_PROPERTY(int weatherCells READ WeatherCells NOTIFY SettingsChanged)
        Q_PROPERTY(bool weatherAuthored READ IsWeatherAuthored NOTIFY SettingsChanged)
        Q_PROPERTY(bool shadowsEnabled READ AreShadowsEnabled NOTIFY SettingsChanged)
        Q_PROPERTY(double shadowStrength READ ShadowStrength NOTIFY SettingsChanged)
        Q_PROPERTY(double bottomKm READ BottomKm NOTIFY SettingsChanged)
        Q_PROPERTY(double thicknessKm READ ThicknessKm NOTIFY SettingsChanged)
        Q_PROPERTY(int marchSteps READ MarchSteps NOTIFY SettingsChanged)

    public:
        explicit CloudsBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle renderer);
        void SetViewport(ViewportWidget* viewport);

        bool Available() const { return static_cast<bool>(Renderer); }
        bool IsEnabled() const;
        bool IsHalfResolution() const;
        bool IsTemporal() const;
        double Coverage() const;
        double Density() const;
        double WindSpeed() const;
        double Erosion() const;
        double PhaseG() const;
        double Powder() const;
        double Ambient() const;
        double TypeBias() const;
        double PeakVariation() const;
        int WeatherSeed() const;
        int WeatherCells() const;
        bool IsWeatherAuthored() const;
        bool AreShadowsEnabled() const;
        double ShadowStrength() const;
        double BottomKm() const;
        double ThicknessKm() const;
        int MarchSteps() const;

        Q_INVOKABLE void SetEnabled(bool enabled);
        Q_INVOKABLE void SetHalfResolution(bool halfResolution);
        Q_INVOKABLE void SetTemporal(bool enabled);
        Q_INVOKABLE void SetCoverage(double value);
        Q_INVOKABLE void SetDensity(double value);
        Q_INVOKABLE void SetWindSpeed(double value);
        Q_INVOKABLE void SetErosion(double value);
        Q_INVOKABLE void SetPhaseG(double value);
        Q_INVOKABLE void SetPowder(double value);
        Q_INVOKABLE void SetAmbient(double value);
        Q_INVOKABLE void SetTypeBias(double value);
        Q_INVOKABLE void SetPeakVariation(double value);
        Q_INVOKABLE void SetWeatherSeed(int seed);
        Q_INVOKABLE void SetWeatherCells(int cells);
        Q_INVOKABLE void LoadWeatherTexture();
        Q_INVOKABLE void ClearWeatherTexture();
        Q_INVOKABLE void SetShadowsEnabled(bool enabled);
        Q_INVOKABLE void SetShadowStrength(double value);
        Q_INVOKABLE void SetAltitude(double bottomKm, double thicknessKm);
        Q_INVOKABLE void SetMarchSteps(int steps);

    signals:
        void AvailableChanged();
        void SettingsChanged();

    private:
        RendererHandle Renderer;
        QPointer<ViewportWidget> Viewport;
    };
}
