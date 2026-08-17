#include "SmileEditor/McpBridge.h"
#include "SmileEditor/CameraBookmarksBridge.h"
#include "SmileEditor/CaptureBridge.h"
#include "SmileEditor/ViewportWidget.h"
#include "Smile/Core/Logger.h"
#include "Smile/Core/VersionInfo.h"
#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Graphics/Renderer.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include <cmath>

namespace SmileEditor {
    namespace {
        constexpr int kProtocolVersion = 1;
        constexpr qsizetype kMaxRequestBytes = 64 * 1024;

        bool ReadInteger(const QJsonObject& _Object, const char* _Name,
                         int _Default, int _Min, int _Max, int& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (Value.isUndefined()) {
                _Out = _Default;
                return true;
            }
            if (!Value.isDouble()) return false;
            const double Number = Value.toDouble();
            if (!std::isfinite(Number) || std::floor(Number) != Number ||
                Number < static_cast<double>(_Min) || Number > static_cast<double>(_Max))
                return false;
            _Out = static_cast<int>(Number);
            return true;
        }

        QJsonArray SerializeTimings(
            const std::vector<Smile::FGpuProfiler::FScopeResult>& _Results,
            const QString& _Queue) {
            QJsonArray Out;
            for (const auto& R : _Results) {
                Out.append(QJsonObject{
                    { QStringLiteral("name"), QString::fromUtf8(R.Name ? R.Name : "") },
                    { QStringLiteral("queue"), _Queue },
                    { QStringLiteral("depth"), static_cast<int>(R.Depth) },
                    { QStringLiteral("milliseconds"), R.Milliseconds },
                    { QStringLiteral("rawMilliseconds"), R.RawMilliseconds },
                });
            }
            return Out;
        }
    }

    McpBridge::McpBridge(CaptureBridge* _Capture, CameraBookmarksBridge* _Bookmarks,
                         QObject* _Parent)
        : QObject(_Parent), Capture(_Capture), Bookmarks(_Bookmarks),
          Server(new QLocalServer(this)),
          PipeName(qEnvironmentVariable("SMILE_MCP_PIPE", "SmileEngine-MCP-v1"))
    {
        Server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(Server, &QLocalServer::newConnection, this, &McpBridge::OnNewConnection);
        connect(Capture, &CaptureBridge::Finished, this, &McpBridge::OnCaptureFinished);

        if (!Server->listen(PipeName)) {
            Smile::LogWarning("SmileMCP bridge indisponivel em '" + PipeName.toStdString() +
                              "': " + Server->errorString().toStdString());
            return;
        }
        Smile::LogInfo("SmileMCP bridge ativo: " + PipeName.toStdString());
    }

    McpBridge::~McpBridge() {
        if (Server) Server->close();
    }

    void McpBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        if (!_Additive) ScenePath = QFileInfo(_ScenePath).absoluteFilePath();
    }

    void McpBridge::OnNewConnection() {
        while (QLocalSocket* Socket = Server->nextPendingConnection()) {
            Buffers.insert(Socket, {});
            connect(Socket, &QLocalSocket::readyRead, this, &McpBridge::OnSocketReadyRead);
            connect(Socket, &QLocalSocket::disconnected, this, &McpBridge::OnSocketDisconnected);
        }
    }

    void McpBridge::OnSocketReadyRead() {
        auto* Socket = qobject_cast<QLocalSocket*>(sender());
        if (!Socket) return;

        QByteArray& Buffer = Buffers[Socket];
        Buffer += Socket->readAll();
        if (Buffer.size() > kMaxRequestBytes) {
            Reply(Socket, {}, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("request excede 64 KiB") } });
            return;
        }

        qsizetype Newline = -1;
        while ((Newline = Buffer.indexOf('\n')) >= 0) {
            const QByteArray Line = Buffer.left(Newline).trimmed();
            Buffer.remove(0, Newline + 1);
            if (!Line.isEmpty()) HandleRequest(Socket, Line);
            if (Socket == ActiveCaptureSocket) break;
        }
    }

    void McpBridge::OnSocketDisconnected() {
        auto* Socket = qobject_cast<QLocalSocket*>(sender());
        if (!Socket) return;
        Buffers.remove(Socket);
        if (Socket == ActiveCaptureSocket) ActiveCaptureSocket.clear();
        Socket->deleteLater();
    }

    void McpBridge::HandleRequest(QLocalSocket* _Socket, const QByteArray& _Line) {
        QJsonParseError ParseError{};
        const QJsonDocument Document = QJsonDocument::fromJson(_Line, &ParseError);
        if (ParseError.error != QJsonParseError::NoError || !Document.isObject()) {
            Reply(_Socket, {}, false,
                  QJsonObject{ { QStringLiteral("error"), QStringLiteral("JSON invalido") } });
            return;
        }

        const QJsonObject Request = Document.object();
        const QString Id = Request.value(QStringLiteral("id")).toString();
        if (Id.isEmpty() || Id.size() > 80) {
            Reply(_Socket, {}, false,
                  QJsonObject{ { QStringLiteral("error"), QStringLiteral("id invalido") } });
            return;
        }
        if (Request.value(QStringLiteral("version")).toInt(0) != kProtocolVersion) {
            Reply(_Socket, Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("versao de protocolo incompativel") } });
            return;
        }

        const QString Command = Request.value(QStringLiteral("command")).toString();
        if (Command == QStringLiteral("status")) {
            // CaptureBridge::Available usa TryLock para nunca engasgar um binding QML. Para a
            // prontidao do protocolo isso e a semantica errada: em uma cena pesada, a amostragem
            // a cada 250 ms pode cair sempre dentro do frame e reportar false para sempre. A
            // consulta MCP pode esperar o frame soltar o lock; o resultado passa a ser estado,
            // nao uma foto da contencao naquele microssegundo.
            bool Ready = false;
            if (Viewport && Viewport->GetRenderer()) {
                auto Access = Viewport->GetRenderer().Lock();
                Ready = Access && Access->IsInitialized();
            } else {
                Ready = Capture && Capture->Available();
            }
            QJsonObject Result{
                { QStringLiteral("protocolVersion"), kProtocolVersion },
                { QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid()) },
                { QStringLiteral("executablePath"), QCoreApplication::applicationFilePath() },
                { QStringLiteral("buildCommit"), QString::fromUtf8(SMILE_BUILD_COMMIT) },
                { QStringLiteral("engineVersion"), QString::fromUtf8(SMILE_VERSION_STRING) },
                { QStringLiteral("ready"), Ready },
                { QStringLiteral("captureBusy"), Capture && Capture->Busy() },
                { QStringLiteral("scenePath"), ScenePath },
            };
            Reply(_Socket, Id, true, QJsonObject{ { QStringLiteral("result"), Result } });
            return;
        }
        if (Command == QStringLiteral("capture_frame")) {
            const QJsonValue Arguments = Request.value(QStringLiteral("arguments"));
            if (!Arguments.isObject()) {
                Reply(_Socket, Id, false,
                      QJsonObject{ { QStringLiteral("error"),
                                    QStringLiteral("arguments precisa ser objeto") } });
                return;
            }
            HandleCapture(_Socket, Id, Arguments.toObject());
            return;
        }
        if (Command == QStringLiteral("profile_configure")) {
            const QJsonValue Arguments = Request.value(QStringLiteral("arguments"));
            if (!Arguments.isObject()) {
                Reply(_Socket, Id, false,
                      QJsonObject{ { QStringLiteral("error"),
                                    QStringLiteral("arguments precisa ser objeto") } });
                return;
            }
            HandleProfileConfigure(_Socket, Id, Arguments.toObject());
            return;
        }
        if (Command == QStringLiteral("profile_snapshot")) {
            HandleProfileSnapshot(_Socket, Id);
            return;
        }
        if (Command == QStringLiteral("shutdown")) {
            if (Capture && Capture->Busy()) {
                Reply(_Socket, Id, false,
                      QJsonObject{ { QStringLiteral("error"),
                                    QStringLiteral("captura em andamento") } });
                return;
            }
            Reply(_Socket, Id, true,
                  QJsonObject{ { QStringLiteral("result"),
                                QJsonObject{ { QStringLiteral("accepted"), true } } } });
            QTimer::singleShot(0, this, [this]() { emit ShutdownRequested(); });
            return;
        }

        Reply(_Socket, Id, false,
              QJsonObject{ { QStringLiteral("error"),
                            QStringLiteral("comando desconhecido: %1").arg(Command) } });
    }

    void McpBridge::HandleProfileConfigure(QLocalSocket* _Socket, const QString& _Id,
                                           const QJsonObject& _Arguments) {
        if (!Viewport || !Viewport->GetRenderer()) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("renderer ainda nao esta pronto") } });
            return;
        }

        const QString Preset = _Arguments.value(QStringLiteral("preset"))
                                   .toString(QStringLiteral("gameplay_rr"));
        if (Preset != QStringLiteral("gameplay_rr") &&
            Preset != QStringLiteral("controlled_native")) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("preset de perfil invalido") } });
            return;
        }

        int Slot = -1;
        if (!ReadInteger(_Arguments, "bookmarkSlot", -1, -1, 3, Slot)) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("bookmarkSlot invalido") } });
            return;
        }
        if (Slot >= 0 && (!Bookmarks || !Bookmarks->Restore(Slot))) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("bookmark solicitado nao existe") } });
            return;
        }

        auto Access = Viewport->GetRenderer().Lock();
        if (!Access || !Access->IsInitialized()) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("renderer ainda nao esta inicializado") } });
            return;
        }

        // Regime fixo para o A/B: o que muda entre processos e somente o bytecode do acesso a
        // geometria. O cache entra ativo de imediato (sem borda de auto-warmup no meio da serie),
        // sol e hora ficam congelados, e todos os consumidores do hit path ficam ligados.
        auto& Settings = Access->Settings();
        Settings.SetRadianceCacheAutoWarmup(false);
        Settings.SetUseGI(true);
        Settings.SetRadianceCacheEnabled(true);
        Settings.SetRadianceCacheQuery(true);
        Settings.SetUseReSTIRGI(true);
        Settings.SetUseReSTIRDI(true);
        Settings.SetUseReflections(true);
        Settings.SetUseAO(true);
        Settings.SetIndirectPrimary(Smile::EIndirectPrimary::ReSTIR_SHaRC);

        if (Preset == QStringLiteral("gameplay_rr")) {
            Settings.SetUpscalerQuality(0);
            Settings.SetDenoiser(Smile::EDenoiser::DLSS_RR);
        } else {
            Settings.SetDenoiser(Smile::EDenoiser::None);
            Settings.SetUpscaler(Smile::EUpscaler::None);
            Settings.SetRenderScale(1.0f);
        }

        auto& Tod = Access->GetTimeOfDay();
        Tod.Enabled   = true;
        Tod.Running   = false;
        Tod.TimeHours = 10.0f;

        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"), QJsonObject{
                  { QStringLiteral("preset"), Preset },
                  { QStringLiteral("bookmarkSlot"), Slot },
                  { QStringLiteral("timeOfDayHours"), 10.0 },
              } } });
    }

    void McpBridge::HandleProfileSnapshot(QLocalSocket* _Socket, const QString& _Id) {
        if (!Viewport || !Viewport->GetRenderer()) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("renderer ainda nao esta pronto") } });
            return;
        }
        auto Access = Viewport->GetRenderer().Lock();
        if (!Access || !Access->IsInitialized()) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("renderer ainda nao esta inicializado") } });
            return;
        }

        const auto& Settings = Access->Settings();
        const auto& VM = Access->GetDevice().QueryVideoMemory();
        QJsonObject Result{
            { QStringLiteral("frameIndex"), static_cast<double>(Access->GetFrameIndex()) },
            { QStringLiteral("cpuFps"), Viewport->GetFPS() },
            { QStringLiteral("outputWidth"), static_cast<int>(Access->OutputWidth()) },
            { QStringLiteral("outputHeight"), static_cast<int>(Access->OutputHeight()) },
            { QStringLiteral("renderWidth"), static_cast<int>(Access->RenderWidth()) },
            { QStringLiteral("renderHeight"), static_cast<int>(Access->RenderHeight()) },
            { QStringLiteral("gpu"), QString::fromStdWString(
                  Access->GetDevice().GetAdapterDescription()) },
            { QStringLiteral("direct"), SerializeTimings(
                  Access->GetGpuProfiler().Results(), QStringLiteral("direct")) },
            { QStringLiteral("asyncCompute"), SerializeTimings(
                  Access->GetAsyncComputeTimings(), QStringLiteral("asyncCompute")) },
            { QStringLiteral("vram"), QJsonObject{
                  { QStringLiteral("valid"), VM.Valid },
                  { QStringLiteral("localUsageBytes"), static_cast<double>(VM.LocalUsage) },
                  { QStringLiteral("localBudgetBytes"), static_cast<double>(VM.LocalBudget) },
                  { QStringLiteral("nonLocalUsageBytes"), static_cast<double>(VM.NonLocalUsage) },
              } },
            { QStringLiteral("settings"), QJsonObject{
                  { QStringLiteral("ddgi"), Settings.GetUseGI() },
                  { QStringLiteral("restirGI"), Settings.GetUseReSTIRGI() },
                  { QStringLiteral("restirDI"), Settings.GetUseReSTIRDI() },
                  { QStringLiteral("radianceCache"), Settings.GetRadianceCacheEnabled() },
                  { QStringLiteral("cacheQuery"), Settings.GetRadianceCacheQuery() },
                  { QStringLiteral("reflections"), Settings.GetUseReflections() },
                  { QStringLiteral("gtao"), Settings.GetUseAO() },
                  { QStringLiteral("indirectPrimaryRequested"),
                    static_cast<int>(Settings.GetIndirectPrimary()) },
                  { QStringLiteral("indirectPrimaryEffective"),
                    static_cast<int>(Settings.EffectiveIndirectPrimary()) },
                  { QStringLiteral("denoiser"), static_cast<int>(Settings.GetDenoiser()) },
                  { QStringLiteral("upscaler"), static_cast<int>(Settings.GetUpscaler()) },
                  { QStringLiteral("upscalerQuality"), Settings.GetUpscalerQuality() },
                  { QStringLiteral("renderScale"), Settings.GetRenderScale() },
                  { QStringLiteral("timeOfDayHours"), Access->GetTimeOfDay().TimeHours },
                  { QStringLiteral("timeOfDayRunning"), Access->GetTimeOfDay().Running },
              } },
        };
        Reply(_Socket, _Id, true, QJsonObject{ { QStringLiteral("result"), Result } });
    }

    void McpBridge::HandleCapture(QLocalSocket* _Socket, const QString& _Id,
                                  const QJsonObject& _Arguments) {
        bool Ready = false;
        if (Viewport && Viewport->GetRenderer()) {
            auto Access = Viewport->GetRenderer().Lock();
            Ready = Access && Access->IsInitialized();
        } else {
            Ready = Capture && Capture->Available();
        }
        if (!Capture || !Ready) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("renderer ainda nao esta pronto") } });
            return;
        }
        if (Capture->Busy()) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("ja existe uma captura em andamento") } });
            return;
        }

        int Slot = -1;
        int Warmup = 128;
        if (!ReadInteger(_Arguments, "bookmarkSlot", -1, -1, 3, Slot) ||
            !ReadInteger(_Arguments, "warmupFrames", 128, 0, 512, Warmup)) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("bookmarkSlot ou warmupFrames invalido") } });
            return;
        }

        const QString Preset = _Arguments.value(QStringLiteral("preset"))
                                   .toString(QStringLiteral("scientific"));
        if (Preset != QStringLiteral("scientific") && Preset != QStringLiteral("gameplay")) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), QStringLiteral("preset invalido") } });
            return;
        }

        double PinHours = -1.0;
        const bool PinTime = _Arguments.value(QStringLiteral("pinTimeOfDay")).toBool(true);
        if (PinTime) {
            const QJsonValue TimeValue = _Arguments.value(QStringLiteral("timeOfDayHours"));
            if (TimeValue.isUndefined()) {
                PinHours = Capture->CurrentTimeOfDayHours();
            } else if (!TimeValue.isDouble() || !std::isfinite(TimeValue.toDouble()) ||
                       TimeValue.toDouble() < 0.0 || TimeValue.toDouble() >= 24.0) {
                Reply(_Socket, _Id, false,
                      QJsonObject{ { QStringLiteral("error"),
                                    QStringLiteral("timeOfDayHours precisa estar em [0, 24)") } });
                return;
            } else {
                PinHours = TimeValue.toDouble();
            }
        }

        if (Slot >= 0 && (!Bookmarks || !Bookmarks->Restore(Slot))) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("bookmark solicitado nao existe ou nao pode ser restaurado") } });
            return;
        }

        if (!Capture->Shoot(Slot, Warmup, Preset == QStringLiteral("scientific"), PinHours)) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("pedido de captura foi recusado") } });
            return;
        }

        ActiveCaptureSocket = _Socket;
        ActiveCaptureId = _Id;
    }

    void McpBridge::OnCaptureFinished(bool _Success, const QString& _PngPath,
                                      const QString& _ManifestPath, const QString& _Error,
                                      int _WarmupFrames) {
        if (ActiveCaptureId.isEmpty()) return; // captura disparada pela UI, nao pelo MCP

        QJsonObject Payload;
        if (_Success) {
            Payload.insert(QStringLiteral("result"), QJsonObject{
                { QStringLiteral("pngPath"), _PngPath },
                { QStringLiteral("manifestPath"), _ManifestPath },
                { QStringLiteral("warmupFrames"), _WarmupFrames },
            });
        } else {
            Payload.insert(QStringLiteral("error"), _Error.isEmpty()
                ? QStringLiteral("captura falhou sem detalhe") : _Error);
        }

        if (ActiveCaptureSocket)
            Reply(ActiveCaptureSocket, ActiveCaptureId, _Success, Payload);
        ActiveCaptureSocket.clear();
        ActiveCaptureId.clear();
    }

    void McpBridge::Reply(QLocalSocket* _Socket, const QString& _Id, bool _Ok,
                          const QJsonObject& _Payload) {
        if (!_Socket || _Socket->state() != QLocalSocket::ConnectedState) return;
        QJsonObject Response = _Payload;
        Response.insert(QStringLiteral("version"), kProtocolVersion);
        Response.insert(QStringLiteral("id"), _Id);
        Response.insert(QStringLiteral("ok"), _Ok);
        _Socket->write(QJsonDocument(Response).toJson(QJsonDocument::Compact));
        _Socket->write("\n");
        _Socket->flush();
        _Socket->disconnectFromServer();
    }
}
