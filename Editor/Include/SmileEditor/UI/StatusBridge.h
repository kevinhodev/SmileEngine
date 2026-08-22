#pragma once

#include <QObject>
#include <QString>

class QTimer;

namespace SmileEditor {
    // Snapshot da barra de status e mensagem transiente.
    class StatusBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(double  fps       READ Fps       NOTIFY StatsChanged)
        Q_PROPERTY(double  frameMs   READ FrameMs   NOTIFY StatsChanged)
        Q_PROPERTY(QString sceneText READ SceneText NOTIFY StatsChanged)
        Q_PROPERTY(QString oceanText READ OceanText NOTIFY StatsChanged)
        Q_PROPERTY(QString message   READ Message   NOTIFY MessageChanged)

    public:
        explicit StatusBridge(QObject* parent = nullptr);

        double  Fps()       const { return Fps_; }
        double  FrameMs()   const { return FrameMs_; }
        QString SceneText() const { return Scene_; }
        QString OceanText() const { return Ocean_; }
        QString Message()   const { return Message_; }

        // Chamado por frame pelo MainWindow::UpdateStats (textos sem separador; a QML os junta).
        void SetStats(double fps, double frameMs, const QString& scene, const QString& ocean);
        // Mensagem transiente; timeoutMs > 0 volta a "Pronto" depois (0 = persistente).
        void ShowMessage(const QString& text, int timeoutMs = 0);

    signals:
        void StatsChanged();
        void MessageChanged();

    private:
        double  Fps_     = 0.0;
        double  FrameMs_ = 0.0;
        QString Scene_;
        QString Ocean_;
        QString Message_;
        QTimer* MsgTimer = nullptr;
    };
}
