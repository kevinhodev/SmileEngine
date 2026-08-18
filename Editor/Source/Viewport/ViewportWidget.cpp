#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Core/Logger.h"
#include <QShowEvent>
#include <QResizeEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QGuiApplication>
#include <QCursor>
#include <QLocale>
#include <QMutexLocker>
#include <QMetaObject>
#include <QPointer>
#include <QPalette>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <vector>

namespace SmileEditor {
    static constexpr float kMouseSensitivity = 0.15f;  
    static constexpr int   kResizeDebounceMs = 80;
    ViewportWidget::ViewportWidget(QWidget* _Parent)
        : QWidget(_Parent),
          Renderer(RendererThread.GetRenderer())
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        // Evita o flash branco do HWND antes do primeiro Present.
        setAttribute(Qt::WA_NoSystemBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        QPalette InitialPalette = palette();
        InitialPalette.setColor(QPalette::Window, Qt::black);
        setPalette(InitialPalette);
        setAutoFillBackground(true);

        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 200);
        setMouseTracking(true);

        // Present controla o pacing; o timer apenas agenda quando o event loop fica livre.
        RedrawTimer = new QTimer(this);
        RedrawTimer->setInterval(0);
        RedrawTimer->setSingleShot(true);
        connect(RedrawTimer, &QTimer::timeout, this, &ViewportWidget::OnRenderTimer);

        // Aguarda o primeiro layout estabilizar antes de criar a swapchain.
        InitializationDebounce = new QTimer(this);
        InitializationDebounce->setSingleShot(true);
        InitializationDebounce->setInterval(kResizeDebounceMs);
        connect(InitializationDebounce, &QTimer::timeout,
                this, &ViewportWidget::EnsureRendererIsInitialized);

        // Coalesce resizes que não passam por WM_ENTERSIZEMOVE.
        ResizeDebounce = new QTimer(this);
        ResizeDebounce->setSingleShot(true);
        ResizeDebounce->setInterval(kResizeDebounceMs);
        connect(ResizeDebounce, &QTimer::timeout, this, &ViewportWidget::ApplyPendingResize);

        // Limita o editor em segundo plano a aproximadamente 10 FPS.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState State) {
            RedrawTimer->setInterval(State == Qt::ApplicationActive ? 0 : 100);
        });

        FrameTimer.start();
    }

    ViewportWidget::~ViewportWidget() {
        if (RedrawTimer) RedrawTimer->stop();
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        // O fluxo normal já espera RendererStopped; isto cobre teardown parcial.
        RendererThread.RequestStop();
        RendererThread.Join();
    }

    void ViewportWidget::BeginRendererShutdown() {
        if (RendererShutdownRequested) return;
        RendererShutdownRequested = true;
        if (RedrawTimer) RedrawTimer->stop();
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        RendererJobs.clear();
        RendererThread.RequestStop();
        if (RendererThread.IsStopped()) OnRenderThreadStopped();
    }

    QPaintEngine* ViewportWidget::paintEngine() const {
        return nullptr;
    }

    QString ViewportWidget::GetViewModeLabel() const {
        // Um alvo de debug ativo substitui o view mode base.
        const int TargetIndex = GetDebugTargetIndex();
        if (TargetIndex >= 0) {
            const QStringList Names = GetDebugTargetNames();
            if (TargetIndex < Names.size()) return Names[TargetIndex];
        }
        switch (CurrentViewMode) {
        case ReflectionHeatmap:
            return QStringLiteral("Heatmap");
        case Lit:
        default:
            return QStringLiteral("Lit");
        }
    }


    void ViewportWidget::SetGizmoMode(int _Mode) {
        if (_Mode < 0 || _Mode > static_cast<int>(GizmoController::EMode::Scale)) return;
        const auto Next = static_cast<GizmoController::EMode>(_Mode);
        if (GizmoCtrl.GetMode() == Next) return;
        GizmoCtrl.SetMode(Next);
        if (GizmoCtrl.GetMode() != Next) return;
        emit GizmoModeChanged();
        // O modo pode alterar a restrição de espaço exibida pela UI.
        emit GizmoSpaceChanged();
    }

    void ViewportWidget::SetGizmoSpace(int _Space) {
        if (_Space < 0 || _Space > static_cast<int>(GizmoController::ESpace::Local)) return;
        const auto Next = static_cast<GizmoController::ESpace>(_Space);
        if (GizmoCtrl.GetSpace() == Next) return;
        GizmoCtrl.SetSpace(Next);
        if (GizmoCtrl.GetSpace() != Next) return;
        emit GizmoSpaceChanged();
    }

    void ViewportWidget::ToggleGizmoSpace() {
        SetGizmoSpace(GizmoCtrl.GetSpace() == GizmoController::ESpace::World
                          ? static_cast<int>(GizmoController::ESpace::Local)
                          : static_cast<int>(GizmoController::ESpace::World));
    }

    void ViewportWidget::ToggleSnap(int _Mode) {
        switch (_Mode) {
            case 1: GizmoCtrl.SetSnapTranslateOn(!GizmoCtrl.Snap().TranslateOn); break;
            case 2: GizmoCtrl.SetSnapRotateOn(!GizmoCtrl.Snap().RotateOn);       break;
            case 3: GizmoCtrl.SetSnapScaleOn(!GizmoCtrl.Snap().ScaleOn);         break;
            default: return;
        }
        emit GizmoSnapChanged();
    }

    void ViewportWidget::SetSnapValue(int _Mode, double _Value) {
        const float V = static_cast<float>(_Value);
        if (!(V > 0.0f)) return;
        switch (_Mode) {
            case 1: GizmoCtrl.SetSnapTranslateM(V); break;
            case 2: GizmoCtrl.SetSnapRotateDeg(V);  break;
            case 3: GizmoCtrl.SetSnapScaleStep(V);  break;
            default: return;
        }
        emit GizmoSnapChanged();
    }

    void ViewportWidget::SelectLit() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetGBufferDebugMode(0);
        RendererAccess->SetFlickerMode(0);
        // O alvo de debug tem prioridade sobre o caminho normal.
        RendererAccess->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        CurrentViewMode = Lit;
        emit ViewStateChanged();
    }

    void ViewportWidget::SelectReflectionHeatmap() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetGBufferDebugMode(0);
        RendererAccess->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        RendererAccess->SetFlickerMode(2);
        CurrentViewMode = ReflectionHeatmap;
        emit ViewStateChanged();
    }

    QStringList ViewportWidget::GetDebugTargetNames() const {
        QStringList Names;
        for (const Smile::FDebugTarget& T : Smile::DebugTargets::All())
            Names << QString::fromStdString(T.Name);
        return Names;
    }

    int ViewportWidget::GetDebugTargetIndex() const {
        if (!Renderer) return -1;
        const Smile::u32 I = Renderer->GetDebugTargetIndex();
        return I == Smile::Renderer::kNoDebugTarget ? -1 : static_cast<int>(I);
    }

    bool ViewportWidget::IsRtShaderTimerAvailable() const {
        return Renderer && Renderer->IsRtShaderTimerAvailable();
    }

    bool ViewportWidget::IsRtShaderTimerEnabled() const {
        return Renderer && Renderer->GetRtShaderTimer();
    }

    bool ViewportWidget::IsBvhDebugAvailable() const {
        return Renderer && Renderer->IsBvhDebugAvailable();
    }

    bool ViewportWidget::IsBvhDebugEnabled() const {
        return Renderer && Renderer->GetBvhDebug();
    }

    int ViewportWidget::GetBvhDebugMode() const {
        return Renderer ? static_cast<int>(Renderer->GetBvhDebugMode()) : 0;
    }

    QVariantList ViewportWidget::GetDebugSelection() const {
        QVariantList L;
        if (!Renderer) return L;
        auto RendererAccess = Renderer.Lock();
        for (Smile::u32 I : Renderer->GetDebugSelection()) L << static_cast<int>(I);
        return L;
    }

    int ViewportWidget::GetDebugColumns() const {
        return Renderer ? static_cast<int>(Renderer->GetDebugColumns()) : 0;
    }

    double ViewportWidget::GetDebugExposure() const {
        return Renderer ? static_cast<double>(Renderer->GetDebugExposure()) : 1.0;
    }

    // Converte o índice global da probe para cascata, índice local e coordenada geométrica.
    bool ViewportWidget::GetDebugProbeCoordValues(FDebugProbeCoord& _Out) const {
        if (!Renderer || !DebugProbeSessionActive) return false;
        auto RendererAccess = Renderer.Lock();
        const auto& DDGI = Renderer->GetDDGI();
        const Smile::u32 Index = Renderer->GetDebugProbeIndex();
        if (!DDGI.IsReady() || Index == Smile::Renderer::kNoDebugProbe ||
            Index >= DDGI.NumProbesCount()) {
            return false;
        }

        const Smile::Vec3 Count = DDGI.GridCount();
        _Out.CountX = static_cast<int>(Count.X);
        _Out.CountY = static_cast<int>(Count.Y);
        _Out.CountZ = static_cast<int>(Count.Z);
        if (_Out.CountX <= 0 || _Out.CountY <= 0 || _Out.CountZ <= 0) return false;

        const int PerCascade = std::max(1, static_cast<int>(DDGI.ProbesPerCascade()));
        _Out.Cascade    = static_cast<int>(Index) / PerCascade;
        _Out.LocalIndex = static_cast<int>(Index) - _Out.Cascade * PerCascade;

        const int XY = _Out.CountX * _Out.CountY;
        int SZ = _Out.LocalIndex / XY;
        const int R = _Out.LocalIndex - SZ * XY;
        int SY = R / _Out.CountX;
        int SX = R - SY * _Out.CountX;
        // Desfaz o scroll circular do armazenamento para obter a coordenada geométrica.
        const int Scroll[3] = { DDGI.CascadeScroll(static_cast<Smile::u32>(_Out.Cascade), 0),
                                DDGI.CascadeScroll(static_cast<Smile::u32>(_Out.Cascade), 1),
                                DDGI.CascadeScroll(static_cast<Smile::u32>(_Out.Cascade), 2) };
        const int Counts[3] = { _Out.CountX, _Out.CountY, _Out.CountZ };
        int Geo[3] = { SX, SY, SZ };
        for (int A = 0; A < 3; ++A) {
            Geo[A] -= Scroll[A];
            if (Geo[A] < 0) Geo[A] += Counts[A];
        }
        _Out.X = Geo[0];
        _Out.Y = Geo[1];
        _Out.Z = Geo[2];
        _Out.GridMin = DDGI.CascadeGridMin(static_cast<Smile::u32>(_Out.Cascade));
        _Out.Spacing = DDGI.CascadeSpacing(static_cast<Smile::u32>(_Out.Cascade));
        return true;
    }

    int ViewportWidget::GetDebugProbeIndex() const {
        if (!Renderer || !DebugProbeSessionActive) return -1;
        const Smile::u32 Index = Renderer->GetDebugProbeIndex();
        return Index == Smile::Renderer::kNoDebugProbe ? -1 : static_cast<int>(Index);
    }

    QString ViewportWidget::GetDebugProbeCoord() const {
        FDebugProbeCoord C;
        if (!GetDebugProbeCoordValues(C)) return QString();
        return QStringLiteral("cascata %1 · grid (%2, %3, %4)")
            .arg(C.Cascade).arg(C.X).arg(C.Y).arg(C.Z);
    }

    QString ViewportWidget::GetDebugProbeWorld() const {
        FDebugProbeCoord C;
        if (!GetDebugProbeCoordValues(C)) return QString();
        const double S  = C.Spacing;
        const double PX = C.GridMin.X + static_cast<double>(C.X) * S;
        const double PY = C.GridMin.Y + static_cast<double>(C.Y) * S;
        const double PZ = C.GridMin.Z + static_cast<double>(C.Z) * S;
        const QLocale Locale;
        return QStringLiteral("posição-base %1 · %2 · %3 m")
            .arg(Locale.toString(PX, 'f', 2))
            .arg(Locale.toString(PY, 'f', 2))
            .arg(Locale.toString(PZ, 'f', 2));
    }

    QString ViewportWidget::GetDebugProbeGrid() const {
        FDebugProbeCoord C;
        if (!GetDebugProbeCoordValues(C)) return QString();
        const QLocale Locale;
        return QStringLiteral("%1 × %2 × %3 probes · spacing %4 m")
            .arg(C.CountX).arg(C.CountY).arg(C.CountZ)
            .arg(Locale.toString(static_cast<double>(C.Spacing), 'f', 2));
    }

    QString ViewportWidget::GetDebugProbeDistanceRange() const {
        FDebugProbeCoord C;
        if (!GetDebugProbeCoordValues(C)) return QString();
        const QLocale Locale;
        // Mantém o mesmo clamp de momentos usado pelo shader de update.
        return QStringLiteral("distância média · 0 → %1 m")
            .arg(Locale.toString(static_cast<double>(C.Spacing) * 2.6, 'f', 2));
    }

    bool ViewportWidget::IsDebugPreviewReady() const {
        QMutexLocker Lock(&DebugPreviewMutex);
        return !DebugPreviewImage.isNull();
    }

    QImage ViewportWidget::DebugPreviewImageCopy() const {
        QMutexLocker Lock(&DebugPreviewMutex);
        return DebugPreviewImage.copy();
    }

    void ViewportWidget::InvalidateDebugPreview() {
        {
            QMutexLocker Lock(&DebugPreviewMutex);
            DebugPreviewImage = QImage();
        }
        ++DebugPreviewSeq;
        emit DebugPreviewUpdated();
    }

    void ViewportWidget::SetDebugPreviewEnabled(bool _Enabled) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        if (!_Enabled && DebugProbeSessionActive) ClearDebugProbeInspection();
        RendererAccess->SetDebugPreviewEnabled(_Enabled);
        if (_Enabled) InvalidateDebugPreview();
    }

    void ViewportWidget::ToggleDebugSelection(int _Index) {
        if (!Renderer || _Index < 0) return;
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        auto RendererAccess = Renderer.Lock();
        std::vector<Smile::u32> Sel = RendererAccess->GetDebugSelection();
        const auto It = std::find(Sel.begin(), Sel.end(), static_cast<Smile::u32>(_Index));
        if (It != Sel.end()) Sel.erase(It);
        else if (Sel.size() < 16u) Sel.push_back(static_cast<Smile::u32>(_Index));
        else return;
        RendererAccess->SetDebugSelection(Sel);
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::ClearDebugSelection() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        RendererAccess->SetDebugSelection({});
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::ToggleRtShaderTimer() {
        if (!Renderer || !Renderer->IsRtShaderTimerAvailable()) return;
        Renderer->SetRtShaderTimer(!Renderer->GetRtShaderTimer());
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::ToggleBvhDebug() {
        if (!Renderer || !Renderer->IsBvhDebugAvailable()) return;
        Renderer->SetBvhDebug(!Renderer->GetBvhDebug());
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::SetBvhDebugMode(int _Mode) {
        if (!Renderer) return;
        if (_Mode < 0 || _Mode >= static_cast<int>(Smile::FBvhDebugView::EMode::Count)) return;
        Renderer->SetBvhDebugMode(static_cast<Smile::FBvhDebugView::EMode>(_Mode));
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::SetDebugColumns(int _Columns) {
        if (!Renderer) return;
        if (DebugProbeSessionActive) return;
        Renderer->SetDebugColumns(_Columns < 0 ? 0 : static_cast<Smile::u32>(_Columns));
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::SetDebugExposure(double _Exposure) {
        if (!Renderer) return;
        Renderer->SetDebugExposure(static_cast<float>(_Exposure));
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::InspectDDGIProbe(
            int _TargetIndex, double _U, double _V, double _TileAspect) {
        if (!Renderer || DebugProbeSessionActive || _TargetIndex < 0) return;
        auto RendererAccess = Renderer.Lock();
        const auto& Targets = Smile::DebugTargets::All();
        if (static_cast<size_t>(_TargetIndex) >= Targets.size()) return;
        const Smile::FDebugTarget& Clicked = Targets[static_cast<size_t>(_TargetIndex)];
        if (Clicked.AtlasTilePx == 0 ||
            (Clicked.Decode != Smile::EDebugDecode::DDGIIrradiance &&
             Clicked.Decode != Smile::EDebugDecode::DDGIDistance)) {
            return;
        }

        const auto& DDGI = Renderer->GetDDGI();
        if (!DDGI.IsReady()) return;
        const Smile::Vec3 Count = DDGI.GridCount();
        const int CountX = static_cast<int>(Count.X);
        const int CountY = static_cast<int>(Count.Y);
        const int CountZ = static_cast<int>(Count.Z);
        if (CountX <= 0 || CountY <= 0 || CountZ <= 0) return;

        // Usa a grade física do atlas, que pode diferir das dimensões geométricas do volume.
        const int TilesX = static_cast<int>(DDGI.AtlasTilesPerRow());
        const int TileRows = static_cast<int>(DDGI.AtlasTileRows());
        if (TilesX <= 0 || TileRows <= 0) return;
        const int Total  = TilesX * TileRows;
        const double Aspect = std::max(_TileAspect, 1e-4);
        const int Cols = std::max(1, static_cast<int>(
            std::ceil(std::sqrt(static_cast<double>(Total) * Aspect))));
        const int Rows = (Total + Cols - 1) / Cols;
        const double U = std::clamp(_U, 0.0, 0.999999);
        const double V = std::clamp(_V, 0.0, 0.999999);
        const int DisplayX = std::min(static_cast<int>(U * Cols), Cols - 1);
        const int DisplayY = std::min(static_cast<int>(V * Rows), Rows - 1);
        const int AtlasIndex = DisplayY * Cols + DisplayX;
        if (AtlasIndex < 0 || AtlasIndex >= Total) return;

        // FDDGI é a fonte única do mapeamento entre tile e probe.
        Smile::u32 Probe = 0;
        if (!DDGI.ProbeFromAtlasTile(static_cast<Smile::u32>(AtlasIndex), Probe)) return;
        const int ProbeIndex = static_cast<int>(Probe);

        DebugProbePreviousTargets.clear();
        for (Smile::u32 Index : Renderer->GetDebugSelection()) {
            if (Index < Targets.size())
                DebugProbePreviousTargets << QString::fromStdString(Targets[Index].Name);
        }
        DebugProbePreviousColumns = static_cast<int>(Renderer->GetDebugColumns());

        std::vector<Smile::u32> DetailTargets;
        for (size_t I = 0; I < Targets.size(); ++I) {
            if (Targets[I].Decode == Smile::EDebugDecode::DDGIIrradiance ||
                Targets[I].Decode == Smile::EDebugDecode::DDGIDistance) {
                DetailTargets.push_back(static_cast<Smile::u32>(I));
            }
        }
        if (DetailTargets.empty()) return;

        DebugProbeSessionActive = true;
        DebugProbeDirection.clear();
        DebugProbeSample.clear();
        ResetDebugProbePoint();
        Renderer->SetDebugSelection(DetailTargets);
        Renderer->SetDebugColumns(DetailTargets.size() > 1 ? 2u : 1u);
        SelectDebugProbe(ProbeIndex);
        InvalidateDebugPreview();
        emit DebugProbeDirectionChanged();
        emit DebugProbeSampleChanged();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::StepDebugProbe(int _DX, int _DY, int _DZ) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        FDebugProbeCoord C;
        if (!GetDebugProbeCoordValues(C)) return;
        const int Counts[3] = { C.CountX, C.CountY, C.CountZ };
        int Geo[3] = { std::clamp(C.X + _DX, 0, C.CountX - 1),
                       std::clamp(C.Y + _DY, 0, C.CountY - 1),
                       std::clamp(C.Z + _DZ, 0, C.CountZ - 1) };
        // Reaplica o scroll antes de linearizar a coordenada de armazenamento.
        const auto& DDGI = Renderer->GetDDGI();
        for (int A = 0; A < 3; ++A) {
            Geo[A] += DDGI.CascadeScroll(static_cast<Smile::u32>(C.Cascade), A);
            if (Geo[A] >= Counts[A]) Geo[A] -= Counts[A];
        }
        const int PerCascade = C.CountX * C.CountY * C.CountZ;
        const int Index = C.Cascade * PerCascade +
                          (Geo[0] + Geo[1] * C.CountX + Geo[2] * C.CountX * C.CountY);
        if (Index == GetDebugProbeIndex()) return;
        ResetDebugProbePoint();
        SelectDebugProbe(Index);
        if (!DebugProbeSample.isEmpty()) {
            DebugProbeSample.clear();
            emit DebugProbeSampleChanged();
        }
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::ClearDebugProbeInspection() {
        if (!Renderer || !DebugProbeSessionActive) return;
        auto RendererAccess = Renderer.Lock();
        ResetDebugProbePoint();
        SelectDebugProbe(-1);

        std::vector<Smile::u32> Restored;
        Restored.reserve(static_cast<size_t>(DebugProbePreviousTargets.size()));
        for (const QString& Name : DebugProbePreviousTargets) {
            const Smile::u32 Index = Smile::DebugTargets::IndexOf(Name.toStdString());
            if (Index != Smile::DebugTargets::kInvalid) Restored.push_back(Index);
        }
        RendererAccess->SetDebugSelection(Restored);
        RendererAccess->SetDebugColumns(
            DebugProbePreviousColumns < 0 ? 0u
                                          : static_cast<Smile::u32>(DebugProbePreviousColumns));

        DebugProbeSessionActive = false;
        DebugProbePreviousTargets.clear();
        DebugProbeDirection.clear();
        DebugProbeSample.clear();
        InvalidateDebugPreview();
        emit DebugProbeDirectionChanged();
        emit DebugProbeSampleChanged();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::SelectDebugProbe(int _ProbeIndex) {
        if (!Renderer) return;
        Renderer->SetDebugProbeIndex(_ProbeIndex);
        DebugProbeBaseIndex = _ProbeIndex;
    }

    void ViewportWidget::ResetDebugProbePoint(bool _CancelRendererRequest) {
        const bool Changed = DebugProbePointPickArmed ||
                             !DebugProbePointSummary.isEmpty() ||
                             !DebugProbeContributors.isEmpty();
        if (Renderer && _CancelRendererRequest) {
            auto RendererAccess = Renderer.Lock();
            RendererAccess->CancelDebugProbePoint();
            if (DebugProbeSessionActive) {
                RendererAccess->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
            }
        }
        DebugProbePointPickArmed = false;
        DebugProbePointSummary.clear();
        DebugProbeContributors.clear();
        DebugProbeContributorIndices.fill(0);
        DebugProbeContributorWeights.fill(0.0f);
        DebugProbeContributorCount = 0;
        DebugProbeContributorRiskSlot = -1;
        if (!MouseLookActive) unsetCursor();
        if (Changed) emit DebugProbePointChanged();
    }

    void ViewportWidget::ArmDebugProbePointPick() {
        if (!Renderer || !Renderer->IsInitialized() || !DebugProbeSessionActive) return;
        if (DebugProbePointPickArmed) {
            ResetDebugProbePoint();
            return;
        }

        {
            auto RendererAccess = Renderer.Lock();
            RendererAccess->CancelDebugProbePoint();
            RendererAccess->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
        }
        DebugProbeContributors.clear();
        DebugProbeContributorIndices.fill(0);
        DebugProbeContributorWeights.fill(0.0f);
        DebugProbeContributorCount = 0;
        DebugProbeContributorRiskSlot = -1;
        DebugProbePointPickArmed = true;
        DebugProbePointSummary =
            QStringLiteral("Clique em uma superfície no viewport para diagnosticar o DDGI");
        setCursor(Qt::CrossCursor);
        emit DebugProbePointChanged();
    }

    void ViewportWidget::ClearDebugProbePoint() {
        ResetDebugProbePoint();
    }

    void ViewportWidget::SelectDebugProbeContributor(int _ProbeIndex) {
        if (!Renderer || !DebugProbeSessionActive || _ProbeIndex < 0) return;
        auto RendererAccess = Renderer.Lock();
        bool Found = false;
        for (Smile::u32 I = 0; I < DebugProbeContributorCount; ++I) {
            if (DebugProbeContributorIndices[I] ==
                static_cast<Smile::u32>(_ProbeIndex)) {
                Found = true;
                break;
            }
        }
        if (!Found) return;

        // A escolha explícita passa a ser a base restaurada por point-picks posteriores.
        SelectDebugProbe(_ProbeIndex);
        RendererAccess->SetDebugProbeContributors(
            DebugProbeContributorIndices.data(),
            DebugProbeContributorWeights.data(),
            DebugProbeContributorCount,
            DebugProbeContributorRiskSlot);
        InvalidateDebugPreview();
        emit DebugSettingsChanged();
    }

    void ViewportWidget::UpdateDebugProbeDirection(double _U, double _V) {
        QString NewLabel;
        const bool Valid = DebugProbeSessionActive && _U >= 0.0 && _U <= 1.0 &&
                           _V >= 0.0 && _V <= 1.0;
        if (Renderer)
            Renderer->SetDebugProbeSampleUV(
                Valid ? static_cast<float>(_U) : -1.0f,
                Valid ? static_cast<float>(_V) : -1.0f);
        if (Valid) {
            double X = _U * 2.0 - 1.0;
            double Y = _V * 2.0 - 1.0;
            double Z = 1.0 - std::abs(X) - std::abs(Y);
            const double T = std::clamp(-Z, 0.0, 1.0);
            X += X >= 0.0 ? -T : T;
            Y += Y >= 0.0 ? -T : T;
            const double L = std::sqrt(X * X + Y * Y + Z * Z);
            if (L > 1e-8) { X /= L; Y /= L; Z /= L; }
            const QLocale Locale;
            NewLabel = QStringLiteral("direção (%1, %2, %3)")
                .arg(Locale.toString(X, 'f', 2))
                .arg(Locale.toString(Y, 'f', 2))
                .arg(Locale.toString(Z, 'f', 2));
        }
        if (DebugProbeDirection != NewLabel) {
            DebugProbeDirection = NewLabel;
            emit DebugProbeDirectionChanged();
        }
        if (!DebugProbeSample.isEmpty()) {
            DebugProbeSample.clear();
            emit DebugProbeSampleChanged();
        }
    }

    void ViewportWidget::SelectDebugTarget(int _Index) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetDebugTargetIndex(
            _Index < 0 ? Smile::Renderer::kNoDebugTarget
                       : static_cast<Smile::u32>(_Index));
        if (_Index >= 0) {
            // O alvo substitui Lit/Heatmap enquanto estiver ativo.
            RendererAccess->SetFlickerMode(0);
            RendererAccess->SetGBufferDebugMode(0);
            CurrentViewMode = Lit;
        }
        emit ViewStateChanged();
    }



    void ViewportWidget::RequestSettings() {
        emit SettingsRequested();
    }

    void ViewportWidget::EnsureRendererIsInitialized() {
        if (RendererShutdownRequested || RendererStoppedFlag || RendererThread.IsStarted()) return;
        const HWND hWnd = reinterpret_cast<HWND>(winId());
        // Limpa o HWND antes de transferir a superfície ao D3D.
        RECT ClientRect{};
        if (GetClientRect(hWnd, &ClientRect)) {
            if (HDC DeviceContext = GetDC(hWnd)) {
                FillRect(DeviceContext, &ClientRect,
                         static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                ReleaseDC(hWnd, DeviceContext);
            }
        }
        const QSize InitialSize = size();
        AppliedResizeSize = InitialSize;

        QPointer<ViewportWidget> Self(this);
        RenderThread::Callbacks Hooks;
        Hooks.Initialized = [Self]() {
            if (!Self) return;
            QMetaObject::invokeMethod(Self, [Self]() {
                if (Self) Self->OnRendererInitialized();
            }, Qt::QueuedConnection);
        };
        Hooks.FrameCompleted = [Self](RenderThread::FrameCompletion Completion) {
            if (!Self) return;
            const QString Error = QString::fromUtf8(Completion.Error);
            QMetaObject::invokeMethod(Self,
                [Self, Success = Completion.Success,
                 Terminal = Completion.Terminal, Error]() {
                if (Self) Self->OnFrameCompleted(Success, Terminal, Error);
            }, Qt::QueuedConnection);
        };
        Hooks.Progress = [Self](const std::string& Label, const std::string& Detail,
                                float Fraction) {
            if (!Self) return;
            const QString LabelText  = QString::fromUtf8(Label);
            const QString DetailText = QString::fromUtf8(Detail);
            QMetaObject::invokeMethod(Self, [Self, LabelText, DetailText, Fraction]() {
                if (Self) emit Self->InitProgress(LabelText, DetailText,
                                                  static_cast<qreal>(Fraction));
            }, Qt::QueuedConnection);
        };
        Hooks.InitializationFailed = [Self](const std::string& Error) {
            if (!Self) return;
            const QString Message = QString::fromUtf8(Error);
            QMetaObject::invokeMethod(Self, [Self, Message]() {
                if (Self) Self->OnRendererInitializationFailed(Message);
            }, Qt::QueuedConnection);
        };
        Hooks.Stopped = [Self]() {
            if (!Self) return;
            QMetaObject::invokeMethod(Self, [Self]() {
                if (Self) Self->OnRenderThreadStopped();
            }, Qt::QueuedConnection);
        };
        RendererThread.Start(hWnd,
                             static_cast<unsigned int>(InitialSize.width()),
                             static_cast<unsigned int>(InitialSize.height()),
                             std::move(Hooks));
    }

    void ViewportWidget::OnRendererInitialized() {
        if (RendererShutdownRequested || Initialized || !RendererThread.IsReady()) return;
        Initialized = true;
        PendingResizeSize = size();
        ApplyPendingResize();
        FrameTimer.restart();
        emit RendererInitialized();
        emit DebugTargetsChanged();
        if (isVisible() && RedrawTimer) RedrawTimer->start();
    }

    void ViewportWidget::OnRendererInitializationFailed(const QString& _Error) {
        Q_UNUSED(_Error);
        Initialized = false;
        if (RedrawTimer) RedrawTimer->stop();
    }

    bool ViewportWidget::CommitImportedSceneAsync(
            std::shared_ptr<Smile::FSceneImportResult> _Imported,
            bool _Additive,
            SceneCommitCallback _Completion) {
        if (!_Imported) return false;
        return EnqueueRendererJob(
            {},
            [Imported = std::move(_Imported), _Additive](Smile::Renderer& _Renderer) mutable {
                RenderThread::JobCompletion Result;
                Result.Success = _Renderer.CommitCookedScene(
                    std::move(Imported), _Additive);
                if (!Result.Success)
                    Result.Error = "CommitCookedScene retornou false";
                return Result;
            },
            std::move(_Completion));
    }

    bool ViewportWidget::EnqueueRendererJob(
            const QString& _CoalesceKey,
            RenderThread::RendererJob _Job,
            SceneCommitCallback _Completion) {
        if (!_Job || RendererShutdownRequested || !RendererThread.IsReady()) return false;

        // Conserva somente o último job ainda não iniciado de cada família.
        if (!_CoalesceKey.isEmpty()) {
            for (auto It = RendererJobs.rbegin(); It != RendererJobs.rend(); ++It) {
                if (It->CoalesceKey == _CoalesceKey) {
                    It->Execute = std::move(_Job);
                    It->Completion = std::move(_Completion);
                    return true;
                }
            }
        }

        RendererJobs.push_back(FQueuedRendererJob{
            _CoalesceKey, std::move(_Job), std::move(_Completion) });
        DispatchNextRendererJob();
        return true;
    }

    void ViewportWidget::DispatchNextRendererJob() {
        if (RendererJobActive || RendererShutdownRequested || RendererJobs.empty()) return;

        FQueuedRendererJob Job = std::move(RendererJobs.front());
        RendererJobs.pop_front();
        if (RedrawTimer) RedrawTimer->stop();
        RendererJobActive = true;

        QPointer<ViewportWidget> Self(this);
        SceneCommitCallback Completion = std::move(Job.Completion);
        SceneCommitCallback FailureCompletion = Completion;
        const bool Queued = RendererThread.RequestRendererJob(
            std::move(Job.Execute),
            [Self, Completion = std::move(Completion)](
                    RenderThread::JobCompletion _Result) mutable {
                if (!Self) return;
                const bool Success = _Result.Success;
                const QString Error = QString::fromStdString(_Result.Error);
                QMetaObject::invokeMethod(Self,
                    [Self, Success, Error, Completion = std::move(Completion)]() mutable {
                        if (!Self) return;

                        // Publica o resultado antes de liberar o próximo job ou frame.
                        if (!Self->RendererShutdownRequested && Completion)
                            Completion(Success, Error);
                        Self->RendererThread.CompleteJob();
                        Self->RendererJobActive = false;
                        Self->DispatchNextRendererJob();

                        if (Self->PendingResizeSize.isValid() &&
                            Self->PendingResizeSize != Self->AppliedResizeSize &&
                            Self->ResizeDebounce && !Self->InteractiveResize)
                            Self->ResizeDebounce->start(0);

                        if (!Self->RendererShutdownRequested &&
                            Self->RendererJobs.empty() && Self->isVisible() &&
                            Self->RedrawTimer)
                            Self->RedrawTimer->start();
                    }, Qt::QueuedConnection);
            });

        if (!Queued) {
            RendererJobActive = false;
            if (!RendererShutdownRequested && FailureCompletion)
                FailureCompletion(false, QStringLiteral("render thread indisponivel"));
            DispatchNextRendererJob();
            if (RendererJobs.empty() && isVisible() && RedrawTimer) RedrawTimer->start(1);
        }
    }

    void ViewportWidget::OnRenderThreadStopped() {
        if (RendererStoppedFlag) return;
        RendererThread.Join();
        RendererStoppedFlag = true;
        Initialized = false;
        if (RedrawTimer) RedrawTimer->stop();
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        emit RendererStopped();
    }

    void ViewportWidget::showEvent(QShowEvent* _Event) {
        QWidget::showEvent(_Event);
        // Evita criar e redimensionar a swapchain durante o primeiro layout.
        PendingResizeSize = size();
        if (!RendererThread.IsStarted()) {
            InitializationDebounce->start();
        } else {
            ApplyPendingResize();
        }
        FrameTimer.restart();
        if (Initialized && !RendererStoppedFlag && !RendererThread.HasFrameInFlight() &&
            !RendererThread.HasJobInFlight())
            RedrawTimer->start();
    }

    void ViewportWidget::hideEvent(QHideEvent* _Event) {
        RedrawTimer->stop();
        InitializationDebounce->stop();
        ResizeDebounce->stop();
        QWidget::hideEvent(_Event);
    }

    void ViewportWidget::resizeEvent(QResizeEvent* _Event) {
        QWidget::resizeEvent(_Event);
        PendingResizeSize = _Event->size();
        if (!Initialized) {
            if (!RendererThread.IsStarted()) InitializationDebounce->start();
            return;
        }
        if (InteractiveResize) return;
        ResizeDebounce->start();
    }

    void ViewportWidget::BeginInteractiveResize() {
        InteractiveResize = true;
        ResizeDebounce->stop();
        PendingResizeSize = size();
    }

    void ViewportWidget::EndInteractiveResize() {
        if (!InteractiveResize) return;
        InteractiveResize = false;
        PendingResizeSize = size();
        // ResizeBuffers roda depois de sair do callback nativo.
        ResizeDebounce->start(0);
    }

    void ViewportWidget::ApplyPendingResize() {
        if (!Initialized || InteractiveResize || !Renderer) return;

        if (RendererThread.HasFrameInFlight() || RendererThread.HasJobInFlight()) return;
        auto RendererAccess = Renderer.TryLock();
        if (!RendererAccess) {
            // Mantém a GUI responsiva enquanto Present possui o Renderer.
            ResizeDebounce->start(1);
            return;
        }
        if (!RendererAccess->IsInitialized()) return;

        const QSize Target = PendingResizeSize;
        PendingResizeSize = {};
        if (Target.width() <= 0 || Target.height() <= 0 || Target == AppliedResizeSize) return;

        RendererAccess->Resize(static_cast<unsigned int>(Target.width()),
                               static_cast<unsigned int>(Target.height()));
        AppliedResizeSize = Target;
        emit DebugTargetsChanged();
    }

    void ViewportWidget::paintEvent(QPaintEvent* _Event) {
        Q_UNUSED(_Event);
    }

    void ViewportWidget::OnRenderTimer() {
        if (RendererShutdownRequested) return;
        EnsureRendererIsInitialized();
        if (!Initialized || !RendererThread.IsReady() ||
            RendererThread.HasFrameInFlight() || RendererThread.HasJobInFlight()) return;

        // O tick é coalescido quando outro acesso sincronizado possui o Renderer.
        auto RendererAccess = Renderer.TryLock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) {
            if (isVisible() && RedrawTimer) RedrawTimer->start(1);
            return;
        }

        // Precisão sub-ms mantém o delta estável em frame rates altos.
        float DeltaTime = static_cast<float>(static_cast<double>(FrameTimer.nsecsElapsed()) / 1.0e9);
        FrameTimer.restart();
        DeltaTime = Smile::Clamp(DeltaTime, 0.0001f, 0.1f);
        LastFrameDeltaTime = DeltaTime;

        Smile::CameraInput CameraInput;
        CameraInput.Look  = MouseLookActive
            ? Smile::Vec2{ MouseDelta.X * kMouseSensitivity,
                          -MouseDelta.Y * kMouseSensitivity }   
            : Smile::Vec2::Zero();
        // Novos eventos continuam acumulando enquanto o frame está em execução.
        MouseDelta = Smile::Vec2::Zero();
        // Movimento de câmera exige mouse-look ativo para não conflitar com atalhos do gizmo.
        CameraInput.Move  = MouseLookActive
            ? Smile::Vec3{
                  static_cast<float>(IsHeld(Qt::Key_D) - IsHeld(Qt::Key_A)),
                  static_cast<float>(IsHeld(Qt::Key_E) - IsHeld(Qt::Key_Q)),
                  static_cast<float>(IsHeld(Qt::Key_W) - IsHeld(Qt::Key_S)),
              }
            : Smile::Vec3::Zero();
        CameraInput.Speed = IsHeld(Qt::Key_Shift) ? 4.0f : 1.0f;

        RendererAccess->UpdateCamera(CameraInput, DeltaTime);
        FlushPendingGizmoInput(*RendererAccess);
        // Submit resolve a restrição dependente da seleção e publica os handles deste frame.
        const int RestrictionBefore = GetGizmoSpaceRestriction();
        GizmoCtrl.Submit(*RendererAccess);
        if (GetGizmoSpaceRestriction() != RestrictionBefore) emit GizmoSpaceChanged();
        if (RendererThread.RequestFrame()) {
            if (!RendererOwnsSurface) {
                RendererOwnsSurface = true;
                setAutoFillBackground(false);
                setAttribute(Qt::WA_NoSystemBackground, true);
                setAttribute(Qt::WA_OpaquePaintEvent, true);
            }
            // O callback do frame rearma o timer single-shot.
        }
    }

    void ViewportWidget::OnFrameCompleted(bool _Success, bool _Terminal,
                                          const QString& _Error) {
        if (!_Success && !_Error.isEmpty() && _Terminal)
            Smile::LogError("Render thread encerrada apos falhas consecutivas: " +
                            _Error.toStdString());

        if (_Success && !RendererShutdownRequested) OnFrameRendered();

        // Toda conclusão libera FrameOutstanding neste ponto único.
        RendererThread.CompleteFrame();
        if (RendererShutdownRequested || _Terminal) {
            if (RedrawTimer) RedrawTimer->stop();
            return;
        }
        if (RendererThread.HasJobInFlight()) return;
        if (PendingResizeSize.isValid() && PendingResizeSize != AppliedResizeSize &&
            ResizeDebounce && !InteractiveResize)
            ResizeDebounce->start(0);
        if (isVisible() && RedrawTimer) RedrawTimer->start();
    }

    void ViewportWidget::OnFrameRendered() {
        // FrameReady observa um snapshot consistente antes de o próximo frame ser solicitado.
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) return;

        // Após o readback, o QML recebe apenas uma cópia em QImage.
        std::vector<Smile::u8> DebugPixels;
        if (Renderer->ConsumeDebugPreview(DebugPixels)) {
            const size_t Expected =
                static_cast<size_t>(Smile::Renderer::kDebugPreviewWidth) *
                Smile::Renderer::kDebugPreviewHeight * 4u;
            if (DebugPixels.size() == Expected) {
                QImage Image(DebugPixels.data(),
                             static_cast<int>(Smile::Renderer::kDebugPreviewWidth),
                             static_cast<int>(Smile::Renderer::kDebugPreviewHeight),
                             QImage::Format_RGBA8888);
                {
                    QMutexLocker Lock(&DebugPreviewMutex);
                    DebugPreviewImage = Image.copy();
                }
                ++DebugPreviewSeq;
                emit DebugPreviewUpdated();
            }
        }

        Smile::Renderer::FDebugProbeSample ProbeSample;
        if (Renderer->ConsumeDebugProbeSample(ProbeSample) &&
            DebugProbeSessionActive &&
            ProbeSample.ProbeIndex == Renderer->GetDebugProbeIndex()) {
            const QLocale Locale;
            DebugProbeSample = QStringLiteral(
                "irr %1 · %2 · %3  •  distância %4 m  •  σ %5 m")
                .arg(Locale.toString(ProbeSample.Irradiance[0], 'f', 3))
                .arg(Locale.toString(ProbeSample.Irradiance[1], 'f', 3))
                .arg(Locale.toString(ProbeSample.Irradiance[2], 'f', 3))
                .arg(Locale.toString(ProbeSample.MeanDistance, 'f', 2))
                .arg(Locale.toString(ProbeSample.DistanceDeviation, 'f', 2));
            emit DebugProbeSampleChanged();
        }

        Smile::FDDGIPointDiagnostic PointDiagnostic;
        if (Renderer->ConsumeDebugProbePoint(PointDiagnostic) &&
            DebugProbeSessionActive) {
            DebugProbeContributors.clear();
            DebugProbeContributorIndices.fill(0);
            DebugProbeContributorWeights.fill(0.0f);
            DebugProbeContributorCount = 0;
            DebugProbeContributorRiskSlot = -1;

            if (!PointDiagnostic.Valid) {
                DebugProbePointSummary =
                    QStringLiteral("Nenhuma superfície encontrada nesse pixel");
                Renderer->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
            } else if (PointDiagnostic.VolumeWeight <= 0.001f) {
                // Fora do volume, restaura a seleção explícita e remove destaques temporários.
                DebugProbePointSummary = QStringLiteral(
                    "ponto %1 · %2 · %3 m  •  fora do volume de sondas — só ambiente")
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.X, 'f', 2))
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.Y, 'f', 2))
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.Z, 'f', 2));
                if (DebugProbeBaseIndex >= 0)
                    Renderer->SetDebugProbeIndex(DebugProbeBaseIndex);
                Renderer->ClearDebugProbeContributors();
                InvalidateDebugPreview();
                emit DebugSettingsChanged();
            } else {
                const auto& DDGI = Renderer->GetDDGI();
                const Smile::Vec3 GridCount = DDGI.GridCount();
                const int CountX = std::max(1, static_cast<int>(GridCount.X));
                const int CountY = std::max(1, static_cast<int>(GridCount.Y));
                const int XY = CountX * CountY;
                const QLocale Locale;

                for (Smile::u32 I = 0;
                     I < Smile::FDDGIDebug::kPointProbeCount; ++I) {
                    const Smile::FDDGIPointProbeDiagnostic& P =
                        PointDiagnostic.Probes[I];
                    // O índice publicado inclui a base global da cascata.
                    const int Index    = static_cast<int>(P.ProbeIndex);
                    const int PerCasc  = std::max(1, XY * std::max(1, static_cast<int>(GridCount.Z)));
                    const int Cascade  = Index / PerCasc;
                    const int Local    = Index - Cascade * PerCasc;
                    const int Z = Local / XY;
                    const int R = Local - Z * XY;
                    const int Y = R / CountX;
                    const int X = R - Y * CountX;

                    QVariantMap Item;
                    Item.insert(QStringLiteral("probeIndex"), Index);
                    Item.insert(QStringLiteral("cascade"), Cascade);
                    Item.insert(QStringLiteral("coord"),
                                QStringLiteral("c%1 (%2,%3,%4)")
                                    .arg(Cascade).arg(X).arg(Y).arg(Z));
                    Item.insert(QStringLiteral("active"), P.Active);
                    Item.insert(QStringLiteral("dominant"),
                                static_cast<int>(I) == PointDiagnostic.DominantSlot);
                    Item.insert(QStringLiteral("risk"),
                                static_cast<int>(I) == PointDiagnostic.RiskSlot);
                    Item.insert(QStringLiteral("distance"), P.DistanceToPoint);
                    Item.insert(QStringLiteral("mean"), P.MeanDistance);
                    Item.insert(QStringLiteral("sigma"), P.DistanceDeviation);
                    Item.insert(QStringLiteral("trilinear"), P.TrilinearWeight);
                    Item.insert(QStringLiteral("visibility"), P.Visibility);
                    Item.insert(QStringLiteral("weight"), P.NormalizedWeight);
                    Item.insert(QStringLiteral("leakRisk"), P.LeakRisk);
                    Item.insert(
                        QStringLiteral("irradiance"),
                        QStringLiteral("%1 · %2 · %3")
                            .arg(Locale.toString(P.Irradiance.X, 'f', 2))
                            .arg(Locale.toString(P.Irradiance.Y, 'f', 2))
                            .arg(Locale.toString(P.Irradiance.Z, 'f', 2)));
                    DebugProbeContributors.append(Item);

                    if (P.Active &&
                        DebugProbeContributorCount <
                            DebugProbeContributorIndices.size()) {
                        if (static_cast<int>(I) == PointDiagnostic.RiskSlot) {
                            DebugProbeContributorRiskSlot =
                                static_cast<int>(DebugProbeContributorCount);
                        }
                        DebugProbeContributorIndices[DebugProbeContributorCount] =
                            P.ProbeIndex;
                        DebugProbeContributorWeights[DebugProbeContributorCount] =
                            P.NormalizedWeight;
                        ++DebugProbeContributorCount;
                    }
                }

                const int FocusSlot = PointDiagnostic.RiskSlot >= 0
                    ? PointDiagnostic.RiskSlot : PointDiagnostic.DominantSlot;
                if (FocusSlot >= 0 &&
                    FocusSlot < static_cast<int>(
                        Smile::FDDGIDebug::kPointProbeCount)) {
                    Renderer->SetDebugProbeIndex(static_cast<int>(
                        PointDiagnostic.Probes[
                            static_cast<size_t>(FocusSlot)].ProbeIndex));
                }
                Renderer->SetDebugProbeContributors(
                    DebugProbeContributorIndices.data(),
                    DebugProbeContributorWeights.data(),
                    DebugProbeContributorCount,
                    DebugProbeContributorRiskSlot);

                const int DominantIndex = PointDiagnostic.DominantSlot >= 0
                    ? static_cast<int>(PointDiagnostic.Probes[
                        static_cast<size_t>(PointDiagnostic.DominantSlot)].ProbeIndex)
                    : -1;
                const int RiskIndex = PointDiagnostic.RiskSlot >= 0
                    ? static_cast<int>(PointDiagnostic.Probes[
                        static_cast<size_t>(PointDiagnostic.RiskSlot)].ProbeIndex)
                    : -1;
                // Omite o peso do volume no caso comum de contribuição integral.
                const QString VolumeNote = PointDiagnostic.VolumeWeight >= 0.999f
                    ? QString()
                    : QStringLiteral("  •  borda do volume (%1% DDGI)")
                          .arg(Locale.toString(PointDiagnostic.VolumeWeight * 100.0f, 'f', 0));
                const bool Blended = PointDiagnostic.NextCascade != PointDiagnostic.PrimaryCascade;
                const QString CascadeNote = Blended
                    ? QStringLiteral("  •  c%1 %2% + c%3 %4%")
                          .arg(PointDiagnostic.PrimaryCascade)
                          .arg(Locale.toString(PointDiagnostic.PrimaryWeight * 100.0f, 'f', 0))
                          .arg(PointDiagnostic.NextCascade)
                          .arg(Locale.toString(
                              (1.0f - PointDiagnostic.PrimaryWeight) * 100.0f, 'f', 0))
                    : QStringLiteral("  •  c%1 100%").arg(PointDiagnostic.PrimaryCascade);
                DebugProbePointSummary = QStringLiteral(
                    "ponto %1 · %2 · %3 m%4  •  dominante #%5%6%7")
                    .arg(Locale.toString(PointDiagnostic.WorldPosition.X, 'f', 2))
                    .arg(Locale.toString(PointDiagnostic.WorldPosition.Y, 'f', 2))
                    .arg(Locale.toString(PointDiagnostic.WorldPosition.Z, 'f', 2))
                    .arg(CascadeNote)
                    .arg(DominantIndex)
                    .arg(RiskIndex >= 0
                        ? QStringLiteral("  •  maior risco #%1").arg(RiskIndex)
                        : QStringLiteral("  •  sem risco relevante"))
                    .arg(VolumeNote);
                InvalidateDebugPreview();
                emit DebugSettingsChanged();
            }
            emit DebugProbePointChanged();
        }

        // Consome o resultado assíncrono do ID-buffer.
        int PickedIndex = -1;
        if (Renderer->TryGetPickResult(PickedIndex)) {
            Renderer->ClearLightSelection();
            if (PickedIndex >= 0) {
                Renderer->SetSelectedObject(PickedIndex);
                const auto& Renderables = Renderer->GetScene().Renderables();
                if (PickedIndex < static_cast<int>(Renderables.size())) {
                    Smile::LogDebug("Selecionado [" + std::to_string(PickedIndex) + "] " +
                                   Renderables[static_cast<size_t>(PickedIndex)].Name);
                }
            } else {
                Renderer->ClearSelection();
                Smile::LogDebug("Selecao limpa (clique no vazio)");
            }
            emit ObjectSelected(PickedIndex);
        }

        // EMA reduz a oscilação do valor exibido.
        const float InstFPS = LastFrameDeltaTime > 0.0f ? 1.0f / LastFrameDeltaTime : 0.0f;
        LastFPS = (LastFPS > 0.0f) ? (LastFPS * 0.96f + InstFPS * 0.04f) : InstFPS;

        emit FrameReady();
    }

    void ViewportWidget::keyPressEvent(QKeyEvent* _Event) {
        // Ações discretas ignoram autorepeat; HeldKeys permanece exclusivo do voo da câmera.
        if (!_Event->isAutoRepeat()) {
            if (_Event->key() == Qt::Key_Delete) {
                emit DeleteSelectionRequested();
                _Event->accept();
                return;
            }
            if (_Event->key() == Qt::Key_D && (_Event->modifiers() & Qt::ControlModifier)) {
                emit DuplicateSelectionRequested();
                _Event->accept();
                return;
            }
            if (_Event->key() == Qt::Key_QuoteLeft &&
                (_Event->modifiers() & Qt::ControlModifier)) {
                ToggleGizmoSpace();
                _Event->accept();
                return;
            }
            // Q/W/E/R trocam a ferramenta quando mouse-look e modificadores estão inativos.
            if (_Event->modifiers() == Qt::NoModifier && !MouseLookActive) {
                switch (_Event->key()) {
                    case Qt::Key_Q: SetGizmoMode(0); break;
                    case Qt::Key_W: SetGizmoMode(1); break;
                    case Qt::Key_E: SetGizmoMode(2); break;
                    case Qt::Key_R: SetGizmoMode(3); break;
                    default: break;
                }
            }
        }
        if (!_Event->isAutoRepeat())
            HeldKeys.insert(_Event->key());
        QWidget::keyPressEvent(_Event);
    }

    void ViewportWidget::keyReleaseEvent(QKeyEvent* _Event) {
        if (!_Event->isAutoRepeat())
            HeldKeys.remove(_Event->key());
        QWidget::keyReleaseEvent(_Event);
    }

    void ViewportWidget::FlushPendingGizmoInput(Smile::Renderer& _Renderer) {
        if (GizmoMousePending) {
            GizmoCtrl.SetSnapInverted(PendingGizmoSnapInverted);
            GizmoCtrl.OnMouseMove(_Renderer, PendingGizmoMouseX, PendingGizmoMouseY);
            GizmoMousePending = false;
        }
        if (GizmoReleasePending) {
            const bool Edited = GizmoCtrl.OnMouseRelease(_Renderer);
            GizmoReleasePending = false;
            if (Edited) emit SceneEdited();
        }
    }

    void ViewportWidget::mousePressEvent(QMouseEvent* _Event) {
        if (_Event->button() == Qt::RightButton) {
            if (DebugProbePointPickArmed) {
                ResetDebugProbePoint();
                _Event->accept();
                return;
            }
            MouseLookActive = true;
            IgnoreNextMove  = false;
            setCursor(Qt::BlankCursor);
            QCursor::setPos(mapToGlobal(rect().center()));
            IgnoreNextMove = true;
            setFocus();
        }
        // Coordenadas lógicas mapeiam 1:1 para o ID-buffer da viewport.
        else if (_Event->button() == Qt::LeftButton && !MouseLookActive) {
            if (Renderer) {
                auto RendererAccess = Renderer.Lock();
                if (!RendererAccess || !RendererAccess->IsInitialized()) {
                    QWidget::mousePressEvent(_Event);
                    return;
                }
                const QPointF P = _Event->position();
                const unsigned int Px = static_cast<unsigned int>(P.x() > 0.0 ? P.x() : 0.0);
                const unsigned int Py = static_cast<unsigned int>(P.y() > 0.0 ? P.y() : 0.0);
                FlushPendingGizmoInput(*RendererAccess);
                if (DebugProbePointPickArmed) {
                    DebugProbePointPickArmed = false;
                    DebugProbeContributors.clear();
                    DebugProbeContributorCount = 0;
                    DebugProbeContributorRiskSlot = -1;
                    // O foco temporário não substitui a seleção explícita da sessão.
                    if (Renderer->RequestDebugProbePoint(Px, Py)) {
                        DebugProbePointSummary =
                            QStringLiteral("Lendo o ponto da cena...");
                    } else {
                        DebugProbePointSummary =
                            QStringLiteral("Não foi possível iniciar o diagnóstico");
                    }
                    unsetCursor();
                    emit DebugProbePointChanged();
                    setFocus();
                    _Event->accept();
                    return;
                }
                // Ordem de picking: handle, ícone de luz e ID-buffer.
                if (!GizmoCtrl.OnMousePress(*RendererAccess, Px, Py)) {
                    const int LightHit = GizmoCtrl.PickLightIcon(*RendererAccess, Px, Py);
                    if (LightHit >= 0) {
                        Renderer->SetSelectedLight(LightHit);
                        Renderer->ClearSelection();
                        const auto& Lights = Renderer->GetScene().Lights();
                        Smile::LogDebug("Luz selecionada [" + std::to_string(LightHit) + "] " +
                                       Lights[static_cast<size_t>(LightHit)].Name);
                    } else {
                        Renderer->RequestPick(Px, Py);
                    }
                }
            }
            setFocus();
        }
        QWidget::mousePressEvent(_Event);
    }

    void ViewportWidget::mouseReleaseEvent(QMouseEvent* _Event) {
        if (_Event->button() == Qt::RightButton) {
            MouseLookActive = false;
            MouseDelta      = Smile::Vec2::Zero();
            unsetCursor();
        }
        else if (_Event->button() == Qt::LeftButton) {
            if (GizmoCtrl.IsDragging() && Renderer) {
                const QPointF P = _Event->position();
                PendingGizmoMouseX = static_cast<Smile::u32>(P.x() > 0.0 ? P.x() : 0.0);
                PendingGizmoMouseY = static_cast<Smile::u32>(P.y() > 0.0 ? P.y() : 0.0);
                GizmoMousePending = true;
                PendingGizmoSnapInverted = (_Event->modifiers() & Qt::ControlModifier) != 0;
                GizmoReleasePending = true;

                // Caso ocupado, OnRenderTimer aplica a posição final e encerra o gesto.
                auto RendererAccess = Renderer.TryLock();
                if (RendererAccess && RendererAccess->IsInitialized())
                    FlushPendingGizmoInput(*RendererAccess);
            } else {
                // O reset também segue o caminho diferido para sincronizar com o CSM.
                GizmoReleasePending = true;
                auto RendererAccess = Renderer ? Renderer.TryLock() : decltype(Renderer.TryLock()){};
                if (RendererAccess && RendererAccess->IsInitialized())
                    FlushPendingGizmoInput(*RendererAccess);
            }
        }
        QWidget::mouseReleaseEvent(_Event);
    }

    void ViewportWidget::mouseMoveEvent(QMouseEvent* _Event) {
        if (!MouseLookActive) {
            // Coalesce o movimento mais recente quando a render thread está ocupada.
            if (!GizmoReleasePending) {
                const QPointF P = _Event->position();
                PendingGizmoMouseX = static_cast<Smile::u32>(P.x() > 0.0 ? P.x() : 0.0);
                PendingGizmoMouseY = static_cast<Smile::u32>(P.y() > 0.0 ? P.y() : 0.0);
                GizmoMousePending = true;
                PendingGizmoSnapInverted = (_Event->modifiers() & Qt::ControlModifier) != 0;
            }
            auto RendererAccess = Renderer.TryLock();
            if (RendererAccess && RendererAccess->IsInitialized()) {
                FlushPendingGizmoInput(*RendererAccess);
            }
            QWidget::mouseMoveEvent(_Event);
            return;
        }

        if (IgnoreNextMove) {
            IgnoreNextMove = false;
            QWidget::mouseMoveEvent(_Event);
            return;
        }

        QPoint Center = mapToGlobal(rect().center());
        QPoint Position    = _Event->globalPosition().toPoint();

        MouseDelta.X += static_cast<float>(Position.x() - Center.x());
        MouseDelta.Y += static_cast<float>(Position.y() - Center.y());

        IgnoreNextMove = true;
        QCursor::setPos(Center);

        QWidget::mouseMoveEvent(_Event);
    }
} 
