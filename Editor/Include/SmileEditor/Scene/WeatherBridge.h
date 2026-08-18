#pragma once

#include <QObject>

#include "SmileEditor/Viewport/RenderThread.h"

namespace SmileEditor {
    // Estado meteorologico autoral da cena. FWeather continua dono da simulacao; esta ponte
    // separa chuva e wetness das configuracoes tecnicas de render.
    class WeatherBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(bool raining READ IsRaining NOTIFY SettingsChanged)
        Q_PROPERTY(double rainAmount READ RainAmount NOTIFY SettingsChanged)
        Q_PROPERTY(double curtainAmount READ CurtainAmount NOTIFY SettingsChanged)
        Q_PROPERTY(double puddleAmount READ PuddleAmount NOTIFY SettingsChanged)
        Q_PROPERTY(double puddleScale READ PuddleScale NOTIFY SettingsChanged)
        Q_PROPERTY(double rippleStrength READ RippleStrength NOTIFY SettingsChanged)
        Q_PROPERTY(double wetDarkening READ WetDarkening NOTIFY SettingsChanged)
        Q_PROPERTY(bool rainOcclusion READ IsRainOcclusionEnabled NOTIFY SettingsChanged)
        Q_PROPERTY(bool drivesSky READ DrivesSky NOTIFY SettingsChanged)

    public:
        explicit WeatherBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle renderer);

        bool Available() const { return static_cast<bool>(Renderer); }
        bool IsRaining() const;
        double RainAmount() const;
        double CurtainAmount() const;
        double PuddleAmount() const;
        double PuddleScale() const;
        double RippleStrength() const;
        double WetDarkening() const;
        bool IsRainOcclusionEnabled() const;
        bool DrivesSky() const;

        Q_INVOKABLE void SetRainAmount(double value);
        Q_INVOKABLE void SetCurtainAmount(double value);
        Q_INVOKABLE void SetPuddleAmount(double value);
        Q_INVOKABLE void SetPuddleScale(double value);
        Q_INVOKABLE void SetRippleStrength(double value);
        Q_INVOKABLE void SetWetDarkening(double value);
        Q_INVOKABLE void SetRainOcclusionEnabled(bool enabled);
        Q_INVOKABLE void SetDrivesSky(bool enabled);

    signals:
        void AvailableChanged();
        void SettingsChanged();

    private:
        RendererHandle Renderer;
    };
}
