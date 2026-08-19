#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QJsonObject;
class QLocalServer;
class QLocalSocket;

namespace SmileEditor {
    class CameraBookmarksBridge;
    class CaptureBridge;
    class RenderSettingsController;

    // Canal local e pequeno entre o SmileMCP e o editor vivo. QLocalServer vira named pipe no
    // Windows; UserAccessOption restringe a conexao ao usuario que iniciou o editor. O protocolo
    // e JSON por linha e versionado, sem incorporar o SDK MCP ao executavel C++.
    class McpBridge final : public QObject {
        Q_OBJECT

    public:
        explicit McpBridge(CaptureBridge* Capture, CameraBookmarksBridge* Bookmarks,
                           RenderSettingsController* RenderSettings,
                           QObject* Parent = nullptr);
        ~McpBridge() override;

        void OnSceneLoaded(const QString& ScenePath, bool Additive);
        // Resultado da carga pedida por SceneLoadRequested. O MainWindow e quem sabe carregar
        // cena (thread de preparo, commit no renderer, pontes a notificar); o bridge so guarda
        // qual requisicao esta esperando e devolve a resposta pela pipe quando ela termina.
        void OnSceneLoadFinished(bool Success, const QString& Error);

    signals:
        // MainWindow aprova o fechamento antes de fechar, evitando dialogos de sidecar durante
        // um ciclo automatizado de benchmark e preservando o shutdown ordenado do renderer.
        void ShutdownRequested();

        // Carga de uma .sscene na sessao viva. Additive=true acrescenta a cena aberta em vez de
        // substitui-la — e o que faz uma malha recem-gerada aparecer no viewport sem reiniciar o
        // editor, preservando camera, hora do dia e o resto do estado.
        void SceneLoadRequested(const QString& Path, bool Additive);

    private slots:
        void OnNewConnection();
        void OnSocketReadyRead();
        void OnSocketDisconnected();
        void OnCaptureFinished(bool Success, const QString& PngPath,
                               const QString& ManifestPath, const QString& Error,
                               int WarmupFrames);

    private:
        void HandleRequest(QLocalSocket* Socket, const QByteArray& Line);
        void HandleCapture(QLocalSocket* Socket, const QString& Id,
                           const QJsonObject& Arguments);
        void HandleProfileConfigure(QLocalSocket* Socket, const QString& Id,
                                    const QJsonObject& Arguments);
        void HandleProfileSnapshot(QLocalSocket* Socket, const QString& Id);
        void HandleLoadScene(QLocalSocket* Socket, const QString& Id,
                             const QJsonObject& Arguments);
        void Reply(QLocalSocket* Socket, const QString& Id, bool Ok,
                   const QJsonObject& Payload);

        CaptureBridge*        Capture   = nullptr;
        CameraBookmarksBridge* Bookmarks = nullptr;
        RenderSettingsController* RenderSettings = nullptr;
        QLocalServer*         Server    = nullptr;
        QHash<QLocalSocket*, QByteArray> Buffers;
        QPointer<QLocalSocket> ActiveCaptureSocket;
        QString               ActiveCaptureId;
        QPointer<QLocalSocket> ActiveLoadSocket;
        QString               ActiveLoadId;
        QString               PipeName;
        QString               ScenePath;
    };
}
