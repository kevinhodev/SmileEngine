#include "SmileEditor/Debugging/DebugTargetsBridge.h"
#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include <QEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <vector>

namespace SmileEditor {
    DebugTargetsBridge::DebugTargetsBridge(QObject* _Parent) : QObject(_Parent) {}

    void DebugTargetsBridge::SetViewport(ViewportWidget* _Viewport) {
        if (Viewport == _Viewport) return;

        if (Viewport) {
            if (DebugProbeSessionActive) ClearDebugProbeInspection();
            if (Renderer) Renderer->SetDebugPreviewEnabled(false);
            Viewport->removeEventFilter(this);
            disconnect(Viewport, nullptr, this, nullptr);
        }

        Viewport = _Viewport;
        Renderer = Viewport ? Viewport->GetRenderer() : RendererHandle{};
        InvalidatePreview();

        if (!Viewport) {
            emit Updated();
            return;
        }

        Viewport->installEventFilter(this);
        connect(Viewport, &ViewportWidget::FrameReady,
                this, &DebugTargetsBridge::ConsumeReadbacks);
        connect(Viewport, &ViewportWidget::RendererResourcesChanged,
                this, &DebugTargetsBridge::NotifyResourcesChanged);
        connect(Viewport, &ViewportWidget::ViewStateChanged,
                this, &DebugTargetsBridge::Updated);
        connect(Viewport, &QObject::destroyed, this, [this]() {
            Viewport = nullptr;
            Renderer = RendererHandle{};
            DebugProbeSessionActive = false;
            ResetDebugProbePoint(false);
            InvalidatePreview();
            emit Updated();
        });

        if (Renderer) Renderer->SetDebugPreviewEnabled(PreviewEnabled);
        emit Updated();
    }

    void DebugTargetsBridge::SetPreviewEnabled(bool _Enabled) {
        PreviewEnabled = _Enabled;
        if (!Renderer) return;
        if (!_Enabled && DebugProbeSessionActive) ClearDebugProbeInspection();
        Renderer->SetDebugPreviewEnabled(_Enabled);
        if (_Enabled) InvalidatePreview();
    }

    void DebugTargetsBridge::NotifyResourcesChanged() {
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        emit Updated();
    }

    QStringList DebugTargetsBridge::GetDebugTargetNames() const {
        QStringList Names;
        for (const Smile::FDebugTarget& Target : Smile::DebugTargets::All())
            Names << QString::fromStdString(Target.Name);
        return Names;
    }

    int DebugTargetsBridge::GetDebugTargetIndex() const {
        if (!Renderer) return -1;
        const Smile::u32 Index = Renderer->GetDebugTargetIndex();
        return Index == Smile::Renderer::kNoDebugTarget ? -1 : static_cast<int>(Index);
    }

    bool DebugTargetsBridge::IsRtShaderTimerAvailable() const {
        return Renderer && Renderer->IsRtShaderTimerAvailable();
    }

    bool DebugTargetsBridge::IsRtShaderTimerEnabled() const {
        return Renderer && Renderer->GetRtShaderTimer();
    }

    bool DebugTargetsBridge::IsBvhDebugAvailable() const {
        return Renderer && Renderer->IsBvhDebugAvailable();
    }

    bool DebugTargetsBridge::IsBvhDebugEnabled() const {
        return Renderer && Renderer->GetBvhDebug();
    }

    int DebugTargetsBridge::GetBvhDebugMode() const {
        return Renderer ? static_cast<int>(Renderer->GetBvhDebugMode()) : 0;
    }

    QVariantList DebugTargetsBridge::GetDebugSelection() const {
        QVariantList Selection;
        if (!Renderer) return Selection;
        auto Access = Renderer.Lock();
        for (Smile::u32 Index : Access->GetDebugSelection())
            Selection << static_cast<int>(Index);
        return Selection;
    }

    int DebugTargetsBridge::GetDebugColumns() const {
        return Renderer ? static_cast<int>(Renderer->GetDebugColumns()) : 0;
    }

    double DebugTargetsBridge::GetDebugExposure() const {
        return Renderer ? static_cast<double>(Renderer->GetDebugExposure()) : 1.0;
    }

    QString DebugTargetsBridge::GetViewModeLabel() const {
        const int TargetIndex = GetDebugTargetIndex();
        if (TargetIndex >= 0) {
            const QStringList Names = GetDebugTargetNames();
            if (TargetIndex < Names.size()) return Names[TargetIndex];
        }
        return Viewport ? Viewport->GetViewModeLabel() : QStringLiteral("Lit");
    }

    bool DebugTargetsBridge::GetDebugProbeCoordValues(FDebugProbeCoord& _Out) const {
        if (!Renderer || !DebugProbeSessionActive) return false;
        auto Access = Renderer.Lock();
        const auto& DDGI = Access->GetDDGI();
        const Smile::u32 Index = Access->GetDebugProbeIndex();
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
        _Out.Cascade = static_cast<int>(Index) / PerCascade;
        _Out.LocalIndex = static_cast<int>(Index) - _Out.Cascade * PerCascade;

        const int XY = _Out.CountX * _Out.CountY;
        const int StorageZ = _Out.LocalIndex / XY;
        const int Remainder = _Out.LocalIndex - StorageZ * XY;
        const int StorageY = Remainder / _Out.CountX;
        const int StorageX = Remainder - StorageY * _Out.CountX;
        const int Scroll[3] = {
            DDGI.CascadeScroll(static_cast<Smile::u32>(_Out.Cascade), 0),
            DDGI.CascadeScroll(static_cast<Smile::u32>(_Out.Cascade), 1),
            DDGI.CascadeScroll(static_cast<Smile::u32>(_Out.Cascade), 2)
        };
        const int Counts[3] = { _Out.CountX, _Out.CountY, _Out.CountZ };
        int Geo[3] = { StorageX, StorageY, StorageZ };
        for (int Axis = 0; Axis < 3; ++Axis) {
            Geo[Axis] -= Scroll[Axis];
            if (Geo[Axis] < 0) Geo[Axis] += Counts[Axis];
        }
        _Out.X = Geo[0];
        _Out.Y = Geo[1];
        _Out.Z = Geo[2];
        _Out.GridMin = DDGI.CascadeGridMin(static_cast<Smile::u32>(_Out.Cascade));
        _Out.Spacing = DDGI.CascadeSpacing(static_cast<Smile::u32>(_Out.Cascade));
        return true;
    }

    int DebugTargetsBridge::GetDebugProbeIndex() const {
        if (!Renderer || !DebugProbeSessionActive) return -1;
        const Smile::u32 Index = Renderer->GetDebugProbeIndex();
        return Index == Smile::Renderer::kNoDebugProbe ? -1 : static_cast<int>(Index);
    }

    QString DebugTargetsBridge::GetDebugProbeCoord() const {
        FDebugProbeCoord Coord;
        if (!GetDebugProbeCoordValues(Coord)) return {};
        return QStringLiteral("cascata %1 · grid (%2, %3, %4)")
            .arg(Coord.Cascade).arg(Coord.X).arg(Coord.Y).arg(Coord.Z);
    }

    QString DebugTargetsBridge::GetDebugProbeWorld() const {
        FDebugProbeCoord Coord;
        if (!GetDebugProbeCoordValues(Coord)) return {};
        const double Spacing = Coord.Spacing;
        const QLocale Locale;
        return QStringLiteral("posição-base %1 · %2 · %3 m")
            .arg(Locale.toString(Coord.GridMin.X + Coord.X * Spacing, 'f', 2))
            .arg(Locale.toString(Coord.GridMin.Y + Coord.Y * Spacing, 'f', 2))
            .arg(Locale.toString(Coord.GridMin.Z + Coord.Z * Spacing, 'f', 2));
    }

    QString DebugTargetsBridge::GetDebugProbeGrid() const {
        FDebugProbeCoord Coord;
        if (!GetDebugProbeCoordValues(Coord)) return {};
        return QStringLiteral("%1 × %2 × %3 probes · spacing %4 m")
            .arg(Coord.CountX).arg(Coord.CountY).arg(Coord.CountZ)
            .arg(QLocale().toString(static_cast<double>(Coord.Spacing), 'f', 2));
    }

    QString DebugTargetsBridge::GetDebugProbeDistanceRange() const {
        FDebugProbeCoord Coord;
        if (!GetDebugProbeCoordValues(Coord)) return {};
        return QStringLiteral("distância média · 0 → %1 m")
            .arg(QLocale().toString(static_cast<double>(Coord.Spacing) * 2.6, 'f', 2));
    }

    bool DebugTargetsBridge::IsDebugPreviewReady() const {
        QMutexLocker Lock(&DebugPreviewMutex);
        return !DebugPreviewImage.isNull();
    }

    QImage DebugTargetsBridge::PreviewImageCopy() const {
        QMutexLocker Lock(&DebugPreviewMutex);
        return DebugPreviewImage.copy();
    }

    void DebugTargetsBridge::InvalidatePreview() {
        {
            QMutexLocker Lock(&DebugPreviewMutex);
            DebugPreviewImage = {};
        }
        ++DebugPreviewSeq;
        emit PreviewUpdated();
    }

    void DebugTargetsBridge::ToggleDebugSelection(int _Index) {
        if (!Renderer || _Index < 0) return;
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        auto Access = Renderer.Lock();
        std::vector<Smile::u32> Selection = Access->GetDebugSelection();
        const auto It = std::find(Selection.begin(), Selection.end(),
                                  static_cast<Smile::u32>(_Index));
        if (It != Selection.end()) Selection.erase(It);
        else if (Selection.size() < 16u) Selection.push_back(static_cast<Smile::u32>(_Index));
        else return;
        Access->SetDebugSelection(Selection);
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::ClearDebugSelection() {
        if (!Renderer) return;
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        Renderer->SetDebugSelection({});
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::ToggleRtShaderTimer() {
        if (!Renderer || !Renderer->IsRtShaderTimerAvailable()) return;
        Renderer->SetRtShaderTimer(!Renderer->GetRtShaderTimer());
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::ToggleBvhDebug() {
        if (!Renderer || !Renderer->IsBvhDebugAvailable()) return;
        Renderer->SetBvhDebug(!Renderer->GetBvhDebug());
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::SetBvhDebugMode(int _Mode) {
        if (!Renderer || _Mode < 0 ||
            _Mode >= static_cast<int>(Smile::FBvhDebugView::EMode::Count)) return;
        Renderer->SetBvhDebugMode(static_cast<Smile::FBvhDebugView::EMode>(_Mode));
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::SetDebugColumns(int _Columns) {
        if (!Renderer || DebugProbeSessionActive) return;
        Renderer->SetDebugColumns(_Columns < 0 ? 0 : static_cast<Smile::u32>(_Columns));
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::SetDebugExposure(double _Exposure) {
        if (!Renderer) return;
        Renderer->SetDebugExposure(static_cast<float>(_Exposure));
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::InspectDDGIProbe(
            int _TargetIndex, double _U, double _V, double _TileAspect) {
        if (!Renderer || DebugProbeSessionActive || _TargetIndex < 0) return;
        auto Access = Renderer.Lock();
        const auto& Targets = Smile::DebugTargets::All();
        if (static_cast<size_t>(_TargetIndex) >= Targets.size()) return;
        const Smile::FDebugTarget& Clicked = Targets[static_cast<size_t>(_TargetIndex)];
        if (Clicked.AtlasTilePx == 0 ||
            (Clicked.Decode != Smile::EDebugDecode::DDGIIrradiance &&
             Clicked.Decode != Smile::EDebugDecode::DDGIDistance)) {
            return;
        }

        const auto& DDGI = Access->GetDDGI();
        if (!DDGI.IsReady()) return;
        const Smile::Vec3 Count = DDGI.GridCount();
        if (Count.X <= 0 || Count.Y <= 0 || Count.Z <= 0) return;

        const int TilesX = static_cast<int>(DDGI.AtlasTilesPerRow());
        const int TileRows = static_cast<int>(DDGI.AtlasTileRows());
        if (TilesX <= 0 || TileRows <= 0) return;
        const int Total = TilesX * TileRows;
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

        Smile::u32 Probe = 0;
        if (!DDGI.ProbeFromAtlasTile(static_cast<Smile::u32>(AtlasIndex), Probe)) return;

        DebugProbePreviousTargets.clear();
        for (Smile::u32 Index : Access->GetDebugSelection()) {
            if (Index < Targets.size())
                DebugProbePreviousTargets << QString::fromStdString(Targets[Index].Name);
        }
        DebugProbePreviousColumns = static_cast<int>(Access->GetDebugColumns());

        std::vector<Smile::u32> DetailTargets;
        for (size_t Index = 0; Index < Targets.size(); ++Index) {
            if (Targets[Index].Decode == Smile::EDebugDecode::DDGIIrradiance ||
                Targets[Index].Decode == Smile::EDebugDecode::DDGIDistance) {
                DetailTargets.push_back(static_cast<Smile::u32>(Index));
            }
        }
        if (DetailTargets.empty()) return;

        DebugProbeSessionActive = true;
        DebugProbeDirection.clear();
        DebugProbeSample.clear();
        ResetDebugProbePoint();
        Access->SetDebugSelection(DetailTargets);
        Access->SetDebugColumns(DetailTargets.size() > 1 ? 2u : 1u);
        SelectDebugProbe(static_cast<int>(Probe));
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::StepDebugProbe(int _DX, int _DY, int _DZ) {
        if (!Renderer) return;
        auto Access = Renderer.Lock();
        FDebugProbeCoord Coord;
        if (!GetDebugProbeCoordValues(Coord)) return;
        const int Counts[3] = { Coord.CountX, Coord.CountY, Coord.CountZ };
        int Geo[3] = {
            std::clamp(Coord.X + _DX, 0, Coord.CountX - 1),
            std::clamp(Coord.Y + _DY, 0, Coord.CountY - 1),
            std::clamp(Coord.Z + _DZ, 0, Coord.CountZ - 1)
        };
        const auto& DDGI = Access->GetDDGI();
        for (int Axis = 0; Axis < 3; ++Axis) {
            Geo[Axis] += DDGI.CascadeScroll(static_cast<Smile::u32>(Coord.Cascade), Axis);
            if (Geo[Axis] >= Counts[Axis]) Geo[Axis] -= Counts[Axis];
        }
        const int PerCascade = Coord.CountX * Coord.CountY * Coord.CountZ;
        const int Index = Coord.Cascade * PerCascade +
            Geo[0] + Geo[1] * Coord.CountX + Geo[2] * Coord.CountX * Coord.CountY;
        if (Index == GetDebugProbeIndex()) return;
        ResetDebugProbePoint();
        SelectDebugProbe(Index);
        DebugProbeSample.clear();
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::ClearDebugProbeInspection() {
        if (!Renderer || !DebugProbeSessionActive) return;
        auto Access = Renderer.Lock();
        ResetDebugProbePoint();
        SelectDebugProbe(-1);

        std::vector<Smile::u32> Restored;
        Restored.reserve(static_cast<size_t>(DebugProbePreviousTargets.size()));
        for (const QString& Name : DebugProbePreviousTargets) {
            const Smile::u32 Index = Smile::DebugTargets::IndexOf(Name.toStdString());
            if (Index != Smile::DebugTargets::kInvalid) Restored.push_back(Index);
        }
        Access->SetDebugSelection(Restored);
        Access->SetDebugColumns(DebugProbePreviousColumns < 0
            ? 0u : static_cast<Smile::u32>(DebugProbePreviousColumns));

        DebugProbeSessionActive = false;
        DebugProbePreviousTargets.clear();
        DebugProbeDirection.clear();
        DebugProbeSample.clear();
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::SelectDebugProbe(int _ProbeIndex) {
        if (!Renderer) return;
        Renderer->SetDebugProbeIndex(_ProbeIndex);
        DebugProbeBaseIndex = _ProbeIndex;
    }

    void DebugTargetsBridge::ResetDebugProbePoint(bool _CancelRendererRequest) {
        const bool Changed = DebugProbePointPickArmed ||
                             !DebugProbePointSummary.isEmpty() ||
                             !DebugProbeContributors.isEmpty();
        if (Renderer && _CancelRendererRequest) {
            auto Access = Renderer.Lock();
            Access->CancelDebugProbePoint();
            if (DebugProbeSessionActive)
                Access->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
        }
        DebugProbePointPickArmed = false;
        DebugProbePointSummary.clear();
        DebugProbeContributors.clear();
        DebugProbeContributorIndices.fill(0);
        DebugProbeContributorWeights.fill(0.0f);
        DebugProbeContributorCount = 0;
        DebugProbeContributorRiskSlot = -1;
        if (Viewport) Viewport->unsetCursor();
        if (Changed) emit Updated();
    }

    void DebugTargetsBridge::ArmDebugProbePointPick() {
        if (!Viewport || !Renderer || !Renderer->IsInitialized() ||
            !DebugProbeSessionActive) return;
        if (DebugProbePointPickArmed) {
            ResetDebugProbePoint();
            return;
        }

        {
            auto Access = Renderer.Lock();
            Access->CancelDebugProbePoint();
            Access->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
        }
        DebugProbeContributors.clear();
        DebugProbeContributorIndices.fill(0);
        DebugProbeContributorWeights.fill(0.0f);
        DebugProbeContributorCount = 0;
        DebugProbeContributorRiskSlot = -1;
        DebugProbePointPickArmed = true;
        DebugProbePointSummary =
            QStringLiteral("Clique em uma superfície no viewport para diagnosticar o DDGI");
        Viewport->setCursor(Qt::CrossCursor);
        emit Updated();
    }

    void DebugTargetsBridge::ClearDebugProbePoint() {
        ResetDebugProbePoint();
    }

    void DebugTargetsBridge::SelectDebugProbeContributor(int _ProbeIndex) {
        if (!Renderer || !DebugProbeSessionActive || _ProbeIndex < 0) return;
        bool Found = false;
        for (Smile::u32 Index = 0; Index < DebugProbeContributorCount; ++Index) {
            if (DebugProbeContributorIndices[Index] == static_cast<Smile::u32>(_ProbeIndex)) {
                Found = true;
                break;
            }
        }
        if (!Found) return;

        auto Access = Renderer.Lock();
        SelectDebugProbe(_ProbeIndex);
        Access->SetDebugProbeContributors(
            DebugProbeContributorIndices.data(), DebugProbeContributorWeights.data(),
            DebugProbeContributorCount, DebugProbeContributorRiskSlot);
        InvalidatePreview();
        emit Updated();
    }

    void DebugTargetsBridge::UpdateDebugProbeDirection(double _U, double _V) {
        QString NewLabel;
        const bool Valid = DebugProbeSessionActive && _U >= 0.0 && _U <= 1.0 &&
                           _V >= 0.0 && _V <= 1.0;
        if (Renderer) Renderer->SetDebugProbeSampleUV(
            Valid ? static_cast<float>(_U) : -1.0f,
            Valid ? static_cast<float>(_V) : -1.0f);
        if (Valid) {
            double X = _U * 2.0 - 1.0;
            double Y = _V * 2.0 - 1.0;
            double Z = 1.0 - std::abs(X) - std::abs(Y);
            const double T = std::clamp(-Z, 0.0, 1.0);
            X += X >= 0.0 ? -T : T;
            Y += Y >= 0.0 ? -T : T;
            const double Length = std::sqrt(X * X + Y * Y + Z * Z);
            if (Length > 1e-8) { X /= Length; Y /= Length; Z /= Length; }
            const QLocale Locale;
            NewLabel = QStringLiteral("direção (%1, %2, %3)")
                .arg(Locale.toString(X, 'f', 2))
                .arg(Locale.toString(Y, 'f', 2))
                .arg(Locale.toString(Z, 'f', 2));
        }
        if (DebugProbeDirection == NewLabel && DebugProbeSample.isEmpty()) return;
        DebugProbeDirection = NewLabel;
        DebugProbeSample.clear();
        emit Updated();
    }

    void DebugTargetsBridge::SelectDebugTarget(int _Index) {
        if (!Renderer) return;
        if (Viewport && _Index >= 0) Viewport->SelectLit();
        Renderer->SetDebugTargetIndex(_Index < 0
            ? Smile::Renderer::kNoDebugTarget : static_cast<Smile::u32>(_Index));
        emit Updated();
    }

    bool DebugTargetsBridge::eventFilter(QObject* _Watched, QEvent* _Event) {
        if (_Watched != Viewport || !DebugProbePointPickArmed ||
            _Event->type() != QEvent::MouseButtonPress) {
            return QObject::eventFilter(_Watched, _Event);
        }

        auto* Mouse = static_cast<QMouseEvent*>(_Event);
        if (Mouse->button() == Qt::RightButton) {
            ResetDebugProbePoint();
            Mouse->accept();
            return true;
        }
        if (Mouse->button() != Qt::LeftButton || !Renderer) return false;

        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) return false;
        const QPointF Position = Mouse->position();
        const Smile::u32 X = static_cast<Smile::u32>(std::max(Position.x(), 0.0));
        const Smile::u32 Y = static_cast<Smile::u32>(std::max(Position.y(), 0.0));
        DebugProbePointPickArmed = false;
        DebugProbeContributors.clear();
        DebugProbeContributorCount = 0;
        DebugProbeContributorRiskSlot = -1;
        DebugProbePointSummary = Access->RequestDebugProbePoint(X, Y)
            ? QStringLiteral("Lendo o ponto da cena...")
            : QStringLiteral("Não foi possível iniciar o diagnóstico");
        Viewport->unsetCursor();
        Viewport->setFocus();
        emit Updated();
        Mouse->accept();
        return true;
    }

    void DebugTargetsBridge::ConsumeReadbacks() {
        if (!Renderer) return;
        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) return;

        std::vector<Smile::u8> Pixels;
        if (Access->ConsumeDebugPreview(Pixels)) {
            const size_t Expected =
                static_cast<size_t>(Smile::Renderer::kDebugPreviewWidth) *
                Smile::Renderer::kDebugPreviewHeight * 4u;
            if (Pixels.size() == Expected) {
                QImage Image(Pixels.data(),
                             static_cast<int>(Smile::Renderer::kDebugPreviewWidth),
                             static_cast<int>(Smile::Renderer::kDebugPreviewHeight),
                             QImage::Format_RGBA8888);
                {
                    QMutexLocker Lock(&DebugPreviewMutex);
                    DebugPreviewImage = Image.copy();
                }
                ++DebugPreviewSeq;
                emit PreviewUpdated();
            }
        }

        bool StateChanged = false;
        Smile::Renderer::FDebugProbeSample ProbeSample;
        if (Access->ConsumeDebugProbeSample(ProbeSample) &&
            DebugProbeSessionActive &&
            ProbeSample.ProbeIndex == Access->GetDebugProbeIndex()) {
            const QLocale Locale;
            DebugProbeSample = QStringLiteral(
                "irr %1 · %2 · %3  •  distância %4 m  •  σ %5 m")
                .arg(Locale.toString(ProbeSample.Irradiance[0], 'f', 3))
                .arg(Locale.toString(ProbeSample.Irradiance[1], 'f', 3))
                .arg(Locale.toString(ProbeSample.Irradiance[2], 'f', 3))
                .arg(Locale.toString(ProbeSample.MeanDistance, 'f', 2))
                .arg(Locale.toString(ProbeSample.DistanceDeviation, 'f', 2));
            StateChanged = true;
        }

        Smile::FDDGIPointDiagnostic PointDiagnostic;
        if (Access->ConsumeDebugProbePoint(PointDiagnostic) && DebugProbeSessionActive) {
            DebugProbeContributors.clear();
            DebugProbeContributorIndices.fill(0);
            DebugProbeContributorWeights.fill(0.0f);
            DebugProbeContributorCount = 0;
            DebugProbeContributorRiskSlot = -1;

            if (!PointDiagnostic.Valid) {
                DebugProbePointSummary =
                    QStringLiteral("Nenhuma superfície encontrada nesse pixel");
                Access->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
            } else if (PointDiagnostic.VolumeWeight <= 0.001f) {
                DebugProbePointSummary = QStringLiteral(
                    "ponto %1 · %2 · %3 m  •  fora do volume de sondas — só ambiente")
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.X, 'f', 2))
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.Y, 'f', 2))
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.Z, 'f', 2));
                if (DebugProbeBaseIndex >= 0)
                    Access->SetDebugProbeIndex(DebugProbeBaseIndex);
                Access->ClearDebugProbeContributors();
                InvalidatePreview();
            } else {
                const auto& DDGI = Access->GetDDGI();
                const Smile::Vec3 GridCount = DDGI.GridCount();
                const int CountX = std::max(1, static_cast<int>(GridCount.X));
                const int CountY = std::max(1, static_cast<int>(GridCount.Y));
                const int XY = CountX * CountY;
                const QLocale Locale;

                for (Smile::u32 Slot = 0;
                     Slot < Smile::FDDGIDebug::kPointProbeCount; ++Slot) {
                    const Smile::FDDGIPointProbeDiagnostic& Probe =
                        PointDiagnostic.Probes[Slot];
                    const int Index = static_cast<int>(Probe.ProbeIndex);
                    const int PerCascade = std::max(
                        1, XY * std::max(1, static_cast<int>(GridCount.Z)));
                    const int Cascade = Index / PerCascade;
                    const int Local = Index - Cascade * PerCascade;
                    const int Z = Local / XY;
                    const int Remainder = Local - Z * XY;
                    const int Y = Remainder / CountX;
                    const int X = Remainder - Y * CountX;

                    QVariantMap Item;
                    Item.insert(QStringLiteral("probeIndex"), Index);
                    Item.insert(QStringLiteral("cascade"), Cascade);
                    Item.insert(QStringLiteral("coord"),
                                QStringLiteral("c%1 (%2,%3,%4)")
                                    .arg(Cascade).arg(X).arg(Y).arg(Z));
                    Item.insert(QStringLiteral("active"), Probe.Active);
                    Item.insert(QStringLiteral("dominant"),
                                static_cast<int>(Slot) == PointDiagnostic.DominantSlot);
                    Item.insert(QStringLiteral("risk"),
                                static_cast<int>(Slot) == PointDiagnostic.RiskSlot);
                    Item.insert(QStringLiteral("distance"), Probe.DistanceToPoint);
                    Item.insert(QStringLiteral("mean"), Probe.MeanDistance);
                    Item.insert(QStringLiteral("sigma"), Probe.DistanceDeviation);
                    Item.insert(QStringLiteral("trilinear"), Probe.TrilinearWeight);
                    Item.insert(QStringLiteral("visibility"), Probe.Visibility);
                    Item.insert(QStringLiteral("weight"), Probe.NormalizedWeight);
                    Item.insert(QStringLiteral("leakRisk"), Probe.LeakRisk);
                    Item.insert(QStringLiteral("irradiance"),
                                QStringLiteral("%1 · %2 · %3")
                                    .arg(Locale.toString(Probe.Irradiance.X, 'f', 2))
                                    .arg(Locale.toString(Probe.Irradiance.Y, 'f', 2))
                                    .arg(Locale.toString(Probe.Irradiance.Z, 'f', 2)));
                    DebugProbeContributors.append(Item);

                    if (Probe.Active &&
                        DebugProbeContributorCount < DebugProbeContributorIndices.size()) {
                        if (static_cast<int>(Slot) == PointDiagnostic.RiskSlot)
                            DebugProbeContributorRiskSlot =
                                static_cast<int>(DebugProbeContributorCount);
                        DebugProbeContributorIndices[DebugProbeContributorCount] =
                            Probe.ProbeIndex;
                        DebugProbeContributorWeights[DebugProbeContributorCount] =
                            Probe.NormalizedWeight;
                        ++DebugProbeContributorCount;
                    }
                }

                const int FocusSlot = PointDiagnostic.RiskSlot >= 0
                    ? PointDiagnostic.RiskSlot : PointDiagnostic.DominantSlot;
                if (FocusSlot >= 0 &&
                    FocusSlot < static_cast<int>(Smile::FDDGIDebug::kPointProbeCount)) {
                    Access->SetDebugProbeIndex(static_cast<int>(
                        PointDiagnostic.Probes[static_cast<size_t>(FocusSlot)].ProbeIndex));
                }
                Access->SetDebugProbeContributors(
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
                const QString VolumeNote = PointDiagnostic.VolumeWeight >= 0.999f
                    ? QString()
                    : QStringLiteral("  •  borda do volume (%1% DDGI)")
                          .arg(Locale.toString(PointDiagnostic.VolumeWeight * 100.0f, 'f', 0));
                const bool Blended =
                    PointDiagnostic.NextCascade != PointDiagnostic.PrimaryCascade;
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
                InvalidatePreview();
            }
            StateChanged = true;
        }

        if (StateChanged) emit Updated();
    }
}
