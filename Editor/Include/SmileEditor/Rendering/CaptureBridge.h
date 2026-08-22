#pragma once

#include <QObject>
#include <QString>
#include "SmileEditor/Viewport/RenderThread.h"

class QTimer;

namespace SmileEditor {
    // Adapta FFrameCapture para a UI. Aquecimento e captura continuam no renderer para contar
    // frames reais; pose e presets pertencem aos respectivos controladores.
    class CaptureBridge : public QObject {
        Q_OBJECT
        // Sessao em curso: a UI desabilita o disparo e mostra o aquecimento.
        Q_PROPERTY(bool    busy            READ Busy            NOTIFY ProgressChanged)
        Q_PROPERTY(int     warmupRemaining READ WarmupRemaining NOTIFY ProgressChanged)
        // Ultimo resultado, ja formatado (caminho do PNG ou o erro).
        Q_PROPERTY(QString lastResult      READ LastResult      NOTIFY ProgressChanged)

    public:
        explicit CaptureBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle R) { Renderer = R; emit ProgressChanged(); }
        // Nome da cena para o manifesto e para o nome do arquivo. Vem do MainWindow no load.
        void OnSceneLoaded(const QString& ScenePath, bool Additive);

        bool    Busy() const;
        int     WarmupRemaining() const;
        QString LastResult() const { return Result; }
        bool    Available() const;

        // Slot < 0 usa camera livre; PinHours < 0 preserva a hora atual.
        Q_INVOKABLE bool Shoot(int Slot, int WarmupFrames, bool Scientific, double PinHours);
        // Variante MCP que pode preservar o histórico vigente.
        bool ShootAdvanced(int Slot, int WarmupFrames, bool Scientific, double PinHours,
                           bool ResetHistory);
        // Negativo quando o renderer nao esta disponivel.
        Q_INVOKABLE double CurrentTimeOfDayHours() const;
        // Publica progresso somente quando o snapshot muda.
        Q_INVOKABLE void Poll();

    signals:
        void ProgressChanged();
        void Message(const QString& Text);
        // Emitido depois da publicacao atomica do PNG e manifesto.
        void Finished(bool Success, const QString& PngPath, const QString& ManifestPath,
                      const QString& Error, int WarmupFrames);

    private:
        RendererHandle Renderer;
        QString        SceneName;
        QString        Result;
        bool           LastBusy      = false;
        int            LastRemaining = 0;
        QTimer*        PollTimer     = nullptr;
    };
}
