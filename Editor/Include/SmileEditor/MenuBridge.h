#pragma once

#include <QObject>

namespace SmileEditor {
    // Ponte C++<->QML dos menus (EditorMenuBar.qml). A QML chama os slots; cada um emite o sinal
    // correspondente, que o MainWindow conecta a logica real (dialogs, setters do Renderer, etc.).
    // Mantem os menus desacoplados do MainWindow, no mesmo espirito de LogBridge/WindowBridge.
    class MenuBridge : public QObject {
        Q_OBJECT
        // Estado dos docks refletido nos checks do menu "Janela".
        Q_PROPERTY(bool consoleVisible READ ConsoleVisible NOTIFY ConsoleVisibleChanged)
        Q_PROPERTY(bool timeOfDayVisible READ TimeOfDayVisible NOTIFY TimeOfDayVisibleChanged)
        Q_PROPERTY(bool lightsVisible READ LightsVisible NOTIFY LightsVisibleChanged)
        Q_PROPERTY(bool statsVisible READ StatsVisible NOTIFY StatsVisibleChanged)
        Q_PROPERTY(bool materialsVisible READ MaterialsVisible NOTIFY MaterialsVisibleChanged)

    public:
        explicit MenuBridge(QObject* parent = nullptr);

        bool ConsoleVisible() const { return ConsoleVis; }
        void SetConsoleVisible(bool v); // MainWindow chama (visibilityChanged do dock)
        bool TimeOfDayVisible() const { return TimeOfDayVis; }
        void SetTimeOfDayVisible(bool v);
        bool LightsVisible() const { return LightsVis; }
        void SetLightsVisible(bool v);
        bool StatsVisible() const { return StatsVis; }
        void SetStatsVisible(bool v);
        bool MaterialsVisible() const { return MaterialsVis; }
        void SetMaterialsVisible(bool v);

    public slots:
        void loadScene()        { emit LoadSceneRequested(); }
        void addScene()         { emit AddSceneRequested(); }
        void about()            { emit AboutRequested(); }
        void openSettings()     { emit SettingsRequested(); }
        void quit()             { emit QuitRequested(); }
        void toggleConsole()    { emit ToggleConsoleRequested(); }
        void toggleTimeOfDay()  { emit ToggleTimeOfDayRequested(); }
        void toggleLights()     { emit ToggleLightsRequested(); }
        void toggleStats()      { emit ToggleStatsRequested(); }
        void toggleMaterials()  { emit ToggleMaterialsRequested(); }
        void setVSync(bool on)            { emit VSyncToggled(on); }
        void setFrustumCulling(bool on)   { emit FrustumCullingToggled(on); }
        void setDepthPrepass(bool on)     { emit DepthPrepassToggled(on); }
        void setMergeByMaterial(bool on)  { emit MergeByMaterialToggled(on); }

    signals:
        void LoadSceneRequested();
        void AddSceneRequested();
        void AboutRequested();
        void SettingsRequested();
        void QuitRequested();
        void ToggleConsoleRequested();
        void ToggleTimeOfDayRequested();
        void ToggleLightsRequested();
        void ToggleStatsRequested();
        void ToggleMaterialsRequested();
        void MaterialsVisibleChanged();
        void TimeOfDayVisibleChanged();
        void LightsVisibleChanged();
        void StatsVisibleChanged();
        void VSyncToggled(bool on);
        void FrustumCullingToggled(bool on);
        void DepthPrepassToggled(bool on);
        void MergeByMaterialToggled(bool on);
        void ConsoleVisibleChanged();

    private:
        bool ConsoleVis   = true;
        bool TimeOfDayVis = false; // TOD agora e janela flutuante: comeca fechada
        bool LightsVis    = true;
        bool StatsVis     = false; // Estatisticas: janela flutuante, comeca fechada
        bool MaterialsVis = false; // Editor de Materiais: janela flutuante, comeca fechada
    };
}
