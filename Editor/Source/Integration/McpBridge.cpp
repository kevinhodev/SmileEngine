#include "SmileEditor/Integration/McpBridge.h"
#include "SmileEditor/Scene/CameraBookmarksBridge.h"
#include "SmileEditor/Rendering/CaptureBridge.h"
#include "SmileEditor/Rendering/RenderSettingsController.h"
#include "Smile/Core/Logger.h"
#include "Smile/Core/VersionInfo.h"
#include "Smile/Graphics/GI/DDGI.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include <cmath>
#include <optional>

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

        // Variantes OPCIONAIS: ausente devolve nullopt, e nao um default. E a diferenca entre
        // "preserve o que esta la" e "escreva o default" — com a segunda, cada etapa do sweep teria
        // de repetir todos os knobs, ou reescreveria calada o eixo que nao estava em teste.
        // `null` explicito conta como ausente, para um cliente poder omitir um campo montando o
        // objeto dinamicamente.
        bool ReadOptionalInteger(const QJsonObject& _Object, const char* _Name,
                                 int _Min, int _Max, std::optional<int>& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (Value.isUndefined() || Value.isNull()) { _Out.reset(); return true; }
            if (!Value.isDouble()) return false;
            const double Number = Value.toDouble();
            if (!std::isfinite(Number) || std::floor(Number) != Number ||
                Number < static_cast<double>(_Min) || Number > static_cast<double>(_Max))
                return false;
            _Out = static_cast<int>(Number);
            return true;
        }

        bool ReadOptionalNumberHalfOpen(const QJsonObject& _Object, const char* _Name,
                                        double _MinInclusive, double _MaxExclusive,
                                        std::optional<double>& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (Value.isUndefined() || Value.isNull()) { _Out.reset(); return true; }
            if (!Value.isDouble()) return false;
            const double Number = Value.toDouble();
            if (!std::isfinite(Number) || Number < _MinInclusive || Number >= _MaxExclusive)
                return false;
            _Out = Number;
            return true;
        }

        bool ReadOptionalBool(const QJsonObject& _Object, const char* _Name,
                              std::optional<bool>& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (Value.isUndefined() || Value.isNull()) { _Out.reset(); return true; }
            if (!Value.isBool()) return false;
            _Out = Value.toBool();
            return true;
        }

        bool ReadNumberClosed(const QJsonObject& _Object, const char* _Name,
                              double _Min, double _Max, double& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (!Value.isDouble()) return false;
            const double Number = Value.toDouble();
            if (!std::isfinite(Number) || Number < _Min || Number > _Max) return false;
            _Out = Number;
            return true;
        }

        bool ReadOptionalIndirectPrimary(
            const QJsonObject& _Object, const char* _Name,
            std::optional<Smile::EIndirectPrimary>& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (Value.isUndefined() || Value.isNull()) { _Out.reset(); return true; }
            if (!Value.isString()) return false;
            const QString Name = Value.toString();
            if (Name == QStringLiteral("restir_sharc"))
                _Out = Smile::EIndirectPrimary::ReSTIR_SHaRC;
            else if (Name == QStringLiteral("ddgi"))
                _Out = Smile::EIndirectPrimary::DDGI;
            else if (Name == QStringLiteral("off"))
                _Out = Smile::EIndirectPrimary::Off;
            else
                return false;
            return true;
        }

        bool ReadOptionalIndirectFallback(
            const QJsonObject& _Object, const char* _Name,
            std::optional<Smile::EIndirectFallback>& _Out) {
            const QJsonValue Value = _Object.value(QLatin1String(_Name));
            if (Value.isUndefined() || Value.isNull()) { _Out.reset(); return true; }
            if (!Value.isString()) return false;
            const QString Name = Value.toString();
            if (Name == QStringLiteral("ddgi"))
                _Out = Smile::EIndirectFallback::DDGI;
            else if (Name == QStringLiteral("environment"))
                _Out = Smile::EIndirectFallback::Environment;
            else if (Name == QStringLiteral("black"))
                _Out = Smile::EIndirectFallback::Black;
            else
                return false;
            return true;
        }

        QString IndirectPrimaryName(int _Value) {
            switch (static_cast<Smile::EIndirectPrimary>(_Value)) {
            case Smile::EIndirectPrimary::ReSTIR_SHaRC: return QStringLiteral("restir_sharc");
            case Smile::EIndirectPrimary::DDGI:         return QStringLiteral("ddgi");
            case Smile::EIndirectPrimary::Off:          return QStringLiteral("off");
            }
            return QStringLiteral("unknown");
        }

        QString IndirectFallbackName(int _Value) {
            switch (static_cast<Smile::EIndirectFallback>(_Value)) {
            case Smile::EIndirectFallback::DDGI:        return QStringLiteral("ddgi");
            case Smile::EIndirectFallback::Environment: return QStringLiteral("environment");
            case Smile::EIndirectFallback::Black:       return QStringLiteral("black");
            }
            return QStringLiteral("unknown");
        }

        QJsonArray SerializeTimings(const QVector<FProfileTimingSnapshot>& _Results) {
            QJsonArray Out;
            for (const auto& R : _Results) {
                Out.append(QJsonObject{
                    { QStringLiteral("name"), R.Name },
                    { QStringLiteral("queue"), R.Queue },
                    { QStringLiteral("depth"), R.Depth },
                    { QStringLiteral("milliseconds"), R.Milliseconds },
                    { QStringLiteral("rawMilliseconds"), R.RawMilliseconds },
                });
            }
            return Out;
        }

        QJsonObject SerializeSettings(const FRenderSettingsSnapshot& _Settings) {
            return QJsonObject{
                { QStringLiteral("ddgi"), _Settings.DDGI },
                { QStringLiteral("restirGI"), _Settings.ReSTIRGI },
                { QStringLiteral("restirDI"), _Settings.ReSTIRDI },
                { QStringLiteral("radianceCache"), _Settings.RadianceCache },
                { QStringLiteral("cacheQuery"), _Settings.CacheQuery },
                { QStringLiteral("cacheStats"), _Settings.CacheStats },
                { QStringLiteral("cacheStatsDetail"), _Settings.CacheStatsDetail },
                { QStringLiteral("cacheStatsSource"), _Settings.CacheStatsSource },
                { QStringLiteral("reflections"), _Settings.Reflections },
                { QStringLiteral("gtao"), _Settings.GTAO },
                { QStringLiteral("indirectPrimaryRequested"),
                  _Settings.IndirectPrimaryRequested },
                { QStringLiteral("indirectPrimaryEffective"),
                  _Settings.IndirectPrimaryEffective },
                { QStringLiteral("indirectFallbackRequested"),
                  _Settings.IndirectFallbackRequested },
                { QStringLiteral("indirectFallbackEffective"),
                  _Settings.IndirectFallbackEffective },
                { QStringLiteral("denoiser"), _Settings.Denoiser },
                { QStringLiteral("upscaler"), _Settings.Upscaler },
                { QStringLiteral("upscalerQuality"), _Settings.UpscalerQuality },
                { QStringLiteral("renderScale"), _Settings.RenderScale },
                { QStringLiteral("timeOfDayHours"), _Settings.TimeOfDayHours },
                { QStringLiteral("timeOfDayRunning"), _Settings.TimeOfDayRunning },
                // Knobs da matriz da Fase 0 do MESH-LIGHTS-PLAN.md. Vao no readback para o sweep
                // confirmar o degrau aplicado em vez de assumir que o pedido pegou.
                { QStringLiteral("diAnalyticCandidates"), _Settings.DIAnalyticCandidates },
                { QStringLiteral("diMeshCandidates"), _Settings.DIMeshCandidates },
                { QStringLiteral("meshLightsInPool"), _Settings.DIMeshLightsInPool },
                { QStringLiteral("diInitialVisibility"), _Settings.DIInitialVisibility },
                { QStringLiteral("meshCompactSupport"), _Settings.DIMeshCompactSupport },
            };
        }

        QJsonObject SerializeProfileSnapshot(const FProfileSnapshot& _Snapshot) {
            return QJsonObject{
                { QStringLiteral("frameIndex"), static_cast<double>(_Snapshot.FrameIndex) },
                { QStringLiteral("cpuFps"), _Snapshot.CpuFps },
                { QStringLiteral("outputWidth"), _Snapshot.OutputWidth },
                { QStringLiteral("outputHeight"), _Snapshot.OutputHeight },
                { QStringLiteral("renderWidth"), _Snapshot.RenderWidth },
                { QStringLiteral("renderHeight"), _Snapshot.RenderHeight },
                { QStringLiteral("gpu"), _Snapshot.Gpu },
                { QStringLiteral("direct"), SerializeTimings(_Snapshot.Direct) },
                { QStringLiteral("asyncCompute"), SerializeTimings(_Snapshot.AsyncCompute) },
                { QStringLiteral("vram"), QJsonObject{
                    { QStringLiteral("valid"), _Snapshot.Vram.Valid },
                    { QStringLiteral("localUsageBytes"),
                      static_cast<double>(_Snapshot.Vram.LocalUsageBytes) },
                    { QStringLiteral("localBudgetBytes"),
                      static_cast<double>(_Snapshot.Vram.LocalBudgetBytes) },
                    { QStringLiteral("nonLocalUsageBytes"),
                      static_cast<double>(_Snapshot.Vram.NonLocalUsageBytes) },
                } },
                { QStringLiteral("settings"), SerializeSettings(_Snapshot.Settings) },
            };
        }

        QJsonObject SerializeCamera(const FCameraPoseSnapshot& _Pose) {
            return QJsonObject{
                { QStringLiteral("position"), QJsonObject{
                    { QStringLiteral("x"), _Pose.X },
                    { QStringLiteral("y"), _Pose.Y },
                    { QStringLiteral("z"), _Pose.Z },
                } },
                { QStringLiteral("pitchDegrees"), _Pose.PitchDeg },
                { QStringLiteral("yawDegrees"), _Pose.YawDeg },
            };
        }

        QJsonObject SerializeGIStatus(const FGIStatusSnapshot& _Status) {
            QJsonArray Cascades;
            for (const auto& Cascade : _Status.Cascades) {
                Cascades.append(QJsonObject{
                    { QStringLiteral("index"), Cascade.Index },
                    { QStringLiteral("gridMin"), QJsonObject{
                        { QStringLiteral("x"), Cascade.GridMinX },
                        { QStringLiteral("y"), Cascade.GridMinY },
                        { QStringLiteral("z"), Cascade.GridMinZ },
                    } },
                    { QStringLiteral("spacing"), Cascade.Spacing },
                    { QStringLiteral("scrollCells"), QJsonObject{
                        { QStringLiteral("x"), Cascade.ScrollX },
                        { QStringLiteral("y"), Cascade.ScrollY },
                        { QStringLiteral("z"), Cascade.ScrollZ },
                    } },
                    { QStringLiteral("updateAge"), Cascade.UpdateAge },
                });
            }

            return QJsonObject{
                { QStringLiteral("frameIndex"), static_cast<double>(_Status.FrameIndex) },
                { QStringLiteral("policy"), QJsonObject{
                    { QStringLiteral("primary"), QJsonObject{
                        { QStringLiteral("requested"), IndirectPrimaryName(
                            _Status.Settings.IndirectPrimaryRequested) },
                        { QStringLiteral("effective"), IndirectPrimaryName(
                            _Status.Settings.IndirectPrimaryEffective) },
                    } },
                    { QStringLiteral("fallback"), QJsonObject{
                        { QStringLiteral("requested"), IndirectFallbackName(
                            _Status.Settings.IndirectFallbackRequested) },
                        { QStringLiteral("effective"), IndirectFallbackName(
                            _Status.Settings.IndirectFallbackEffective) },
                    } },
                } },
                { QStringLiteral("ddgi"), QJsonObject{
                    { QStringLiteral("enabled"), _Status.Settings.DDGI },
                    { QStringLiteral("initialized"), _Status.DDGIInitialized },
                    { QStringLiteral("desiredCascadeCount"), _Status.DesiredCascadeCount },
                    { QStringLiteral("actualCascadeCount"), _Status.ActualCascadeCount },
                    { QStringLiteral("gridCount"), QJsonObject{
                        { QStringLiteral("x"), _Status.GridCountX },
                        { QStringLiteral("y"), _Status.GridCountY },
                        { QStringLiteral("z"), _Status.GridCountZ },
                    } },
                    { QStringLiteral("probesPerCascade"), _Status.ProbesPerCascade },
                    { QStringLiteral("totalProbes"), _Status.TotalProbes },
                    { QStringLiteral("raysPerProbe"), _Status.RaysPerProbe },
                    { QStringLiteral("adaptiveMinRays"), _Status.AdaptiveMinRays },
                    { QStringLiteral("adaptiveMaxRays"), _Status.AdaptiveMaxRays },
                    { QStringLiteral("interleavedUpdates"), _Status.InterleavedUpdates },
                    { QStringLiteral("probeCompaction"), _Status.ProbeCompaction },
                    { QStringLiteral("probeCompactionEffective"),
                      _Status.ProbeCompactionEffective },
                    { QStringLiteral("activeProbeCount"), _Status.ActiveProbeCount },
                    { QStringLiteral("compactedProbeCapacity"),
                      _Status.CompactedProbeCapacity },
                    { QStringLiteral("probeWakeInterval"), _Status.ProbeWakeInterval },
                    { QStringLiteral("lastProbeWakeSerial"),
                      static_cast<double>(_Status.LastProbeWakeSerial) },
                    { QStringLiteral("scheduledCascadeCount"), _Status.ScheduledCascadeCount },
                    { QStringLiteral("lastUpdatedCascadeCount"), _Status.LastUpdatedCascadeCount },
                    { QStringLiteral("updateSerial"), static_cast<double>(_Status.UpdateSerial) },
                    { QStringLiteral("lastForcedUpdateSerial"),
                      static_cast<double>(_Status.LastForcedUpdateSerial) },
                    { QStringLiteral("scheduledFullForced"), _Status.ScheduledFullForced },
                    { QStringLiteral("adaptiveRays"), _Status.AdaptiveRays },
                    { QStringLiteral("adaptiveHysteresis"), _Status.AdaptiveHysteresis },
                    { QStringLiteral("cascades"), Cascades },
                } },
                { QStringLiteral("settings"), SerializeSettings(_Status.Settings) },
            };
        }
    }

    McpBridge::McpBridge(CaptureBridge* _Capture, CameraBookmarksBridge* _Bookmarks,
                         RenderSettingsController* _RenderSettings,
                         QObject* _Parent)
        : QObject(_Parent), Capture(_Capture), Bookmarks(_Bookmarks),
          RenderSettings(_RenderSettings),
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
            const bool Ready = RenderSettings ? RenderSettings->Ready()
                                              : Capture && Capture->Available();
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
        if (Command == QStringLiteral("camera_get")) {
            HandleCameraGet(_Socket, Id);
            return;
        }
        if (Command == QStringLiteral("camera_set")) {
            const QJsonValue Arguments = Request.value(QStringLiteral("arguments"));
            if (!Arguments.isObject()) {
                Reply(_Socket, Id, false,
                      QJsonObject{ { QStringLiteral("error"),
                                    QStringLiteral("arguments precisa ser objeto") } });
                return;
            }
            HandleCameraSet(_Socket, Id, Arguments.toObject());
            return;
        }
        if (Command == QStringLiteral("gi_status")) {
            HandleGIStatus(_Socket, Id);
            return;
        }
        if (Command == QStringLiteral("gi_configure")) {
            const QJsonValue Arguments = Request.value(QStringLiteral("arguments"));
            if (!Arguments.isObject()) {
                Reply(_Socket, Id, false,
                      QJsonObject{ { QStringLiteral("error"),
                                    QStringLiteral("arguments precisa ser objeto") } });
                return;
            }
            HandleGIConfigure(_Socket, Id, Arguments.toObject());
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
        if (!RenderSettings) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("controlador de render indisponivel") } });
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
        // Knobs da matriz da Fase 0 do MESH-LIGHTS-PLAN.md. Todos opcionais: o profile_configure
        // continua sendo o mesmo comando com o mesmo contrato, e um chamador que nao conheca estes
        // campos ve o comportamento de antes.
        //
        // O teto de 64 nao e limite de qualidade — o sweep vai de 8 para baixo. E guarda contra um
        // digito a mais num script, que num loop por pixel vira um dispatch de minutos.
        //
        // ⚠️ VALIDADO ANTES DO Restore ABAIXO, e a ordem nao e estilo. O Restore MOVE A CAMERA, e
        // uma falha depois dele devolveria erro deixando a pose trocada: o chamador conclui que
        // nada aconteceu e a proxima captura com bookmarkSlot -1 sai da camera errada, sem nada no
        // manifesto denunciando. Numa regua que existe para ser deterministica, todo argumento se
        // valida antes do primeiro efeito colateral.
        FProfileOverrides Overrides;
        if (!ReadOptionalNumberHalfOpen(_Arguments, "timeOfDayHours", 0.0, 24.0,
                                        Overrides.TimeOfDayHours)) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("timeOfDayHours precisa estar em [0, 24)") } });
            return;
        }

        const bool KnobsOk =
            ReadOptionalInteger(_Arguments, "diAnalyticCandidates", 1, 64,
                                Overrides.DIAnalyticCandidates) &&
            // Zero e valido no de mesh: e "nenhuma proposta de triangulo", que nao e o mesmo que
            // tirar o pool (para isso existe o meshLightsInPool).
            ReadOptionalInteger(_Arguments, "diMeshCandidates", 0, 64,
                                Overrides.DIMeshCandidates) &&
            ReadOptionalBool(_Arguments, "meshLightsInPool", Overrides.DIMeshLightsInPool) &&
            ReadOptionalBool(_Arguments, "diInitialVisibility", Overrides.DIInitialVisibility) &&
            ReadOptionalBool(_Arguments, "meshCompactSupport", Overrides.DIMeshCompactSupport) &&
            ReadOptionalBool(_Arguments, "backgroundThrottle",
                             Overrides.BackgroundThrottleEnabled);
        if (!KnobsOk) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("knob de amostragem do DI invalido") } });
            return;
        }

        if (Slot >= 0 && (!Bookmarks || !Bookmarks->Restore(Slot))) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("bookmark solicitado nao existe") } });
            return;
        }

        const EProfilePreset ProfilePreset = Preset == QStringLiteral("gameplay_rr")
            ? EProfilePreset::GameplayRR : EProfilePreset::ControlledNative;
        QString Error;
        const auto Applied = RenderSettings->ApplyProfilePreset(ProfilePreset, Error, Overrides);
        if (!Applied) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), Error } });
            return;
        }

        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"), QJsonObject{
                  { QStringLiteral("preset"), Applied->Preset },
                  { QStringLiteral("bookmarkSlot"), Slot },
                  { QStringLiteral("timeOfDayHours"), Applied->TimeOfDayHours },
                  { QStringLiteral("backgroundThrottle"),
                    Applied->BackgroundThrottleEnabled },
                  { QStringLiteral("settings"), SerializeSettings(Applied->Settings) },
              } } });
    }

    void McpBridge::HandleProfileSnapshot(QLocalSocket* _Socket, const QString& _Id) {
        if (!RenderSettings) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("controlador de render indisponivel") } });
            return;
        }
        QString Error;
        const auto Snapshot = RenderSettings->ProfileSnapshot(Error);
        if (!Snapshot) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), Error } });
            return;
        }
        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"),
                            SerializeProfileSnapshot(*Snapshot) } });
    }

    void McpBridge::HandleCameraGet(QLocalSocket* _Socket, const QString& _Id) {
        if (!RenderSettings) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("controlador de render indisponivel") } });
            return;
        }
        QString Error;
        const auto Pose = RenderSettings->CameraSnapshot(Error);
        if (!Pose) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), Error } });
            return;
        }
        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"), SerializeCamera(*Pose) } });
    }

    void McpBridge::HandleCameraSet(QLocalSocket* _Socket, const QString& _Id,
                                    const QJsonObject& _Arguments) {
        if (!RenderSettings) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("controlador de render indisponivel") } });
            return;
        }

        FCameraPoseSnapshot Pose;
        const bool Valid =
            ReadNumberClosed(_Arguments, "x", -1'000'000.0, 1'000'000.0, Pose.X) &&
            ReadNumberClosed(_Arguments, "y", -1'000'000.0, 1'000'000.0, Pose.Y) &&
            ReadNumberClosed(_Arguments, "z", -1'000'000.0, 1'000'000.0, Pose.Z) &&
            ReadNumberClosed(_Arguments, "pitchDegrees", -89.9, 89.9, Pose.PitchDeg) &&
            ReadNumberClosed(_Arguments, "yawDegrees", -36'000.0, 36'000.0, Pose.YawDeg);
        const QJsonValue CameraCutValue =
            _Arguments.value(QStringLiteral("cameraCut"));
        const bool CameraCutValid =
            CameraCutValue.isUndefined() || CameraCutValue.isBool();
        if (!Valid || !CameraCutValid) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("pose de camera invalida") } });
            return;
        }
        const bool CameraCut = CameraCutValue.toBool(true);

        QString Error;
        const auto Applied = RenderSettings->SetCameraPose(Pose, CameraCut, Error);
        if (!Applied) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), Error } });
            return;
        }
        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"), SerializeCamera(*Applied) } });
    }

    void McpBridge::HandleGIStatus(QLocalSocket* _Socket, const QString& _Id) {
        if (!RenderSettings) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("controlador de render indisponivel") } });
            return;
        }
        QString Error;
        const auto Status = RenderSettings->GIStatus(Error);
        if (!Status) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), Error } });
            return;
        }
        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"), SerializeGIStatus(*Status) } });
    }

    void McpBridge::HandleGIConfigure(QLocalSocket* _Socket, const QString& _Id,
                                      const QJsonObject& _Arguments) {
        if (!RenderSettings) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("controlador de render indisponivel") } });
            return;
        }

        FGIOverrides Overrides;
        const bool Valid =
            ReadOptionalBool(_Arguments, "ddgiEnabled", Overrides.DDGIEnabled) &&
            ReadOptionalIndirectPrimary(_Arguments, "indirectPrimary",
                                        Overrides.IndirectPrimary) &&
            ReadOptionalIndirectFallback(_Arguments, "indirectFallback",
                                         Overrides.IndirectFallback) &&
            ReadOptionalInteger(_Arguments, "cascadeCount", 1,
                                static_cast<int>(Smile::FDDGI::kMaxCascades),
                                Overrides.CascadeCount) &&
            ReadOptionalBool(_Arguments, "interleavedUpdates", Overrides.InterleavedUpdates) &&
            ReadOptionalBool(_Arguments, "probeCompaction", Overrides.ProbeCompaction) &&
            ReadOptionalBool(_Arguments, "adaptiveRays", Overrides.AdaptiveRays) &&
            ReadOptionalBool(_Arguments, "adaptiveHysteresis",
                             Overrides.AdaptiveHysteresis);
        if (!Valid) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"),
                                QStringLiteral("configuracao de GI invalida") } });
            return;
        }

        QString Error;
        const auto Applied = RenderSettings->ApplyGIOverrides(Overrides, Error);
        if (!Applied) {
            Reply(_Socket, _Id, false,
                  QJsonObject{ { QStringLiteral("error"), Error } });
            return;
        }
        Reply(_Socket, _Id, true,
              QJsonObject{ { QStringLiteral("result"), SerializeGIStatus(*Applied) } });
    }

    void McpBridge::HandleCapture(QLocalSocket* _Socket, const QString& _Id,
                                  const QJsonObject& _Arguments) {
        const bool Ready = RenderSettings ? RenderSettings->Ready()
                                          : Capture && Capture->Available();
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
