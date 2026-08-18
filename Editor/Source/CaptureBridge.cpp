#include "SmileEditor/CaptureBridge.h"
#include "Smile/Graphics/Renderer/Renderer.h"

#include <QFileInfo>
#include <QTimer>
#include <cmath>

namespace SmileEditor {

    CaptureBridge::CaptureBridge(QObject* _Parent)
        : QObject(_Parent), PollTimer(new QTimer(this))
    {
        PollTimer->setInterval(100);
        connect(PollTimer, &QTimer::timeout, this, &CaptureBridge::Poll);
    }

    bool CaptureBridge::Available() const {
        if (!Renderer) return false;
        auto Access = Renderer.TryLock();
        return Access && Access->IsInitialized();
    }

    void CaptureBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        // Carga aditiva nao troca o nome: a captura pertence a cena PRINCIPAL, mesma regra dos
        // bookmarks e dos demais sidecars.
        if (_Additive) return;
        SceneName = QFileInfo(_ScenePath).completeBaseName();
        emit ProgressChanged();
    }

    bool CaptureBridge::Busy() const {
        if (!Renderer) return false;
        auto Access = Renderer.TryLock();
        // TryLock e nao Lock: isto e lido por binding de QML a cada repaint, e bloquear a GUI
        // esperando a render thread terminar um frame para responder "esta ocupado?" trocaria
        // uma informacao de status por um engasgo na interface. Sem o lock, o valor anterior
        // continua valendo por alguns milissegundos, que e o bastante para um rotulo.
        return Access ? Access->CaptureBusy() : LastBusy;
    }

    int CaptureBridge::WarmupRemaining() const {
        if (!Renderer) return 0;
        auto Access = Renderer.TryLock();
        return Access ? static_cast<int>(Access->CaptureWarmupRemaining()) : LastRemaining;
    }

    double CaptureBridge::CurrentTimeOfDayHours() const {
        if (!Renderer) return -1.0;
        auto Access = Renderer.TryLock();
        if (!Access) return -1.0;
        return static_cast<double>(Access->GetTimeOfDay().TimeHours);
    }

    bool CaptureBridge::Shoot(int _Slot, int _WarmupFrames, bool _Scientific, double _PinHours) {
        if (!Renderer) {
            emit Message(tr("Renderizador indisponível — captura não iniciada"));
            return false;
        }

        Smile::FCaptureRequest Request;
        Request.SceneName    = SceneName.toStdString();
        Request.BookmarkSlot = _Slot;
        // Teto de 512 e o mesmo da calibracao do N no protocolo: acima disso o aquecimento passa
        // de dez segundos e o operador provavelmente digitou errado.
        Request.WarmupFrames = static_cast<Smile::u32>(
            _WarmupFrames < 0 ? 0 : (_WarmupFrames > 512 ? 512 : _WarmupFrames));
        Request.Preset       = _Scientific ? Smile::ECapturePreset::Scientific
                                           : Smile::ECapturePreset::Gameplay;
        // Negativo = não fixar (usa a hora corrente). Fixar é o caminho normal do A/B: com o
        // Time of Day correndo, duas capturas disparadas com minutos de diferença teriam sóis
        // diferentes, e nenhum reset conserta isso depois.
        Request.PinTimeOfDayHours =
            (_PinHours < 0.0) ? -1.0f
                              : static_cast<float>(std::fmod(_PinHours, 24.0));

        bool Accepted = false;
        {
            auto Access = Renderer.Lock();
            if (!Access) return false;
            Accepted = Access->RequestCapture(Request);
        }
        if (!Accepted) {
            emit Message(tr("Já existe uma captura em andamento"));
            return false;
        }

        Result = tr("aquecendo %1 frames…").arg(Request.WarmupFrames);
        PollTimer->start();
        emit ProgressChanged();
        emit Message(tr("Captura iniciada: %1 frames de aquecimento").arg(Request.WarmupFrames));
        return true;
    }

    void CaptureBridge::Poll() {
        if (!Renderer) return;

        bool Busy = LastBusy;
        int  Remaining = LastRemaining;
        Smile::FFrameCapture::FResult CaptureResult;
        bool HasResult = false;
        {
            auto Access = Renderer.TryLock();
            if (!Access) return;
            HasResult = Access->ConsumeCaptureResult(CaptureResult);
            Busy      = Access->CaptureBusy();
            Remaining = static_cast<int>(Access->CaptureWarmupRemaining());
        }

        if (HasResult) {
            const QString Png      = QString::fromStdWString(CaptureResult.PngPath);
            const QString Manifest = QString::fromStdWString(CaptureResult.ManifestPath);
            const QString Error    = QString::fromStdString(CaptureResult.Error);
            // Sucesso implica PNG *e* manifesto: sem manifesto o motor descarta os dois e reporta
            // falha, entao nao ha estado intermediario a exibir aqui.
            if (CaptureResult.Success) {
                Result = QFileInfo(Png).fileName();
                emit Message(tr("Captura gravada: %1").arg(Png));
            } else {
                Result = tr("falhou");
                emit Message(tr("Falha na captura: %1").arg(Error));
            }
            PollTimer->stop();
            emit Finished(CaptureResult.Success, Png, Manifest, Error,
                          static_cast<int>(CaptureResult.WarmupFrames));
        }

        if (!HasResult && !Busy) PollTimer->stop();

        if (HasResult || Busy != LastBusy || Remaining != LastRemaining) {
            LastBusy      = Busy;
            LastRemaining = Remaining;
            emit ProgressChanged();
        }
    }
}
