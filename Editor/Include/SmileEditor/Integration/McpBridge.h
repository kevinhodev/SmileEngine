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

    signals:
        // MainWindow aprova o fechamento antes de fechar, evitando dialogos de sidecar durante
        // um ciclo automatizado de benchmark e preservando o shutdown ordenado do renderer.
        void ShutdownRequested();

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
        void Reply(QLocalSocket* Socket, const QString& Id, bool Ok,
                   const QJsonObject& Payload);

        CaptureBridge*        Capture   = nullptr;
        CameraBookmarksBridge* Bookmarks = nullptr;
        RenderSettingsController* RenderSettings = nullptr;
        QLocalServer*         Server    = nullptr;
        QHash<QLocalSocket*, QByteArray> Buffers;
        QPointer<QLocalSocket> ActiveCaptureSocket;
        QString               ActiveCaptureId;
        QString               PipeName;
        QString               ScenePath;
    };
}
