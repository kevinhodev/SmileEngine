#pragma once

#include <QObject>
#include <QString>
#include "SmileEditor/Viewport/RenderThread.h"

class QTimer;

namespace SmileEditor {
    // ============================================================================================
    // Disparo da captura deterministica (Docs/CAPTURE-PROTOCOL.md).
    //
    // A bridge NAO implementa o protocolo — ele mora no motor (FFrameCapture), porque os passos
    // que importam sao "aquecer N frames RENDERIZADOS" e "capturar o frame seguinte", e a GUI nao
    // tem como saber quantos frames foram renderizados sem contar tique de timer, que e
    // exatamente o erro que o protocolo proibe: um tique que nao virou frame nao converge nada, e
    // contar tique tornaria o N funcao da carga da maquina.
    //
    // Entao aqui so ha: enfileirar o pedido, e devolver progresso/resultado para a UI.
    //
    // A POSE E DE QUEM CHAMA. A ordem do protocolo e preset -> camera -> reset, e restaurar a pose
    // e do CameraBookmarksBridge (quem tem os slots). O QML compoe as duas na ordem certa; sao dois
    // Q_INVOKABLE em sequencia, cada um pegando o lock do RendererHandle, e a sessao so comeca no
    // proximo RenderFrame — depois das duas.
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

        // Enfileira. Slot < 0 = camera livre (o manifesto registra -1). Scientific = resolucao
        // nativa, sem upscaler e sem TAA. PinHours >= 0 fixa a hora do dia da sessao.
        Q_INVOKABLE bool Shoot(int Slot, int WarmupFrames, bool Scientific, double PinHours);
        // Variante MCP que pode preservar o histórico vigente.
        bool ShootAdvanced(int Slot, int WarmupFrames, bool Scientific, double PinHours,
                           bool ResetHistory);
        // Hora do dia corrente, para a UI oferecer "usar a hora atual" sem o operador ter de
        // ler o painel de TOD e digitar. Negativo se o renderer nao estiver disponivel.
        Q_INVOKABLE double CurrentTimeOfDayHours() const;
        // A sessao vive na render thread; a UI pergunta enquanto o card estiver visivel. Emite
        // ProgressChanged so quando algo mudou — um Timer a 200 ms nao deve provocar relayout a
        // cada disparo.
        Q_INVOKABLE void Poll();

    signals:
        void ProgressChanged();
        void Message(const QString& Text);
        // Resultado consumido do renderer. Alem da UI, o bridge MCP usa este sinal para manter a
        // named pipe aberta ate PNG + manifesto terem sido publicados de forma atomica.
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
