#include "SmileEditor/ViewportWidget.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/VramTracker.h"
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
#include <QFileDialog>
#include <QMutexLocker>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace SmileEditor {
    static constexpr float kMouseSensitivity = 0.15f;  

    ViewportWidget::ViewportWidget(QWidget* _Parent)
        : QWidget(_Parent),
          Renderer(std::make_unique<Smile::Renderer>())
    {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent);

        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 200);
        setMouseTracking(true); // hover do gizmo precisa de mouse-move sem botao pressionado

        // Interval 0: renderiza continuamente (dispara quando a fila de eventos esvazia,
        // sem starvar input/resize). O pacing fica a cargo do Present: com VSync ligado
        // ele trava no vblank; desligado, roda em FPS livre.
        RedrawTimer = new QTimer(this);
        RedrawTimer->setInterval(0);
        connect(RedrawTimer, &QTimer::timeout, this, &ViewportWidget::OnRenderTimer);

        // Editor em segundo plano (nenhuma janela nossa com foco): cai pra ~10fps em vez
        // de queimar GPU em FPS livre enquanto o usuario esta em outro app (estilo "Use
        // Less CPU when in Background" da UE). Minimizado ja para de vez (hideEvent).
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState State) {
            RedrawTimer->setInterval(State == Qt::ApplicationActive ? 0 : 100);
        });

        FrameTimer.start();
    }

    ViewportWidget::~ViewportWidget() {
        if (RedrawTimer) RedrawTimer->stop();
        // O destrutor noexcept do Renderer centraliza o shutdown e absorve falhas tardias.
    }

    QPaintEngine* ViewportWidget::paintEngine() const {
        return nullptr;
    }

    QString ViewportWidget::GetViewModeLabel() const {
        // Um alvo de debug ativo e o que a tela mostra, entao ele manda no rotulo.
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

    bool ViewportWidget::IsDDGIEnabled() const {
        return Renderer && Renderer->GetUseGI();
    }

    bool ViewportWidget::IsReSTIRGIEnabled() const {
        return Renderer && Renderer->GetUseReSTIRGI();
    }

    bool ViewportWidget::IsReGIREnabled() const {
        return Renderer && Renderer->GetUseReGIR();
    }

    bool ViewportWidget::IsReSTIRGIVisibilityEnabled() const {
        return Renderer && Renderer->GetReSTIRGI().GetVisibility();
    }

    bool ViewportWidget::AreGIFoliageShadowsEnabled() const {
        return Renderer && Renderer->GetDDGI().GetFoliageShadows();
    }

    bool ViewportWidget::IsReflectionsCullBackfaceEnabled() const {
        return Renderer && Renderer->GetReflectionsCullBackface();
    }

    bool ViewportWidget::IsReSTIRDIEnabled() const {
        return Renderer && Renderer->GetUseReSTIRDI();
    }

    bool ViewportWidget::IsGIBackfacePolicyEnabled() const {
        return Renderer && Renderer->GetGIBackfacePolicy();
    }

    double ViewportWidget::GetGISurfaceBiasMax() const {
        return Renderer ? static_cast<double>(Renderer->GetGISurfaceBiasMax()) : 0.0;
    }

    double ViewportWidget::GetGIVolumeFadeProbes() const {
        return Renderer ? static_cast<double>(Renderer->GetGIVolumeFadeProbes()) : 0.0;
    }

    bool ViewportWidget::IsGTAOEnabled() const {
        return Renderer && Renderer->GetUseAO();
    }

    bool ViewportWidget::IsGTAOHalfRes() const {
        return Renderer && Renderer->GetAO().GetHalfRes();
    }

    bool ViewportWidget::AreReflectionsEnabled() const {
        return Renderer && Renderer->GetUseReflections();
    }

    bool ViewportWidget::IsNrdEnabled() const {
        return Renderer && Renderer->GetUseNrdDenoise();
    }

    int ViewportWidget::GetDenoiserMode() const {
        return Renderer ? static_cast<int>(Renderer->GetDenoiser()) : 0;  // 0=None 1=NRD 2=DLSS RR
    }

    bool ViewportWidget::IsRRAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->RRAvailable();
    }

    int ViewportWidget::GetUpscalerMode() const {
        return Renderer ? static_cast<int>(Renderer->GetUpscaler()) : 0;  // 0=None 1=FSR 2=DLSS
    }

    bool ViewportWidget::IsFsrAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->UpscalerAvailable(Smile::EUpscaler::FSR);
    }

    bool ViewportWidget::IsDlssAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->UpscalerAvailable(Smile::EUpscaler::DLSS);
    }

    int ViewportWidget::GetUpscalerQuality() const {
        return Renderer ? Renderer->GetUpscalerQuality() : 0;  // qualidade compartilhada FSR/DLSS
    }

    int ViewportWidget::GetRecommendedUpscalerMode() const {
        if (IsDlssAvailable()) return 2;   // DLSS preferido em NVIDIA
        if (IsFsrAvailable())  return 1;
        return 0;
    }

    QString ViewportWidget::GetRecommendedUpscalerName() const {
        if (IsDlssAvailable()) return QStringLiteral("DLSS");
        if (IsFsrAvailable())  return QStringLiteral("FSR 3.1");
        return QStringLiteral("Sem upscaling");
    }

    double ViewportWidget::GetRenderScale() const {
        return Renderer ? static_cast<double>(Renderer->GetRenderScale()) : 1.0;
    }

    bool ViewportWidget::IsTAAEnabled() const {
        return Renderer && Renderer->GetUseTAA();
    }

    bool ViewportWidget::IsFrustumCullingEnabled() const {
        return Renderer && Renderer->GetFrustumCulling();
    }

    bool ViewportWidget::IsOcclusionCullingEnabled() const {
        return Renderer && Renderer->GetOcclusionCulling();
    }

    bool ViewportWidget::IsDepthPrepassEnabled() const {
        return Renderer && Renderer->GetDepthPrepass();
    }

    double ViewportWidget::GetFrameTimeMs() const {
        return LastFPS > 0.0f ? 1000.0 / static_cast<double>(LastFPS) : 0.0;
    }

    int ViewportWidget::GetVisibleDrawCount() const {
        return Renderer ? static_cast<int>(Renderer->GetVisibleCount()) : 0;
    }

    int ViewportWidget::GetTotalDrawCount() const {
        return Renderer ? static_cast<int>(Renderer->GetDrawCount()) : 0;
    }

    int ViewportWidget::GetOccludedDrawCount() const {
        return Renderer ? static_cast<int>(Renderer->GetOccludedCount()) : 0;
    }

    QString ViewportWidget::GetInternalResolution() const {
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("—");
        return QStringLiteral("%1×%2").arg(Renderer->RenderWidth()).arg(Renderer->RenderHeight());
    }

    QString ViewportWidget::GetOutputResolution() const {
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("—");
        return QStringLiteral("%1×%2").arg(Renderer->OutputWidth()).arg(Renderer->OutputHeight());
    }

    QString ViewportWidget::GetGPUName() const {
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("Inicializando GPU…");
        return QString::fromStdWString(Renderer->GetDevice().GetAdapterDescription());
    }

    QString ViewportWidget::GetVRAMText() const {
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("—");
        const double GiB = static_cast<double>(
            Renderer->GetDevice().GetAdapterDedicatedVideoMemory()) / (1024.0 * 1024.0 * 1024.0);
        return QLocale(QLocale::Portuguese, QLocale::Brazil).toString(GiB, 'f', 1) +
               QStringLiteral(" GB");
    }

    QString ViewportWidget::GetVRAMUsageText() const {
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("—");
        const auto& VM = Renderer->GetDevice().QueryVideoMemory();
        if (!VM.Valid) return QStringLiteral("—");

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        const double GiB = 1024.0 * 1024.0 * 1024.0;
        return Loc.toString(static_cast<double>(VM.LocalUsage) / GiB, 'f', 2) +
               QStringLiteral(" / ") +
               Loc.toString(static_cast<double>(VM.LocalBudget) / GiB, 'f', 1) +
               QStringLiteral(" GB");
    }

    bool ViewportWidget::IsVRAMOverBudget() const {
        if (!Renderer || !Renderer->IsInitialized()) return false;
        return Renderer->GetDevice().QueryVideoMemory().OverBudget;
    }

    double ViewportWidget::GetVRAMBudgetFrac() const {
        if (!Renderer || !Renderer->IsInitialized()) return 0.0;
        const auto& VM = Renderer->GetDevice().QueryVideoMemory();
        if (!VM.Valid || VM.LocalBudget == 0) return 0.0;
        return std::min(1.0, static_cast<double>(VM.LocalUsage) /
                             static_cast<double>(VM.LocalBudget));
    }

    namespace {
        QString FormatBytes(Smile::u64 _Bytes) {
            const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
            const double MiB = static_cast<double>(_Bytes) / (1024.0 * 1024.0);
            if (MiB >= 1024.0) return Loc.toString(MiB / 1024.0, 'f', 2) + QStringLiteral(" GB");
            return Loc.toString(MiB, 'f', 1) + QStringLiteral(" MB");
        }
    }

    QString ViewportWidget::GetVRAMNonLocalText() const {
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("—");
        const auto& VM = Renderer->GetDevice().QueryVideoMemory();
        if (!VM.Valid) return QStringLiteral("—");
        return FormatBytes(VM.NonLocalUsage) + QStringLiteral(" / ") +
               FormatBytes(VM.NonLocalBudget);
    }

    // Tabela da janela de Estatisticas: categorias rastreadas (> 0) em ordem decrescente
    // + linha "Nao rastreado" (uso DXGI − soma rastreada: driver, descriptor heaps,
    // swapchain, upload heaps e o que nao foi instrumentado). frac e relativo ao uso total.
    QVariantList ViewportWidget::GetVRAMBreakdown() const {
        QVariantList Rows;
        if (!Renderer || !Renderer->IsInitialized()) return Rows;

        const auto& VM   = Renderer->GetDevice().QueryVideoMemory();
        const auto  Snap = Smile::VramTracker::Snapshot();
        const double Total = VM.Valid && VM.LocalUsage > 0
            ? static_cast<double>(VM.LocalUsage)
            : static_cast<double>(std::max<Smile::u64>(Snap.TotalTracked, 1));

        using Smile::EVramCategory;
        std::vector<std::pair<size_t, Smile::u64>> Sorted;
        for (size_t i = 0; i < static_cast<size_t>(EVramCategory::Count); ++i)
            if (Snap.Bytes[i] > 0) Sorted.emplace_back(i, Snap.Bytes[i]);
        std::sort(Sorted.begin(), Sorted.end(),
                  [](const auto& A, const auto& B) { return A.second > B.second; });

        for (const auto& [Index, Bytes] : Sorted) {
            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QString::fromUtf8(
                Smile::VramTracker::CategoryName(static_cast<EVramCategory>(Index))));
            Row.insert(QStringLiteral("text"), FormatBytes(Bytes));
            Row.insert(QStringLiteral("frac"), static_cast<double>(Bytes) / Total);
            Rows.push_back(Row);
        }

        if (VM.Valid && VM.LocalUsage > Snap.TotalTracked) {
            const Smile::u64 Untracked = VM.LocalUsage - Snap.TotalTracked;
            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QStringLiteral("Não rastreado"));
            Row.insert(QStringLiteral("text"), FormatBytes(Untracked));
            Row.insert(QStringLiteral("frac"), static_cast<double>(Untracked) / Total);
            Rows.push_back(Row);
        }
        return Rows;
    }

    namespace {
        constexpr const char* kGpuFrameScope = "Frame (GPU)";
    }

    QString ViewportWidget::GetGpuFrameText() const {
        const double FrameMs = GetGpuFrameMs();
        if (FrameMs <= 0.0) return QStringLiteral("—");
        return QLocale(QLocale::Portuguese, QLocale::Brazil)
                   .toString(FrameMs, 'f', 2) + QStringLiteral(" ms");
    }

    double ViewportWidget::GetGpuFrameMs() const {
        if (!Renderer || !Renderer->IsInitialized()) return 0.0;
        for (const auto& R : Renderer->GetGpuProfiler().Results())
            if (std::strcmp(R.Name, kGpuFrameScope) == 0) return R.Milliseconds;
        return 0.0;
    }

    // Tabela "GPU por passe": escopos do FGpuProfiler (sem o total, que vira o header),
    // em ordem de custo com histerese; frac relativo ao frame total de GPU. A margem conserva
    // a leitura por consumo sem deixar passes quase empatados trocarem de lugar a cada snapshot.
    QVariantList ViewportWidget::GetGpuTimings() const {
        QVariantList Rows;
        if (!Renderer || !Renderer->IsInitialized()) return Rows;
        const auto& Results = Renderer->GetGpuProfiler().Results();
        if (Results.empty()) return Rows;

        double FrameMs = 0.0;
        for (const auto& R : Results)
            if (std::strcmp(R.Name, kGpuFrameScope) == 0) FrameMs = R.Milliseconds;

        // Passes da fila de COMPUTE (DDGI async) entram na mesma tabela. O FGpuProfiler agora
        // preserva Depth, portanto os scopes aninhados viram subpasses expansíveis no QML.
        const auto ComputeResults = Renderer->GetAsyncComputeTimings();

        struct FTimingGroup {
            const Smile::FGpuProfiler::FScopeResult* Parent = nullptr;
            std::vector<const Smile::FGpuProfiler::FScopeResult*> Children;
            bool Async = false;
        };
        std::vector<FTimingGroup> Groups;
        Groups.reserve(Results.size() + ComputeResults.size());

        FTimingGroup* Current = nullptr;
        for (const auto& R : Results) {
            if (std::strcmp(R.Name, kGpuFrameScope) == 0) continue;
            // A fila direta tem "Frame (GPU)" em Depth 0. Seus passes ficam em 1 e os
            // subpasses em 2+. Níveis mais profundos são achatados no pai visual imediato.
            if (R.Depth <= 1 || !Current) {
                Groups.push_back({ &R, {}, false });
                Current = &Groups.back();
            } else {
                Current->Children.push_back(&R);
            }
        }
        for (const auto& R : ComputeResults)
            Groups.push_back({ &R, {}, true });

        auto GroupName = [](const FTimingGroup& Group) {
            return QString::fromUtf8(Group.Parent->Name);
        };
        if (GpuTimingOrder.isEmpty()) {
            std::stable_sort(Groups.begin(), Groups.end(),
                             [](const FTimingGroup& A, const FTimingGroup& B) {
                                 return A.Parent->Milliseconds > B.Parent->Milliseconds;
                             });
        } else {
            // Comeca pelo ranking anterior. Passes novos entram no fim e sobem pela mesma regra
            // de histerese; assim toggles de render nao embaralham o restante da tabela.
            std::stable_sort(Groups.begin(), Groups.end(),
                             [&](const FTimingGroup& A, const FTimingGroup& B) {
                                 const int IA = GpuTimingOrder.indexOf(GroupName(A));
                                 const int IB = GpuTimingOrder.indexOf(GroupName(B));
                                 const int KA = IA >= 0 ? IA : GpuTimingOrder.size();
                                 const int KB = IB >= 0 ? IB : GpuTimingOrder.size();
                                 return KA < KB;
                             });
            // Insertion sort com margem: diferenca real (>5% ou >0,02 ms) muda o ranking;
            // ruido/empate conserva a posicao anterior.
            for (size_t i = 1; i < Groups.size(); ++i) {
                size_t j = i;
                while (j > 0) {
                    const double Ahead = Groups[j - 1].Parent->Milliseconds;
                    const double Behind = Groups[j].Parent->Milliseconds;
                    const double Margin = std::max(0.02, Ahead * 0.05);
                    if (Behind <= Ahead + Margin) break;
                    std::swap(Groups[j - 1], Groups[j]);
                    --j;
                }
            }
        }
        GpuTimingOrder.clear();
        for (const FTimingGroup& Group : Groups)
            GpuTimingOrder.push_back(GroupName(Group));

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        for (const FTimingGroup& Group : Groups) {
            const auto* R = Group.Parent;
            QVariantList Children;
            double ChildTotalMs = 0.0;
            auto MakeChildRow = [&](const Smile::FGpuProfiler::FScopeResult* Child) {
                ChildTotalMs += Child->Milliseconds;
                QVariantMap ChildRow;
                ChildRow.insert(QStringLiteral("name"), QString::fromUtf8(Child->Name));
                ChildRow.insert(QStringLiteral("text"),
                                Loc.toString(Child->Milliseconds, 'f', 2) + QStringLiteral(" ms"));
                ChildRow.insert(QStringLiteral("frac"), R->Milliseconds > 0.0
                    ? std::min(1.0, Child->Milliseconds / R->Milliseconds) : 0.0);
                ChildRow.insert(QStringLiteral("shareText"), R->Milliseconds > 0.0
                    ? Loc.toString(100.0 * Child->Milliseconds / R->Milliseconds, 'f', 0) +
                          QStringLiteral("%")
                    : QStringLiteral("—"));
                ChildRow.insert(QStringLiteral("active"), true);
                return ChildRow;
            };

            if (std::strcmp(R->Name, "Sombras — sol (CSM)") == 0) {
                // O cache atualiza apenas parte das cascatas em cada frame. As ausentes seguem
                // sendo usadas com o depth anterior: mantemos as quatro linhas para a arvore nao
                // crescer/encolher e marcamos visualmente as que vieram do cache.
                static constexpr const char* CascadeNames[4] = {
                    "Cascata 0", "Cascata 1", "Cascata 2", "Cascata 3" };
                for (const char* CascadeName : CascadeNames) {
                    const Smile::FGpuProfiler::FScopeResult* Found = nullptr;
                    for (const auto* Child : Group.Children) {
                        if (std::strcmp(Child->Name, CascadeName) == 0) {
                            Found = Child;
                            break;
                        }
                    }
                    if (Found) {
                        Children.push_back(MakeChildRow(Found));
                    } else {
                        QVariantMap CachedRow;
                        CachedRow.insert(QStringLiteral("name"), QString::fromUtf8(CascadeName));
                        CachedRow.insert(QStringLiteral("text"), QStringLiteral("—"));
                        CachedRow.insert(QStringLiteral("frac"), 0.0);
                        CachedRow.insert(QStringLiteral("shareText"), QStringLiteral("CACHE"));
                        CachedRow.insert(QStringLiteral("active"), false);
                        Children.push_back(CachedRow);
                    }
                }
            } else {
                for (const auto* Child : Group.Children)
                    Children.push_back(MakeChildRow(Child));
            }

            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QString::fromUtf8(R->Name));
            Row.insert(QStringLiteral("text"), Loc.toString(R->Milliseconds, 'f', 2) +
                                               QStringLiteral(" ms"));
            Row.insert(QStringLiteral("frac"),
                       FrameMs > 0.0 ? std::min(1.0, R->Milliseconds / FrameMs) : 0.0);
            Row.insert(QStringLiteral("percentText"), FrameMs > 0.0
                ? Loc.toString(100.0 * R->Milliseconds / FrameMs, 'f', 1) + QStringLiteral("%")
                : QStringLiteral("—"));
            Row.insert(QStringLiteral("selfText"),
                       Loc.toString(std::max(0.0, R->Milliseconds - ChildTotalMs), 'f', 2) +
                           QStringLiteral(" ms"));
            Row.insert(QStringLiteral("async"), Group.Async);
            Row.insert(QStringLiteral("hasChildren"), !Children.isEmpty());
            Row.insert(QStringLiteral("children"), Children);
            Rows.push_back(Row);
        }
        return Rows;
    }

    void ViewportWidget::SelectLit() {
        if (!Renderer) return;
        Renderer->SetGBufferDebugMode(0);
        Renderer->SetFlickerMode(0);
        // Um alvo de debug ativo tem prioridade sobre o caminho normal no Renderer, entao
        // escolher Lit precisa desliga-lo — senao a tela nao muda e o menu fica com duas
        // linhas marcadas.
        Renderer->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        CurrentViewMode = Lit;
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SelectReflectionHeatmap() {
        if (!Renderer) return;
        Renderer->SetGBufferDebugMode(0);
        Renderer->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        // Ainda nao ha um heatmap exclusivo dos raios de reflexao. O heatmap temporal
        // existente e a visualizacao funcional mais proxima para este slot do mockup.
        Renderer->SetFlickerMode(2);
        CurrentViewMode = ReflectionHeatmap;
        emit ViewSettingsChanged();
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

    QVariantList ViewportWidget::GetDebugSelection() const {
        QVariantList L;
        if (!Renderer) return L;
        for (Smile::u32 I : Renderer->GetDebugSelection()) L << static_cast<int>(I);
        return L;
    }

    int ViewportWidget::GetDebugColumns() const {
        return Renderer ? static_cast<int>(Renderer->GetDebugColumns()) : 0;
    }

    double ViewportWidget::GetDebugExposure() const {
        return Renderer ? static_cast<double>(Renderer->GetDebugExposure()) : 1.0;
    }

    bool ViewportWidget::GetDebugProbeCoordValues(
            int& _X, int& _Y, int& _Z, int& _CountX, int& _CountY, int& _CountZ) const {
        if (!Renderer || !DebugProbeSessionActive) return false;
        const auto& DDGI = Renderer->GetDDGI();
        const Smile::u32 Index = Renderer->GetDebugProbeIndex();
        if (!DDGI.IsReady() || Index == Smile::Renderer::kNoDebugProbe ||
            Index >= DDGI.NumProbesCount()) {
            return false;
        }

        const Smile::Vec3 Count = DDGI.GridCount();
        _CountX = static_cast<int>(Count.X);
        _CountY = static_cast<int>(Count.Y);
        _CountZ = static_cast<int>(Count.Z);
        if (_CountX <= 0 || _CountY <= 0 || _CountZ <= 0) return false;

        const int XY = _CountX * _CountY;
        _Z = static_cast<int>(Index) / XY;
        const int R = static_cast<int>(Index) - _Z * XY;
        _Y = R / _CountX;
        _X = R - _Y * _CountX;
        return true;
    }

    int ViewportWidget::GetDebugProbeIndex() const {
        if (!Renderer || !DebugProbeSessionActive) return -1;
        const Smile::u32 Index = Renderer->GetDebugProbeIndex();
        return Index == Smile::Renderer::kNoDebugProbe ? -1 : static_cast<int>(Index);
    }

    QString ViewportWidget::GetDebugProbeCoord() const {
        int X, Y, Z, CX, CY, CZ;
        if (!GetDebugProbeCoordValues(X, Y, Z, CX, CY, CZ)) return QString();
        return QStringLiteral("grid (%1, %2, %3)").arg(X).arg(Y).arg(Z);
    }

    QString ViewportWidget::GetDebugProbeWorld() const {
        int X, Y, Z, CX, CY, CZ;
        if (!GetDebugProbeCoordValues(X, Y, Z, CX, CY, CZ)) return QString();
        const auto& DDGI = Renderer->GetDDGI();
        const Smile::Vec3 Min = DDGI.GridMin();
        const double S = DDGI.Spacing();
        const double PX = Min.X + static_cast<double>(X) * S;
        const double PY = Min.Y + static_cast<double>(Y) * S;
        const double PZ = Min.Z + static_cast<double>(Z) * S;
        const QLocale Locale;
        return QStringLiteral("posição-base %1 · %2 · %3 m")
            .arg(Locale.toString(PX, 'f', 2))
            .arg(Locale.toString(PY, 'f', 2))
            .arg(Locale.toString(PZ, 'f', 2));
    }

    QString ViewportWidget::GetDebugProbeGrid() const {
        int X, Y, Z, CX, CY, CZ;
        if (!GetDebugProbeCoordValues(X, Y, Z, CX, CY, CZ)) return QString();
        const QLocale Locale;
        return QStringLiteral("%1 × %2 × %3 probes · spacing %4 m")
            .arg(CX).arg(CY).arg(CZ)
            .arg(Locale.toString(Renderer->GetDDGI().Spacing(), 'f', 2));
    }

    QString ViewportWidget::GetDebugProbeDistanceRange() const {
        if (!Renderer || !DebugProbeSessionActive) return QString();
        const QLocale Locale;
        return QStringLiteral("distância média · 0 → %1 m")
            .arg(Locale.toString(Renderer->GetDDGI().DistanceMomentMax(), 'f', 2));
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
        if (!_Enabled && DebugProbeSessionActive) ClearDebugProbeInspection();
        Renderer->SetDebugPreviewEnabled(_Enabled);
        if (_Enabled) InvalidateDebugPreview();
    }

    // Liga/desliga um alvo na grade offscreen da janela. O alvo unico do toolbar continua
    // independente e, se ativo, permanece fullscreen no viewport principal.
    void ViewportWidget::ToggleDebugSelection(int _Index) {
        if (!Renderer || _Index < 0) return;
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        std::vector<Smile::u32> Sel = Renderer->GetDebugSelection();
        const auto It = std::find(Sel.begin(), Sel.end(), static_cast<Smile::u32>(_Index));
        if (It != Sel.end()) Sel.erase(It);
        else if (Sel.size() < 16u) Sel.push_back(static_cast<Smile::u32>(_Index));
        else return;
        Renderer->SetDebugSelection(Sel);
        InvalidateDebugPreview();
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ClearDebugSelection() {
        if (!Renderer) return;
        if (DebugProbeSessionActive) ClearDebugProbeInspection();
        Renderer->SetDebugSelection({});
        InvalidateDebugPreview();
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetDebugColumns(int _Columns) {
        if (!Renderer) return;
        if (DebugProbeSessionActive) return;
        Renderer->SetDebugColumns(_Columns < 0 ? 0 : static_cast<Smile::u32>(_Columns));
        InvalidateDebugPreview();
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetDebugExposure(double _Exposure) {
        if (!Renderer) return;
        Renderer->SetDebugExposure(static_cast<float>(_Exposure));
        InvalidateDebugPreview();
        emit ViewSettingsChanged();
    }

    void ViewportWidget::InspectDDGIProbe(
            int _TargetIndex, double _U, double _V, double _TileAspect) {
        if (!Renderer || DebugProbeSessionActive || _TargetIndex < 0) return;
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

        const int TilesX = CountX * CountZ;
        const int Total  = TilesX * CountY;
        const double Aspect = std::max(_TileAspect, 1e-4);
        const int Cols = std::max(1, static_cast<int>(
            std::ceil(std::sqrt(static_cast<double>(Total) * Aspect))));
        const int Rows = (Total + Cols - 1) / Cols;
        const double U = std::clamp(_U, 0.0, 0.999999);
        const double V = std::clamp(_V, 0.0, 0.999999);
        const int DisplayX = std::min(static_cast<int>(U * Cols), Cols - 1);
        const int DisplayY = std::min(static_cast<int>(V * Rows), Rows - 1);
        const int AtlasIndex = DisplayY * Cols + DisplayX;
        if (AtlasIndex < 0 || AtlasIndex >= Total) return; // celula vazia da ultima linha

        const int TileCol = AtlasIndex % TilesX;
        const int Y = AtlasIndex / TilesX;
        const int X = TileCol % CountX;
        const int Z = TileCol / CountX;
        const int ProbeIndex = X + Y * CountX + Z * CountX * CountY;

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
        emit ViewSettingsChanged();
    }

    void ViewportWidget::StepDebugProbe(int _DX, int _DY, int _DZ) {
        int X, Y, Z, CountX, CountY, CountZ;
        if (!GetDebugProbeCoordValues(X, Y, Z, CountX, CountY, CountZ)) return;
        const int NX = std::clamp(X + _DX, 0, CountX - 1);
        const int NY = std::clamp(Y + _DY, 0, CountY - 1);
        const int NZ = std::clamp(Z + _DZ, 0, CountZ - 1);
        const int Index = NX + NY * CountX + NZ * CountX * CountY;
        if (Index == GetDebugProbeIndex()) return;
        ResetDebugProbePoint();
        SelectDebugProbe(Index);
        if (!DebugProbeSample.isEmpty()) {
            DebugProbeSample.clear();
            emit DebugProbeSampleChanged();
        }
        InvalidateDebugPreview();
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ClearDebugProbeInspection() {
        if (!Renderer || !DebugProbeSessionActive) return;
        ResetDebugProbePoint();
        SelectDebugProbe(-1);

        std::vector<Smile::u32> Restored;
        Restored.reserve(static_cast<size_t>(DebugProbePreviousTargets.size()));
        for (const QString& Name : DebugProbePreviousTargets) {
            const Smile::u32 Index = Smile::DebugTargets::IndexOf(Name.toStdString());
            if (Index != Smile::DebugTargets::kInvalid) Restored.push_back(Index);
        }
        Renderer->SetDebugSelection(Restored);
        Renderer->SetDebugColumns(
            DebugProbePreviousColumns < 0 ? 0u
                                          : static_cast<Smile::u32>(DebugProbePreviousColumns));

        DebugProbeSessionActive = false;
        DebugProbePreviousTargets.clear();
        DebugProbeDirection.clear();
        DebugProbeSample.clear();
        InvalidateDebugPreview();
        emit DebugProbeDirectionChanged();
        emit DebugProbeSampleChanged();
        emit ViewSettingsChanged();
    }

    // Ver o header: so as escolhas EXPLICITAS do usuario passam por aqui. O foco automatico do
    // point-pick chama Renderer->SetDebugProbeIndex direto, para nao virar "selecao da sessao".
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
            Renderer->CancelDebugProbePoint();
            if (DebugProbeSessionActive) {
                Renderer->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
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

        Renderer->CancelDebugProbePoint();
        Renderer->SetDebugProbeContributors(nullptr, nullptr, 0, -1);
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
        bool Found = false;
        for (Smile::u32 I = 0; I < DebugProbeContributorCount; ++I) {
            if (DebugProbeContributorIndices[I] ==
                static_cast<Smile::u32>(_ProbeIndex)) {
                Found = true;
                break;
            }
        }
        if (!Found) return;

        // Clicar num contribuinte e escolha do usuario: vira a probe da sessao, e um pick
        // seguinte fora do volume volta para ELA, nao para a dominante automatica anterior.
        SelectDebugProbe(_ProbeIndex);
        Renderer->SetDebugProbeContributors(
            DebugProbeContributorIndices.data(),
            DebugProbeContributorWeights.data(),
            DebugProbeContributorCount,
            DebugProbeContributorRiskSlot);
        InvalidateDebugPreview();
        emit ViewSettingsChanged();
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
        // -1 desliga e devolve o viewport ao caminho normal.
        Renderer->SetDebugTargetIndex(_Index < 0 ? Smile::Renderer::kNoDebugTarget
                                                 : static_cast<Smile::u32>(_Index));
        if (_Index >= 0) {
            // Modo unico: o alvo substitui Lit/Heatmap, entao zera o heatmap temporal e
            // volta o view mode base p/ Lit (é o estado ao qual "Desligado" retorna).
            Renderer->SetFlickerMode(0);
            Renderer->SetGBufferDebugMode(0);
            CurrentViewMode = Lit;
        }
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleDDGI() {
        if (!Renderer) return;
        Renderer->SetUseGI(!Renderer->GetUseGI());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleReSTIRGI() {
        if (!Renderer) return;
        Renderer->SetUseReSTIRGI(!Renderer->GetUseReSTIRGI());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleReGIR() {
        if (!Renderer) return;
        Renderer->SetUseReGIR(!Renderer->GetUseReGIR());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleReSTIRGIVisibility() {
        if (!Renderer) return;
        auto& ReSTIRGI = Renderer->GetReSTIRGI();
        ReSTIRGI.SetVisibility(!ReSTIRGI.GetVisibility());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleGIFoliageShadows() {
        if (!Renderer) return;
        // Toggle unico p/ os 3 consumidores do HitShading (DDGI e a fonte da verdade na leitura).
        const bool V = !Renderer->GetDDGI().GetFoliageShadows();
        Renderer->GetDDGI().SetFoliageShadows(V);
        Renderer->GetReSTIRGI().SetFoliageShadows(V);
        Renderer->GetReflections().SetFoliageShadows(V);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleReflectionsCullBackface() {
        if (!Renderer) return;
        Renderer->SetReflectionsCullBackface(!Renderer->GetReflectionsCullBackface());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleReSTIRDI() {
        if (!Renderer) return;
        Renderer->SetUseReSTIRDI(!Renderer->GetUseReSTIRDI());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleGIBackfacePolicy() {
        if (!Renderer) return;
        // Pelo Renderer: alem dos reservoirs, o NRD/RR/TAA acumulam sobre o resultado e
        // precisam cair juntos, senao o A/B denoisado compara estado misturado.
        Renderer->SetGIBackfacePolicy(!Renderer->GetGIBackfacePolicy());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetGISurfaceBiasMax(double _Meters) {
        if (!Renderer) return;
        Renderer->SetGISurfaceBiasMax(static_cast<float>(qBound(0.0, _Meters, 2.0)));
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetGIVolumeFadeProbes(double _Probes) {
        if (!Renderer) return;
        Renderer->SetGIVolumeFadeProbes(static_cast<float>(qBound(0.0, _Probes, 3.0)));
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleGTAO() {
        if (!Renderer) return;
        Renderer->SetUseAO(!Renderer->GetUseAO());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleGTAOHalfRes() {
        if (!Renderer) return;
        auto& AO = Renderer->GetAO();
        AO.SetHalfRes(!AO.GetHalfRes());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleReflections() {
        if (!Renderer) return;
        Renderer->SetUseReflections(!Renderer->GetUseReflections());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ToggleNrd() {
        if (!Renderer) return;
        Renderer->SetUseNrdDenoise(!Renderer->GetUseNrdDenoise());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetDenoiserMode(int _Mode) {
        if (!Renderer) return;
        // 0=Nenhum 1=NRD 2=DLSS RR. Selecionar RR forca e trava o upscaler em DLSS (o RR faz o upscale);
        // o Renderer cai p/ NRD se o RR nao estiver disponivel (sem NVIDIA/SDK).
        Renderer->SetDenoiser(static_cast<Smile::EDenoiser>(_Mode));
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetUpscalerMode(int _Mode) {
        if (!Renderer) return;
        Renderer->SetUpscaler(static_cast<Smile::EUpscaler>(_Mode));  // 0=None 1=FSR 2=DLSS; cai p/ None se indisponivel
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetUpscalerQuality(int _Quality) {
        if (!Renderer) return;
        Renderer->SetUpscalerQuality(_Quality);  // qualidade compartilhada FSR/DLSS
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetRenderScale(double _Scale) {
        if (!Renderer) return;
        Renderer->SetRenderScale(static_cast<float>(_Scale));
        emit ViewSettingsChanged();
    }

    namespace {
        // Tabela unica dos knobs de epsilon: dirige leitura, escrita e a UI. O ponteiro-p/-membro
        // evita 9 getters e 9 setters quase identicos, e a UI e um Repeater sobre esta lista.
        //
        // UiScale = quantas unidades de UI por unidade de mundo (metro). Os offsets aparecem em
        // MILIMETROS porque o sweep interessante e sub-milimetrico (o Lumen usa 0,1-0,5 mm) e ler
        // "0,0002 m" num slider e inutil. MaxAge e contagem de frames, entao escala 1.
        struct FEpsKnob {
            const char* Key;
            const char* Label;
            const char* Unit;
            const char* Hint;
            float Smile::FRayEpsilonProfile::* Field;
            double UiScale;
            double UiMin;
            double UiMax;
            int    Decimals;
        };

        const FEpsKnob kEpsKnobs[] = {
            { "originFloorMin", "Piso do offset de origem", "mm",
              "Somado ao offset ULP em todo raio que sai do G-buffer. 200 mm e o modo legado e a "
              "causa medida das manchas nas cortinas. Varrer ate 0.",
              &Smile::FRayEpsilonProfile::OriginFloorMin, 1000.0, 0.0, 250.0, 2 },
            { "originFloorPerMeter", "Offset por metro de camera", "mm/m",
              "Termo proporcional a distancia da camera. Heuristico: a 50 m ele sozinho da 10 mm. "
              "Varrer ate 0 — a quantizacao do depth justifica ~0,003 mm.",
              &Smile::FRayEpsilonProfile::OriginFloorPerMeter, 1000.0, 0.0, 1.0, 3 },
            { "originAngularMax", "Bias angular (raio rasante)", "mm",
              "Estilo Lumen: raio rasante leva este valor, perpendicular leva 20% dele. 0 = "
              "desligado. O Lumen usa 0,5 mm — mas em coordenadas camera-relative.",
              &Smile::FRayEpsilonProfile::OriginAngularMax, 1000.0, 0.0, 5.0, 3 },
            { "hitShadowRayBias", "Bias do shadow ray (2o hit)", "mm",
              "Desloca a origem das sombras de sol e puntuais no ponto de hit. Independente do "
              "piso acima: baixar so o piso deixa este dominando o contato.",
              &Smile::FRayEpsilonProfile::HitShadowRayBias, 1000.0, 0.0, 250.0, 2 },
            { "shadowRayBiasMin", "Piso do bias do shadow ray", "mm",
              "ATENCAO: em 1 mm ele mascara qualquer sweep do bias acima abaixo disso — o A/B "
              "pareceria nao fazer diferenca pelo motivo errado.",
              &Smile::FRayEpsilonProfile::ShadowRayBiasMin, 1000.0, 0.0, 10.0, 3 },
            { "shadowRayTMin", "TMin do shadow ray", "mm",
              "Soma com o bias: em 10 mm, oclusor a menos de 1 cm do hit fica invisivel mesmo "
              "depois de o bias cair.",
              &Smile::FRayEpsilonProfile::ShadowRayTMin, 1000.0, 0.0, 50.0, 2 },
            { "visRayTMin", "TMin do visibility ray", "mm",
              "Reuso espacial do ReSTIR. Entra no comprimento minimo da conexao, que e derivado "
              "de TMin + margem + folga.",
              &Smile::FRayEpsilonProfile::VisRayTMin, 1000.0, 0.0, 100.0, 2 },
            { "visRayEndMargin", "Margem final do visibility ray", "mm",
              "Para o raio antes da superficie do x2. Tambem entra no comprimento minimo.",
              &Smile::FRayEpsilonProfile::VisRayEndMargin, 1000.0, 0.0, 100.0, 2 },
            { "maxAge", "Idade maxima do reservoir", "frames",
              "Expira a amostra selecionada (RTXDI usa 30). 0 = sem expiracao, que e o estado "
              "atual. O MCap limita o PESO do historico, nao a vida da amostra.",
              &Smile::FRayEpsilonProfile::MaxAge, 1.0, 0.0, 60.0, 0 },
        };
    }

    QVariantList ViewportWidget::GetRayEpsilons() const {
        QVariantList Out;
        if (!Renderer) return Out;
        const Smile::FRayEpsilonProfile& P = Renderer->GetRayEpsilons();
        for (const FEpsKnob& K : kEpsKnobs) {
            QVariantMap M;
            M["key"]      = QString::fromLatin1(K.Key);
            M["label"]    = QString::fromUtf8(K.Label);
            M["unit"]     = QString::fromUtf8(K.Unit);
            M["hint"]     = QString::fromUtf8(K.Hint);
            M["value"]    = static_cast<double>(P.*(K.Field)) * K.UiScale;
            M["min"]      = K.UiMin;
            M["max"]      = K.UiMax;
            M["decimals"] = K.Decimals;
            Out.append(M);
        }
        return Out;
    }

    void ViewportWidget::SetRayEpsilon(const QString& _Key, double _UiValue) {
        if (!Renderer) return;
        for (const FEpsKnob& K : kEpsKnobs) {
            if (_Key != QLatin1String(K.Key)) continue;
            Smile::FRayEpsilonProfile P = Renderer->GetRayEpsilons();
            P.*(K.Field) = static_cast<float>(
                qBound(K.UiMin, _UiValue, K.UiMax) / K.UiScale);
            Renderer->SetRayEpsilons(P); // invalida reservoirs + historico do denoiser
            emit ViewSettingsChanged();
            return;
        }
    }

    void ViewportWidget::ResetRayEpsilons() {
        if (!Renderer) return;
        Renderer->SetRayEpsilons(Smile::FRayEpsilonProfile{}); // volta aos defaults do header
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetTAAEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseTAA(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetFrustumCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetFrustumCulling(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetOcclusionCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetOcclusionCulling(_Enabled);
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreSunShadowsEnabled() const {
        return Renderer && Renderer->GetUseSunShadows();
    }

    bool ViewportWidget::IsShadowCacheEnabled() const {
        return Renderer && Renderer->GetSunShadows().GetCascadeCache();
    }

    bool ViewportWidget::IsShadowDebugCascades() const {
        return Renderer && Renderer->GetSunShadows().GetDebugCascades();
    }

    double ViewportWidget::GetShadowMaxDistance() const {
        return Renderer ? Renderer->GetSunShadows().GetMaxDistance() : 800.0;
    }

    double ViewportWidget::GetShadowDepthBias() const {
        return Renderer ? Renderer->GetSunShadows().GetDepthBias() : 0.0006;
    }

    double ViewportWidget::GetShadowMinCasterTexels() const {
        return Renderer ? Renderer->GetSunShadows().GetMinCasterTexels() : 2.0;
    }

    QVariantList ViewportWidget::GetShadowCascadeBias() const {
        QVariantList List;
        for (Smile::u32 c = 0; c < Smile::FSunShadows::kNumCascades; ++c)
            List.append(Renderer ? Renderer->GetSunShadows().GetCascadeBiasScale(c) : 1.0);
        return List;
    }

    void ViewportWidget::SetSunShadowsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseSunShadows(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetShadowCacheEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetCascadeCache(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetShadowDebugCascades(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetDebugCascades(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetShadowMaxDistance(double _Distance) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetMaxDistance(static_cast<Smile::f32>(_Distance));
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetShadowDepthBias(double _Bias) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetDepthBias(static_cast<Smile::f32>(_Bias));
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetShadowMinCasterTexels(double _Texels) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetMinCasterTexels(static_cast<Smile::f32>(_Texels));
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetShadowCascadeBiasScale(int _Cascade, double _Scale) {
        if (!Renderer || _Cascade < 0) return;
        Renderer->GetSunShadows().SetCascadeBiasScale(static_cast<Smile::u32>(_Cascade),
                                                      static_cast<Smile::f32>(_Scale));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetShadowSunAngle() const {
        return Renderer ? Renderer->GetSunShadows().GetSunAngularSize() : 0.53;
    }

    void ViewportWidget::SetShadowSunAngle(double _Degrees) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetSunAngularSize(static_cast<Smile::f32>(_Degrees));
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreSunShaftsEnabled() const {
        return Renderer ? Renderer->GetUseSunShafts() : true;
    }

    void ViewportWidget::SetSunShaftsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseSunShafts(_Enabled);
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsIntensity() const {
        return Renderer ? Renderer->GetSunShafts().GetVolIntensity() : 1.0;
    }

    void ViewportWidget::SetSunShaftsIntensity(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolIntensity(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsDust() const {
        return Renderer ? Renderer->GetSunShafts().GetVolDust() : 8.0;
    }

    void ViewportWidget::SetSunShaftsDust(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolDust(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsPhaseG() const {
        return Renderer ? Renderer->GetSunShafts().GetVolPhaseG() : 0.7;
    }

    void ViewportWidget::SetSunShaftsPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolPhaseG(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    int ViewportWidget::GetSunShaftsSteps() const {
        return Renderer ? static_cast<int>(Renderer->GetSunShafts().GetVolSteps()) : 32;
    }

    void ViewportWidget::SetSunShaftsSteps(int _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolSteps(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsRange() const {
        return Renderer ? Renderer->GetSunShafts().GetVolMaxDist() : 128.0;
    }

    void ViewportWidget::SetSunShaftsRange(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolMaxDist(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreSunShaftsTemporal() const {
        return Renderer ? Renderer->GetSunShafts().GetVolTemporal() : true;
    }

    void ViewportWidget::SetSunShaftsTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolTemporal(_Enabled);
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::IsVolFogEnabled() const {
        return Renderer ? Renderer->GetUseVolumetricFog() : true;
    }

    void ViewportWidget::SetVolFogEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseVolumetricFog(_Enabled);
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetVolFogDistance() const {
        return Renderer ? Renderer->GetVolumetricFog().GetMaxDistance() : 100.0;
    }

    void ViewportWidget::SetVolFogDistance(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetMaxDistance(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetVolFogPhaseG() const {
        return Renderer ? Renderer->GetVolumetricFog().GetPhaseG() : 0.3;
    }

    void ViewportWidget::SetVolFogPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetPhaseG(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetVolFogDensity() const {
        return Renderer ? Renderer->GetVolumetricFog().GetExtinctionScale() : 1.0;
    }

    void ViewportWidget::SetVolFogDensity(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetExtinctionScale(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetVolFogAmbient() const {
        return Renderer ? Renderer->GetVolumetricFog().GetAmbientIntensity() : 1.0;
    }

    void ViewportWidget::SetVolFogAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetAmbientIntensity(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::IsVolFogTemporal() const {
        return Renderer ? Renderer->GetVolumetricFog().GetTemporal() : true;
    }

    void ViewportWidget::SetVolFogTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetTemporal(_Enabled);
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetVolFogLights() const {
        return Renderer ? Renderer->GetVolumetricFog().GetLightsIntensity() : 1.0;
    }

    void ViewportWidget::SetVolFogLights(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetLightsIntensity(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::IsVolFogConsDepth() const {
        return Renderer ? Renderer->GetVolumetricFog().GetConservativeDepth() : true;
    }

    void ViewportWidget::SetVolFogConsDepth(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetConservativeDepth(_Enabled);
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreCloudsEnabled() const {
        return Renderer ? Renderer->GetUseClouds() : false;
    }

    void ViewportWidget::SetCloudsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseClouds(_Enabled);
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreCloudsHalfRes() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetHalfRes() : true;
    }

    bool ViewportWidget::AreCloudsTemporal() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetUseTemporal() : true;
    }

    void ViewportWidget::SetCloudsTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetUseTemporal(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetCloudsHalfRes(bool _HalfRes) {
        if (!Renderer) return;
        Renderer->SetCloudsHalfRes(_HalfRes);
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudCoverage() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetCoverage() : 0.45;
    }

    void ViewportWidget::SetCloudCoverage(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetCoverage(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudDensity() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetDensityScale() : 1.6;
    }

    void ViewportWidget::SetCloudDensity(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetDensityScale(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudWindSpeed() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetWindSpeed() : 0.01;
    }

    void ViewportWidget::SetCloudWindSpeed(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetWindSpeed(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudErosion() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetErosion() : 0.45;
    }

    void ViewportWidget::SetCloudErosion(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetErosion(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudPhaseG() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetPhaseG() : 0.8;
    }

    void ViewportWidget::SetCloudPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetPhaseG(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudPowder() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetPowder() : 0.5;
    }

    void ViewportWidget::SetCloudPowder(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetPowder(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudAmbient() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetAmbientScale() : 1.0;
    }

    void ViewportWidget::SetCloudAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetAmbientScale(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudTypeBias() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetCloudTypeBias() : 0.0;
    }

    void ViewportWidget::SetCloudTypeBias(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetCloudTypeBias(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudPeakVariation() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetPeakVariation() : 1.0;
    }

    void ViewportWidget::SetCloudPeakVariation(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetPeakVariation(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    int ViewportWidget::GetCloudWeatherSeed() const {
        return Renderer ? static_cast<int>(Renderer->GetCloudWeatherSeed()) : 1337;
    }

    void ViewportWidget::SetCloudWeatherSeed(int _Seed) {
        if (!Renderer) return;
        Renderer->SetCloudWeatherSeed(static_cast<Smile::u32>(_Seed < 0 ? 0 : _Seed));
        emit ViewSettingsChanged();
    }

    int ViewportWidget::GetCloudWeatherCells() const {
        return Renderer ? static_cast<int>(Renderer->GetCloudWeatherCells()) : 3;
    }

    void ViewportWidget::SetCloudWeatherCells(int _Mult) {
        if (!Renderer) return;
        Renderer->SetCloudWeatherCells(static_cast<Smile::u32>(_Mult < 1 ? 1 : _Mult));
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::IsCloudWeatherAuthored() const {
        return Renderer ? Renderer->CloudWeatherAuthored() : false;
    }

    void ViewportWidget::LoadCloudWeatherTexture() {
        if (!Renderer) return;
        const QString Path = QFileDialog::getOpenFileName(
            this, tr("Weather map (R=cobertura, G=tipo, B=altura de topo)"), QString(),
            tr("Imagens (*.png *.jpg *.jpeg *.tga *.bmp)"));
        if (Path.isEmpty()) return;
        Renderer->LoadCloudWeatherTexture(Path.toStdWString());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ClearCloudWeatherTexture() {
        if (!Renderer) return;
        Renderer->ClearCloudWeatherTexture();
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreCloudShadowsEnabled() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetShadowsEnabled() : true;
    }

    void ViewportWidget::SetCloudShadowsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetShadowsEnabled(_Enabled);
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudShadowStrength() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetShadowStrength() : 1.0;
    }

    void ViewportWidget::SetCloudShadowStrength(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetShadowStrength(static_cast<Smile::f32>(_Value));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCloudBottomKm() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetBottomAltitude() : 2.0;
    }

    double ViewportWidget::GetCloudThicknessKm() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetThickness() : 3.0;
    }

    void ViewportWidget::SetCloudAltitude(double _BottomKm, double _ThicknessKm) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetAltitude(static_cast<Smile::f32>(_BottomKm),
                                                    static_cast<Smile::f32>(_ThicknessKm));
        emit ViewSettingsChanged();
    }

    int ViewportWidget::GetCloudMarchSteps() const {
        return Renderer ? static_cast<int>(Renderer->GetVolumetricClouds().GetMarchSteps()) : 128;
    }

    void ViewportWidget::SetCloudMarchSteps(int _Steps) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetMarchSteps(static_cast<Smile::f32>(_Steps));
        emit ViewSettingsChanged();
    }

    // ---- Clima (FWeather; defaults espelham o struct p/ antes do renderer existir) ----
    double ViewportWidget::GetRainAmount() const {
        return Renderer ? Renderer->GetWeather().RainAmount : 0.0;
    }
    void ViewportWidget::SetRainAmount(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().RainAmount = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetPuddleAmount() const {
        return Renderer ? Renderer->GetWeather().PuddleAmount : 0.65;
    }
    void ViewportWidget::SetPuddleAmount(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().PuddleAmount = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetPuddleScale() const {
        return Renderer ? Renderer->GetWeather().PuddleScale : 8.0;
    }
    void ViewportWidget::SetPuddleScale(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().PuddleScale = static_cast<Smile::f32>(std::clamp(_Value, 1.0, 64.0));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetRippleStrength() const {
        return Renderer ? Renderer->GetWeather().RippleStrength : 1.0;
    }
    void ViewportWidget::SetRippleStrength(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().RippleStrength = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 2.0));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetWetDarkening() const {
        return Renderer ? Renderer->GetWeather().WetDarkening : 0.85;
    }
    void ViewportWidget::SetWetDarkening(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().WetDarkening = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit ViewSettingsChanged();
    }

    double ViewportWidget::GetCurtainAmount() const {
        return Renderer ? Renderer->GetWeather().CurtainAmount : 1.0;
    }
    void ViewportWidget::SetCurtainAmount(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().CurtainAmount = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::IsRainOcclusion() const {
        return Renderer ? Renderer->GetWeather().RainOcclusion : true;
    }
    void ViewportWidget::SetRainOcclusion(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetWeather().RainOcclusion = _Enabled;
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::AreRainParticles() const {
        return Renderer ? Renderer->GetWeather().RainParticles : true;
    }
    void ViewportWidget::SetRainParticles(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetWeather().RainParticles = _Enabled;
        emit ViewSettingsChanged();
    }

    bool ViewportWidget::IsWeatherDriveSky() const {
        return Renderer ? Renderer->GetWeather().DriveSky : true;
    }
    void ViewportWidget::SetWeatherDriveSky(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetWeather().DriveSky = _Enabled;
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetDepthPrepassEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetDepthPrepass(_Enabled);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::ResetRenderSettings() {
        if (!Renderer) return;
        // O padrao segue o backend recomendado (DLSS em NVIDIA, senao FSR, senao nativo), mas em
        // escala 1:1 (tier 0 = 100%): reconstroi/faz AA sem upscale. Bate com o default do Renderer.
        Renderer->SetUpscalerQuality(0);
        Renderer->SetUpscaler(static_cast<Smile::EUpscaler>(GetRecommendedUpscalerMode()));
        Renderer->SetUseTAA(true);
        Renderer->SetFrustumCulling(true);
        Renderer->SetDepthPrepass(false);
        emit ViewSettingsChanged();
    }

    void ViewportWidget::RequestSettings() {
        emit SettingsRequested();
    }

    void ViewportWidget::EnsureRendererIsInitialized() {
        if (Initialized) return;
        const HWND hWnd = reinterpret_cast<HWND>(winId());
        Renderer->Initialize(hWnd,
                             static_cast<unsigned int>(width()),
                             static_cast<unsigned int>(height()));
        // A selecao de upscaler/qualidade e uma preferencia anterior ao contexto. Agora que os passes
        // existem, reaplica a escala (e revalida a disponibilidade) recriando os alvos internos 1x.
        Renderer->SetUpscaler(Renderer->GetUpscaler());
        Initialized = true;
        emit RendererInitialized();
        emit DebugTargetsChanged();   // os alvos foram publicados na criacao dos targets internos
    }

    void ViewportWidget::showEvent(QShowEvent* _Event) {
        QWidget::showEvent(_Event);
        EnsureRendererIsInitialized();
        FrameTimer.restart();
        RedrawTimer->start();
    }

    void ViewportWidget::hideEvent(QHideEvent* _Event) {
        RedrawTimer->stop();
        QWidget::hideEvent(_Event);
    }

    void ViewportWidget::resizeEvent(QResizeEvent* _Event) {
        QWidget::resizeEvent(_Event);
        if (Initialized) {
            Renderer->Resize(static_cast<unsigned int>(_Event->size().width()),
                             static_cast<unsigned int>(_Event->size().height()));
            // Resize realoca SRVs: os alvos foram re-registrados com slots novos.
            emit DebugTargetsChanged();
        }
    }

    void ViewportWidget::paintEvent(QPaintEvent* _Event) {
        Q_UNUSED(_Event);
    }

    void ViewportWidget::OnRenderTimer() {
        EnsureRendererIsInitialized();
        if (!Renderer->IsInitialized()) return;

        // Resolucao de NANOSEGUNDOS: a >200 FPS o dt em ms inteiros (4/5/6ms) fazia o
        // FPS pular feito louco. nsecsElapsed da precisao sub-ms p/ dt e FPS estaveis.
        float DeltaTime = static_cast<float>(static_cast<double>(FrameTimer.nsecsElapsed()) / 1.0e9);
        FrameTimer.restart();
        DeltaTime = Smile::Clamp(DeltaTime, 0.0001f, 0.1f);

        Smile::CameraInput CameraInput;
        CameraInput.Look  = MouseLookActive
            ? Smile::Vec2{ MouseDelta.X * kMouseSensitivity,
                          -MouseDelta.Y * kMouseSensitivity }   
            : Smile::Vec2::Zero();
        CameraInput.Move  = Smile::Vec3{
            static_cast<float>(IsHeld(Qt::Key_D) - IsHeld(Qt::Key_A)),   
            static_cast<float>(IsHeld(Qt::Key_E) - IsHeld(Qt::Key_Q)),   
            static_cast<float>(IsHeld(Qt::Key_W) - IsHeld(Qt::Key_S)),   
        };
        CameraInput.Speed = IsHeld(Qt::Key_Shift) ? 4.0f : 1.0f;

        Renderer->UpdateCamera(CameraInput, DeltaTime);
        // Gizmo (editor-side): submete as setas ao DebugDraw da Engine ANTES do RenderFrame, que
        // as desenha e limpa. Geometria world-space -> projetada com a VP do frame (sem lag).
        GizmoCtrl.Submit(*Renderer);
        Renderer->RenderFrame();

        // A captura e produzida dentro do frame, em target offscreen. O readback fica pronto
        // quando o slot de frame volta a ser usado; daqui em diante o QML so enxerga QImage.
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
                // Fora do volume o pixel usa SO o ambiente de fallback: as oito sondas nao
                // contribuem com nada. Anunciar "dominante"/"maior risco" e destacar uma delas
                // no viewport seria apontar para quem nao iluminou o ponto. O Clear e explicito
                // (e nao contagem zero no setter) porque aquele caminho RESTAURA o destaque da
                // probe do pick anterior — ver Renderer::ClearDebugProbeContributors.
                DebugProbePointSummary = QStringLiteral(
                    "ponto %1 · %2 · %3 m  •  fora do volume de sondas — só ambiente")
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.X, 'f', 2))
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.Y, 'f', 2))
                    .arg(QLocale().toString(PointDiagnostic.WorldPosition.Z, 'f', 2));
                // Devolve a selecao a probe que o usuario tinha antes do pick: o cabecalho e o
                // preview do tile saem do indice selecionado, e mante-lo na dominante do pick
                // anterior mostraria "PROBE #N" com o oct-map dela ao lado de um resumo que diz
                // que nenhuma probe contribuiu. Restaurar tambem mantem a sessao viva — indice
                // valido e pre-requisito do proximo RequestDebugProbePoint.
                if (DebugProbeBaseIndex >= 0)
                    Renderer->SetDebugProbeIndex(DebugProbeBaseIndex);
                Renderer->ClearDebugProbeContributors();
                InvalidateDebugPreview();
                emit ViewSettingsChanged();
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
                    const int Index = static_cast<int>(P.ProbeIndex);
                    const int Z = Index / XY;
                    const int R = Index - Z * XY;
                    const int Y = R / CountX;
                    const int X = R - Y * CountX;

                    QVariantMap Item;
                    Item.insert(QStringLiteral("probeIndex"), Index);
                    Item.insert(QStringLiteral("coord"),
                                QStringLiteral("(%1,%2,%3)").arg(X).arg(Y).arg(Z));
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
                // Peso do volume: so aparece quando NAO e 1, senao polui a linha no caso comum.
                // Sem ele, mexer no fade de borda reexecutaria o diagnostico sem mover nada no
                // painel — os pesos por sonda nao dependem do fade, so o resultado do pixel.
                // O peso e uma FRACAO do gather (o resto vem do ambiente), nao uma contagem de
                // sondas — a largura do fade e que e medida em celulas, no slider. (O caso
                // peso = 0 nem chega aqui: tem ramo proprio, sem dominante nem contribuintes.)
                const QString VolumeNote = PointDiagnostic.VolumeWeight >= 0.999f
                    ? QString()
                    : QStringLiteral("  •  borda do volume (%1% DDGI)")
                          .arg(Locale.toString(PointDiagnostic.VolumeWeight * 100.0f, 'f', 0));
                DebugProbePointSummary = QStringLiteral(
                    "ponto %1 · %2 · %3 m  •  dominante #%4%5%6")
                    .arg(Locale.toString(PointDiagnostic.WorldPosition.X, 'f', 2))
                    .arg(Locale.toString(PointDiagnostic.WorldPosition.Y, 'f', 2))
                    .arg(Locale.toString(PointDiagnostic.WorldPosition.Z, 'f', 2))
                    .arg(DominantIndex)
                    .arg(RiskIndex >= 0
                        ? QStringLiteral("  •  maior risco #%1").arg(RiskIndex)
                        : QStringLiteral("  •  sem risco relevante"))
                    .arg(VolumeNote);
                InvalidateDebugPreview();
                emit ViewSettingsChanged();
            }
            emit DebugProbePointChanged();
        }

        // Picking: coleta o resultado de um clique recente (readback assincrono pronto alguns
        // frames depois). Atualiza a selecao e loga o objeto (validacao da Fase 1).
        int PickedIndex = -1;
        if (Renderer->TryGetPickResult(PickedIndex)) {
            // Um pick por GPU resolvido significa que o clique NAO foi num marker de luz
            // (senao nem teria virado RequestPick) -> a selecao de luz sai em ambos os casos.
            Renderer->ClearLightSelection();
            if (PickedIndex >= 0) {
                Renderer->SetSelectedObject(PickedIndex);
                const auto& Renderables = Renderer->GetScene().Renderables();
                if (PickedIndex < static_cast<int>(Renderables.size())) {
                    Smile::LogInfo("Selecionado [" + std::to_string(PickedIndex) + "] " +
                                   Renderables[static_cast<size_t>(PickedIndex)].Name);
                }
            } else {
                Renderer->ClearSelection();
                Smile::LogInfo("Selecao limpa (clique no vazio)");
            }
            emit ObjectSelected(PickedIndex);
        }

        // FPS suavizado por media exponencial (EMA) — leitura estavel em vez do valor
        // instantaneo 1/dt (que oscila muito frame a frame).
        const float InstFPS = DeltaTime > 0.0f ? 1.0f / DeltaTime : 0.0f;
        LastFPS = (LastFPS > 0.0f) ? (LastFPS * 0.96f + InstFPS * 0.04f) : InstFPS;
        MouseDelta = Smile::Vec2::Zero();
        emit FrameReady();
    }

    void ViewportWidget::keyPressEvent(QKeyEvent* _Event) {
        if (!_Event->isAutoRepeat())
            HeldKeys.insert(_Event->key());
        QWidget::keyPressEvent(_Event);
    }

    void ViewportWidget::keyReleaseEvent(QKeyEvent* _Event) {
        if (!_Event->isAutoRepeat())
            HeldKeys.remove(_Event->key());
        QWidget::keyReleaseEvent(_Event);
    }

    int ViewportWidget::PickLightMarker(unsigned int _X, unsigned int _Y) const {
        if (!Renderer || !Renderer->IsInitialized()) return -1;
        constexpr float kPickRadiusPx = 22.0f; // ~raio do icone billboard (~44px de altura)
        const float fx = static_cast<float>(_X), fy = static_cast<float>(_Y);

        const auto& Lights = Renderer->GetScene().Lights();
        float Best = kPickRadiusPx;
        int   BestIdx = -1;
        for (int i = 0; i < static_cast<int>(Lights.size()); ++i) {
            float sx, sy;
            if (!Renderer->WorldToScreen(Lights[static_cast<size_t>(i)].Position, sx, sy))
                continue;
            const float d = std::sqrt((fx - sx) * (fx - sx) + (fy - sy) * (fy - sy));
            if (d < Best) { Best = d; BestIdx = i; }
        }
        return BestIdx;
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
        // Clique esquerdo (fora do modo camera) = picking. O backbuffer/IDTarget tem o tamanho
        // logico do widget (Resize usa size() logico), entao a posicao do evento mapeia 1:1.
        // O passe de ID roda no proximo frame; o resultado e coletado em OnRenderTimer.
        else if (_Event->button() == Qt::LeftButton && !MouseLookActive) {
            if (Renderer && Renderer->IsInitialized()) {
                const QPointF P = _Event->position();
                const unsigned int Px = static_cast<unsigned int>(P.x() > 0.0 ? P.x() : 0.0);
                const unsigned int Py = static_cast<unsigned int>(P.y() > 0.0 ? P.y() : 0.0);
                if (DebugProbePointPickArmed) {
                    DebugProbePointPickArmed = false;
                    DebugProbeContributors.clear();
                    DebugProbeContributorCount = 0;
                    DebugProbeContributorRiskSlot = -1;
                    // A probe da sessao NAO e capturada aqui: ela ja foi registrada quando o
                    // usuario a escolheu (SelectDebugProbe). Capturar no clique leria o foco
                    // deixado pelo pick ANTERIOR e o promoveria a "selecao do usuario".
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
                // 1) Tenta pegar um handle do gizmo. Se pegou, comeca o arraste e NAO faz picking.
                // 2) Senao, tenta um marker de LUZ (teste 2D em tela — luz nao esta no ID-buffer).
                // 3) Senao, picking normal por GPU (seleciona o objeto sob o cursor).
                if (!GizmoCtrl.OnMousePress(*Renderer, Px, Py)) {
                    const int LightHit = PickLightMarker(Px, Py);
                    if (LightHit >= 0) {
                        Renderer->SetSelectedLight(LightHit);
                        Renderer->ClearSelection(); // selecoes exclusivas
                        const auto& Lights = Renderer->GetScene().Lights();
                        Smile::LogInfo("Luz selecionada [" + std::to_string(LightHit) + "] " +
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
            GizmoCtrl.OnMouseRelease(); // fim do arraste do gizmo (no-op se nao estava arrastando)
        }
        QWidget::mouseReleaseEvent(_Event);
    }

    void ViewportWidget::mouseMoveEvent(QMouseEvent* _Event) {
        if (!MouseLookActive) {
            // Sem camera-look: roteia pro gizmo. Arrastando -> move o objeto; senao -> hover (destaca
            // o eixo sob o cursor). Coords logicas = pixels do backbuffer (1:1).
            if (Renderer && Renderer->IsInitialized()) {
                const QPointF P = _Event->position();
                const unsigned int Px = static_cast<unsigned int>(P.x() > 0.0 ? P.x() : 0.0);
                const unsigned int Py = static_cast<unsigned int>(P.y() > 0.0 ? P.y() : 0.0);
                GizmoCtrl.OnMouseMove(*Renderer, Px, Py); // arraste se ativo, senao hover
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
