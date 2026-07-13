#pragma once

#include <QObject>

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Ponte C++<->QML do painel Time of Day (TimeOfDayPanel.qml). Le/escreve o FTimeOfDay do
    // Renderer direto — seguro porque o render roda no timer do ViewportWidget, na mesma thread
    // da GUI. Alem do estado bruto, expoe leituras derivadas (elevacao/azimute de sol e lua,
    // fracao de fase) e SunElevationAt/MoonElevationAt p/ o painel desenhar o arco do dia.
    //
    // Sinais: StateChanged = qualquer knob mudou (setter ou hora escrutinada); TimeChanged = o
    // relogio andou (por frame, via Refresh no FrameReady) — separados p/ o QML nao reavaliar
    // os cards estaticos a cada frame.
    class TimeOfDayBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(bool enabled READ Enabled WRITE SetEnabled NOTIFY StateChanged)
        Q_PROPERTY(bool running READ Running WRITE SetRunning NOTIFY StateChanged)
        Q_PROPERTY(double timeHours READ TimeHours WRITE SetTimeHours NOTIFY TimeChanged)
        Q_PROPERTY(double dayLengthSec READ DayLengthSec WRITE SetDayLengthSec NOTIFY StateChanged)
        Q_PROPERTY(double latitudeDeg READ LatitudeDeg WRITE SetLatitudeDeg NOTIFY StateChanged)
        Q_PROPERTY(int dayOfYear READ DayOfYear WRITE SetDayOfYear NOTIFY StateChanged)
        Q_PROPERTY(double northOffsetDeg READ NorthOffsetDeg WRITE SetNorthOffsetDeg NOTIFY StateChanged)
        Q_PROPERTY(bool moonEnabled READ MoonEnabled WRITE SetMoonEnabled NOTIFY StateChanged)
        Q_PROPERTY(double moonPhaseOffsetHours READ MoonPhaseOffsetHours WRITE SetMoonPhaseOffsetHours NOTIFY StateChanged)
        Q_PROPERTY(double moonIntensity READ MoonIntensity WRITE SetMoonIntensity NOTIFY StateChanged)
        Q_PROPERTY(double moonDiskSize READ MoonDiskSize WRITE SetMoonDiskSize NOTIFY StateChanged)
        Q_PROPERTY(double moonDiskBrightness READ MoonDiskBrightness WRITE SetMoonDiskBrightness NOTIFY StateChanged)
        Q_PROPERTY(double starIntensity READ StarIntensity WRITE SetStarIntensity NOTIFY StateChanged)
        // Clima (FWeather do Renderer) — F1: chuva/wetness deferred.
        Q_PROPERTY(double rainAmount READ RainAmount WRITE SetRainAmount NOTIFY StateChanged)
        Q_PROPERTY(double puddleAmount READ PuddleAmount WRITE SetPuddleAmount NOTIFY StateChanged)
        Q_PROPERTY(double puddleScale READ PuddleScale WRITE SetPuddleScale NOTIFY StateChanged)
        Q_PROPERTY(double rippleStrength READ RippleStrength WRITE SetRippleStrength NOTIFY StateChanged)
        Q_PROPERTY(double wetDarkening READ WetDarkening WRITE SetWetDarkening NOTIFY StateChanged)
        Q_PROPERTY(double curtainAmount READ CurtainAmount WRITE SetCurtainAmount NOTIFY StateChanged)
        // Sol manual (TOD desligado): az/el em graus, escreve via Renderer::SetSunAzimuthElevation.
        Q_PROPERTY(double manualAzimuthDeg READ ManualAzimuthDeg WRITE SetManualAzimuthDeg NOTIFY StateChanged)
        Q_PROPERTY(double manualElevationDeg READ ManualElevationDeg WRITE SetManualElevationDeg NOTIFY StateChanged)
        // Derivados (leitura, seguem o relogio).
        Q_PROPERTY(double sunElevationDeg READ SunElevationDeg NOTIFY TimeChanged)
        Q_PROPERTY(double sunAzimuthDeg READ SunAzimuthDeg NOTIFY TimeChanged)
        Q_PROPERTY(double moonElevationDeg READ MoonElevationDeg NOTIFY TimeChanged)
        Q_PROPERTY(double moonPhaseFraction READ MoonPhaseFraction NOTIFY StateChanged)

    public:
        explicit TimeOfDayBridge(QObject* parent = nullptr);

        void SetRenderer(Smile::Renderer* R); // MainWindow chama no RendererInitialized

        bool   Available() const { return Renderer != nullptr; }
        bool   Enabled() const;
        bool   Running() const;
        double TimeHours() const;
        double DayLengthSec() const;
        double LatitudeDeg() const;
        int    DayOfYear() const;
        double NorthOffsetDeg() const;
        bool   MoonEnabled() const;
        double MoonPhaseOffsetHours() const;
        double MoonIntensity() const;
        double MoonDiskSize() const;
        double MoonDiskBrightness() const;
        double StarIntensity() const;
        double RainAmount() const;
        double PuddleAmount() const;
        double PuddleScale() const;
        double RippleStrength() const;
        double WetDarkening() const;
        double CurtainAmount() const;
        double ManualAzimuthDeg() const  { return ManualAz; }
        double ManualElevationDeg() const { return ManualEl; }
        double SunElevationDeg() const;
        double SunAzimuthDeg() const;
        double MoonElevationDeg() const;
        double MoonPhaseFraction() const;

        void SetEnabled(bool V);
        void SetRunning(bool V);
        void SetTimeHours(double V);
        void SetDayLengthSec(double V);
        void SetLatitudeDeg(double V);
        void SetDayOfYear(int V);
        void SetNorthOffsetDeg(double V);
        void SetMoonEnabled(bool V);
        void SetMoonPhaseOffsetHours(double V);
        void SetMoonIntensity(double V);
        void SetMoonDiskSize(double V);
        void SetMoonDiskBrightness(double V);
        void SetStarIntensity(double V);
        void SetRainAmount(double V);
        void SetPuddleAmount(double V);
        void SetPuddleScale(double V);
        void SetRippleStrength(double V);
        void SetWetDarkening(double V);
        void SetCurtainAmount(double V);
        void SetManualAzimuthDeg(double V);
        void SetManualElevationDeg(double V);

        // Elevacao (graus) de sol/lua numa hora arbitraria do dia corrente — curva do arco do ceu.
        Q_INVOKABLE double sunElevationAt(double hours) const;
        Q_INVOKABLE double moonElevationAt(double hours) const;
        Q_INVOKABLE void   closePanel() { emit CloseRequested(); }

    public slots:
        void Refresh(); // FrameReady: emite TimeChanged se o relogio andou desde o ultimo frame

    signals:
        void AvailableChanged();
        void StateChanged();
        void TimeChanged();
        void CloseRequested(); // botao X do painel -> MainWindow fecha o dock

    private:
        void SyncManualFromRenderer(); // az/el manuais a partir do SunDir atual do Renderer

        Smile::Renderer* Renderer = nullptr;
        double ManualAz = 60.0;  // defaults ate o renderer existir
        double ManualEl = 35.0;
        double LastEmittedHours = -1.0;
    };
}
