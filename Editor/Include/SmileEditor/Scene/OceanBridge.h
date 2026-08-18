#pragma once

#include <QObject>

#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    // API autoral do oceano para o Scene Outliner. A simulacao continua em FWaterRenderer;
    // esta ponte impede que o inspector dependa das configuracoes globais de render.
    class OceanBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(bool enabled READ IsEnabled NOTIFY SettingsChanged)
        Q_PROPERTY(double windDirectionDegrees READ WindDirectionDegrees NOTIFY SettingsChanged)
        Q_PROPERTY(double windSpeed READ WindSpeed NOTIFY SettingsChanged)
        Q_PROPERTY(double fetchKm READ FetchKm NOTIFY SettingsChanged)
        Q_PROPERTY(double depthM READ DepthM NOTIFY SettingsChanged)
        Q_PROPERTY(double swell READ Swell NOTIFY SettingsChanged)
        Q_PROPERTY(double wavesAmount READ WavesAmount NOTIFY SettingsChanged)
        Q_PROPERTY(double displacement READ Displacement NOTIFY SettingsChanged)
        Q_PROPERTY(double choppy READ Choppy NOTIFY SettingsChanged)

    public:
        explicit OceanBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle renderer);

        bool Available() const { return static_cast<bool>(Renderer); }
        bool IsEnabled() const;
        double WindDirectionDegrees() const;
        double WindSpeed() const;
        double FetchKm() const;
        double DepthM() const;
        double Swell() const;
        double WavesAmount() const;
        double Displacement() const;
        double Choppy() const;

        Q_INVOKABLE void SetWindDirectionDegrees(double degrees);
        Q_INVOKABLE void SetWindSpeed(double metresPerSecond);
        Q_INVOKABLE void SetFetchKm(double kilometres);
        Q_INVOKABLE void SetDepthM(double metres);
        Q_INVOKABLE void SetSwell(double value);
        Q_INVOKABLE void SetWavesAmount(double value);
        Q_INVOKABLE void SetDisplacement(double value);
        Q_INVOKABLE void SetChoppy(double value);

    signals:
        void AvailableChanged();
        void SettingsChanged();

    private:
        RendererHandle Renderer;
    };
}
