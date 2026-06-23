#pragma once

#include <QObject>

namespace SmileEditor {
    // Ponte C++<->QML dos menus (EditorMenuBar.qml). A QML chama os slots; cada um emite o sinal
    // correspondente, que o MainWindow conecta a logica real (dialogs, setters do Renderer, etc.).
    // Mantem os menus desacoplados do MainWindow, no mesmo espirito de LogBridge/WindowBridge.
    class MenuBridge : public QObject {
        Q_OBJECT
        // Estado do dock do Console refletido no check do menu "Janela".
        Q_PROPERTY(bool consoleVisible READ ConsoleVisible NOTIFY ConsoleVisibleChanged)

    public:
        explicit MenuBridge(QObject* parent = nullptr);

        bool ConsoleVisible() const { return ConsoleVis; }
        void SetConsoleVisible(bool v); // MainWindow chama (visibilityChanged do dock)

    public slots:
        void loadScene()        { emit LoadSceneRequested(); }
        void addScene()         { emit AddSceneRequested(); }
        void openEnvironment()  { emit OpenEnvironmentRequested(); }
        void about()            { emit AboutRequested(); }
        void quit()             { emit QuitRequested(); }
        void toggleConsole()    { emit ToggleConsoleRequested(); }
        void setVSync(bool on)            { emit VSyncToggled(on); }
        void setFrustumCulling(bool on)   { emit FrustumCullingToggled(on); }
        void setDepthPrepass(bool on)     { emit DepthPrepassToggled(on); }
        void setMergeByMaterial(bool on)  { emit MergeByMaterialToggled(on); }

    signals:
        void LoadSceneRequested();
        void AddSceneRequested();
        void OpenEnvironmentRequested();
        void AboutRequested();
        void QuitRequested();
        void ToggleConsoleRequested();
        void VSyncToggled(bool on);
        void FrustumCullingToggled(bool on);
        void DepthPrepassToggled(bool on);
        void MergeByMaterialToggled(bool on);
        void ConsoleVisibleChanged();

    private:
        bool ConsoleVis = true;
    };
}
