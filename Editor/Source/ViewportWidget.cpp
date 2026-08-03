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
#include <QMetaObject>
#include <QPointer>
#include <QPalette>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstring>
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
        // Antes do primeiro Present, deixa o Qt limpar a janela nativa em preto. Sem isso o
        // HWND aparece branco durante a inicializacao assincrona do Renderer.
        setAttribute(Qt::WA_NoSystemBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        QPalette InitialPalette = palette();
        InitialPalette.setColor(QPalette::Window, Qt::black);
        setPalette(InitialPalette);
        setAutoFillBackground(true);

        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 200);
        setMouseTracking(true); // hover do gizmo precisa de mouse-move sem botao pressionado

        // Intervalo 0: solicita o proximo frame quando a fila de eventos esvazia. O timer
        // fica pausado enquanto a thread de renderizacao trabalha e volta no callback;
        // o pacing continua a cargo do Present (VSync ou FPS livre).
        RedrawTimer = new QTimer(this);
        RedrawTimer->setInterval(0);
        RedrawTimer->setSingleShot(true);
        connect(RedrawTimer, &QTimer::timeout, this, &ViewportWidget::OnRenderTimer);

        // A janela principal ainda pode maximizar e relayoutar o viewport logo depois do
        // primeiro showEvent. Inicializa a swapchain apenas quando esse tamanho estabilizar.
        InitializationDebounce = new QTimer(this);
        InitializationDebounce->setSingleShot(true);
        InitializationDebounce->setInterval(kResizeDebounceMs);
        connect(InitializationDebounce, &QTimer::timeout,
                this, &ViewportWidget::EnsureRendererIsInitialized);

        // Fallback para Snap, maximizar/restaurar, DPI e resizes programaticos: esses caminhos
        // podem nao entrar no loop WM_ENTERSIZEMOVE. Eventos consecutivos conservam apenas o
        // ultimo tamanho e disparam uma unica realocacao depois que a geometria estabiliza.
        ResizeDebounce = new QTimer(this);
        ResizeDebounce->setSingleShot(true);
        ResizeDebounce->setInterval(kResizeDebounceMs);
        connect(ResizeDebounce, &QTimer::timeout, this, &ViewportWidget::ApplyPendingResize);

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
        if (InitializationDebounce) InitializationDebounce->stop();
        if (ResizeDebounce) ResizeDebounce->stop();
        // O closeEvent normal espera RendererStopped antes de destruir a arvore Qt. O fallback
        // preserva a vida dos campos em construcao parcial/teardown excepcional.
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
        return Telemetry.VisibleDrawCount;
    }

    int ViewportWidget::GetTotalDrawCount() const {
        return Telemetry.TotalDrawCount;
    }

    int ViewportWidget::GetOccludedDrawCount() const {
        return Telemetry.OccludedDrawCount;
    }

    QString ViewportWidget::GetInternalResolution() const {
        return Telemetry.InternalResolution;
    }

    QString ViewportWidget::GetOutputResolution() const {
        return Telemetry.OutputResolution;
    }

    QString ViewportWidget::GetGPUName() const {
        return Telemetry.GPUName;
    }

    QString ViewportWidget::GetVRAMText() const {
        return Telemetry.VRAMText;
    }

    QString ViewportWidget::GetVRAMUsageText() const {
        return Telemetry.VRAMUsageText;
    }

    bool ViewportWidget::IsVRAMOverBudget() const {
        return Telemetry.VRAMOverBudget;
    }

    double ViewportWidget::GetVRAMBudgetFrac() const {
        return Telemetry.VRAMBudgetFrac;
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
        return Telemetry.VRAMNonLocalText;
    }

    QVariantList ViewportWidget::GetVRAMBreakdown() const {
        return Telemetry.VRAMBreakdown;
    }

    // Tabela da janela de Estatisticas: categorias rastreadas (> 0) em ordem decrescente
    // + linha "Nao rastreado" (uso DXGI − soma rastreada: driver, descriptor heaps,
    // swapchain, upload heaps e o que nao foi instrumentado). frac e relativo ao uso total.
    QVariantList ViewportWidget::BuildVRAMBreakdown(Smile::Renderer& _Renderer) const {
        QVariantList Rows;
        const auto& VM   = _Renderer.GetDevice().QueryVideoMemory();
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
        return Telemetry.GPUFrameText;
    }

    double ViewportWidget::GetGpuFrameMs() const {
        return Telemetry.GPUFrameMs;
    }

    QVariantList ViewportWidget::GetGpuTimings() const {
        return Telemetry.GpuTimings;
    }

    // Tabela "GPU por passe": escopos do FGpuProfiler (sem o total, que vira o header),
    // em ordem de custo com histerese; frac relativo ao frame total de GPU. A margem conserva
    // a leitura por consumo sem deixar passes quase empatados trocarem de lugar a cada snapshot.
    QVariantList ViewportWidget::BuildGpuTimings(Smile::Renderer& _Renderer) {
        QVariantList Rows;
        const auto& Results = _Renderer.GetGpuProfiler().Results();
        if (Results.empty()) return Rows;

        double FrameMs = 0.0;
        for (const auto& R : Results)
            if (std::strcmp(R.Name, kGpuFrameScope) == 0) FrameMs = R.Milliseconds;

        // Passes da fila de COMPUTE (DDGI async) entram na mesma tabela. O FGpuProfiler agora
        // preserva Depth, portanto os scopes aninhados viram subpasses expansíveis no QML.
        const auto ComputeResults = _Renderer.GetAsyncComputeTimings();

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

    void ViewportWidget::CaptureTelemetry(Smile::Renderer& _Renderer) {
        FTelemetrySnapshot Next;
        Next.VisibleDrawCount = static_cast<int>(_Renderer.GetVisibleCount());
        Next.TotalDrawCount = static_cast<int>(_Renderer.GetDrawCount());
        Next.OccludedDrawCount = static_cast<int>(_Renderer.GetOccludedCount());
        Next.InternalResolution = QStringLiteral("%1×%2")
            .arg(_Renderer.RenderWidth()).arg(_Renderer.RenderHeight());
        Next.OutputResolution = QStringLiteral("%1×%2")
            .arg(_Renderer.OutputWidth()).arg(_Renderer.OutputHeight());
        Next.GPUName = QString::fromStdWString(
            _Renderer.GetDevice().GetAdapterDescription());

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
        Next.VRAMText = Loc.toString(
            static_cast<double>(_Renderer.GetDevice().GetAdapterDedicatedVideoMemory()) / GiB,
            'f', 1) + QStringLiteral(" GB");

        const auto& VM = _Renderer.GetDevice().QueryVideoMemory();
        if (VM.Valid) {
            Next.VRAMUsageText =
                Loc.toString(static_cast<double>(VM.LocalUsage) / GiB, 'f', 2) +
                QStringLiteral(" / ") +
                Loc.toString(static_cast<double>(VM.LocalBudget) / GiB, 'f', 1) +
                QStringLiteral(" GB");
            Next.VRAMOverBudget = VM.OverBudget;
            Next.VRAMBudgetFrac = VM.LocalBudget > 0
                ? std::min(1.0, static_cast<double>(VM.LocalUsage) /
                                  static_cast<double>(VM.LocalBudget))
                : 0.0;
            Next.VRAMNonLocalText = FormatBytes(VM.NonLocalUsage) + QStringLiteral(" / ") +
                                    FormatBytes(VM.NonLocalBudget);
        }
        Next.VRAMBreakdown = BuildVRAMBreakdown(_Renderer);

        for (const auto& Result : _Renderer.GetGpuProfiler().Results()) {
            if (std::strcmp(Result.Name, kGpuFrameScope) == 0) {
                Next.GPUFrameMs = Result.Milliseconds;
                break;
            }
        }
        if (Next.GPUFrameMs > 0.0) {
            Next.GPUFrameText = Loc.toString(Next.GPUFrameMs, 'f', 2) +
                                QStringLiteral(" ms");
        }
        Next.GpuTimings = BuildGpuTimings(_Renderer);
        Telemetry = std::move(Next);
    }

    void ViewportWidget::SelectLit() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetGBufferDebugMode(0);
        RendererAccess->SetFlickerMode(0);
        // Um alvo de debug ativo tem prioridade sobre o caminho normal no Renderer, entao
        // escolher Lit precisa desliga-lo — senao a tela nao muda e o menu fica com duas
        // linhas marcadas.
        RendererAccess->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        CurrentViewMode = Lit;
        emit ViewStateChanged();
    }

    void ViewportWidget::SelectReflectionHeatmap() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetGBufferDebugMode(0);
        RendererAccess->SetDebugTargetIndex(Smile::Renderer::kNoDebugTarget);
        // Ainda nao ha um heatmap exclusivo dos raios de reflexao. O heatmap temporal
        // existente e a visualizacao funcional mais proxima para este slot do mockup.
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

    bool ViewportWidget::GetDebugProbeCoordValues(
            int& _X, int& _Y, int& _Z, int& _CountX, int& _CountY, int& _CountZ) const {
        if (!Renderer || !DebugProbeSessionActive) return false;
        auto RendererAccess = Renderer.Lock();
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
        auto RendererAccess = Renderer.Lock();
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
        auto RendererAccess = Renderer.Lock();
        if (!_Enabled && DebugProbeSessionActive) ClearDebugProbeInspection();
        RendererAccess->SetDebugPreviewEnabled(_Enabled);
        if (_Enabled) InvalidateDebugPreview();
    }

    // Liga/desliga um alvo na grade offscreen da janela. O alvo unico do toolbar continua
    // independente e, se ativo, permanece fullscreen no viewport principal.
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
        emit DebugSettingsChanged();
    }

    void ViewportWidget::StepDebugProbe(int _DX, int _DY, int _DZ) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
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

        // Clicar num contribuinte e escolha do usuario: vira a probe da sessao, e um pick
        // seguinte fora do volume volta para ELA, nao para a dominante automatica anterior.
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
        // -1 desliga e devolve o viewport ao caminho normal.
        RendererAccess->SetDebugTargetIndex(
            _Index < 0 ? Smile::Renderer::kNoDebugTarget
                       : static_cast<Smile::u32>(_Index));
        if (_Index >= 0) {
            // Modo unico: o alvo substitui Lit/Heatmap, entao zera o heatmap temporal e
            // volta o view mode base p/ Lit (é o estado ao qual "Desligado" retorna).
            RendererAccess->SetFlickerMode(0);
            RendererAccess->SetGBufferDebugMode(0);
            CurrentViewMode = Lit;
        }
        emit ViewStateChanged();
    }

    void ViewportWidget::ToggleDDGI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetUseGI(!RendererAccess->GetUseGI());
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleReSTIRGI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetUseReSTIRGI(!RendererAccess->GetUseReSTIRGI());
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleReGIR() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetUseReGIR(!RendererAccess->GetUseReGIR());
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleReSTIRGIVisibility() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& ReSTIRGI = RendererAccess->GetReSTIRGI();
        ReSTIRGI.SetVisibility(!ReSTIRGI.GetVisibility());
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleGIFoliageShadows() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // Toggle unico p/ os 3 consumidores do HitShading (DDGI e a fonte da verdade na leitura).
        const bool V = !RendererAccess->GetDDGI().GetFoliageShadows();
        RendererAccess->GetDDGI().SetFoliageShadows(V);
        RendererAccess->GetReSTIRGI().SetFoliageShadows(V);
        RendererAccess->GetReflections().SetFoliageShadows(V);
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleReflectionsCullBackface() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetReflectionsCullBackface(
            !RendererAccess->GetReflectionsCullBackface());
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleReSTIRDI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetUseReSTIRDI(!RendererAccess->GetUseReSTIRDI());
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleGIBackfacePolicy() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // Pelo Renderer: alem dos reservoirs, o NRD/RR/TAA acumulam sobre o resultado e
        // precisam cair juntos, senao o A/B denoisado compara estado misturado.
        RendererAccess->SetGIBackfacePolicy(!RendererAccess->GetGIBackfacePolicy());
        emit GISettingsChanged();
    }

    void ViewportWidget::SetGISurfaceBiasMax(double _Meters) {
        if (!Renderer) return;
        Renderer->SetGISurfaceBiasMax(static_cast<float>(qBound(0.0, _Meters, 2.0)));
        emit GISettingsChanged();
    }

    void ViewportWidget::SetGIVolumeFadeProbes(double _Probes) {
        if (!Renderer) return;
        Renderer->SetGIVolumeFadeProbes(static_cast<float>(qBound(0.0, _Probes, 3.0)));
        emit GISettingsChanged();
    }

    void ViewportWidget::ToggleGTAO() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetUseAO(!RendererAccess->GetUseAO());
        emit RenderSettingsChanged();
    }

    void ViewportWidget::ToggleGTAOHalfRes() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& AO = RendererAccess->GetAO();
        AO.SetHalfRes(!AO.GetHalfRes());
        emit RenderSettingsChanged();
    }

    void ViewportWidget::ToggleReflections() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->SetUseReflections(!RendererAccess->GetUseReflections());
        emit RenderSettingsChanged();
    }

    void ViewportWidget::ToggleNrd() {
        if (!Renderer) return;
        EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                _Renderer.SetUseNrdDenoise(!_Renderer.GetUseNrdDenoise());
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
    }

    void ViewportWidget::SetDenoiserMode(int _Mode) {
        if (!Renderer) return;
        // 0=Nenhum 1=NRD 2=DLSS RR. Selecionar RR forca e trava o upscaler em DLSS (o RR faz o upscale);
        // o Renderer cai p/ NRD se o RR nao estiver disponivel (sem NVIDIA/SDK).
        EnqueueRendererJob(
            QStringLiteral("denoiser"),
            [_Mode](Smile::Renderer& _Renderer) {
                _Renderer.SetDenoiser(static_cast<Smile::EDenoiser>(_Mode));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
    }

    void ViewportWidget::SetUpscalerMode(int _Mode) {
        if (!Renderer) return;
        EnqueueRendererJob(
            QStringLiteral("upscaler-mode"),
            [_Mode](Smile::Renderer& _Renderer) {
                // 0=None 1=FSR 2=DLSS; cai p/ None se indisponivel.
                _Renderer.SetUpscaler(static_cast<Smile::EUpscaler>(_Mode));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
    }

    void ViewportWidget::SetUpscalerQuality(int _Quality) {
        if (!Renderer) return;
        EnqueueRendererJob(
            QStringLiteral("upscaler-quality"),
            [_Quality](Smile::Renderer& _Renderer) {
                _Renderer.SetUpscalerQuality(_Quality); // qualidade compartilhada FSR/DLSS
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
    }

    void ViewportWidget::SetRenderScale(double _Scale) {
        if (!Renderer) return;
        EnqueueRendererJob(
            QStringLiteral("render-scale"),
            [_Scale](Smile::Renderer& _Renderer) {
                _Renderer.SetRenderScale(static_cast<float>(_Scale));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
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
        auto RendererAccess = Renderer.Lock();
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
            auto RendererAccess = Renderer.Lock();
            Smile::FRayEpsilonProfile P = RendererAccess->GetRayEpsilons();
            P.*(K.Field) = static_cast<float>(
                qBound(K.UiMin, _UiValue, K.UiMax) / K.UiScale);
            RendererAccess->SetRayEpsilons(P); // invalida reservoirs + historico do denoiser
            emit GISettingsChanged();
            return;
        }
    }

    void ViewportWidget::ResetRayEpsilons() {
        if (!Renderer) return;
        Renderer->SetRayEpsilons(Smile::FRayEpsilonProfile{}); // volta aos defaults do header
        emit GISettingsChanged();
    }

    void ViewportWidget::SetTAAEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseTAA(_Enabled);
        emit RenderSettingsChanged();
    }

    void ViewportWidget::SetFrustumCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetFrustumCulling(_Enabled);
        emit RenderSettingsChanged();
    }

    void ViewportWidget::SetOcclusionCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetOcclusionCulling(_Enabled);
        emit RenderSettingsChanged();
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
        emit ShadowSettingsChanged();
    }

    void ViewportWidget::SetShadowCacheEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetCascadeCache(_Enabled);
        emit ShadowSettingsChanged();
    }

    void ViewportWidget::SetShadowDebugCascades(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetDebugCascades(_Enabled);
        emit ShadowSettingsChanged();
    }

    void ViewportWidget::SetShadowMaxDistance(double _Distance) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetMaxDistance(static_cast<Smile::f32>(_Distance));
        emit ShadowSettingsChanged();
    }

    void ViewportWidget::SetShadowDepthBias(double _Bias) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetDepthBias(static_cast<Smile::f32>(_Bias));
        emit ShadowSettingsChanged();
    }

    void ViewportWidget::SetShadowMinCasterTexels(double _Texels) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetMinCasterTexels(static_cast<Smile::f32>(_Texels));
        emit ShadowSettingsChanged();
    }

    void ViewportWidget::SetShadowCascadeBiasScale(int _Cascade, double _Scale) {
        if (!Renderer || _Cascade < 0) return;
        Renderer->GetSunShadows().SetCascadeBiasScale(static_cast<Smile::u32>(_Cascade),
                                                      static_cast<Smile::f32>(_Scale));
        emit ShadowSettingsChanged();
    }

    double ViewportWidget::GetShadowSunAngle() const {
        return Renderer ? Renderer->GetSunShadows().GetSunAngularSize() : 0.53;
    }

    void ViewportWidget::SetShadowSunAngle(double _Degrees) {
        if (!Renderer) return;
        Renderer->GetSunShadows().SetSunAngularSize(static_cast<Smile::f32>(_Degrees));
        emit ShadowSettingsChanged();
    }

    bool ViewportWidget::AreSunShaftsEnabled() const {
        return Renderer ? Renderer->GetUseSunShafts() : true;
    }

    void ViewportWidget::SetSunShaftsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseSunShafts(_Enabled);
        emit SunShaftsSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsIntensity() const {
        return Renderer ? Renderer->GetSunShafts().GetVolIntensity() : 1.0;
    }

    void ViewportWidget::SetSunShaftsIntensity(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolIntensity(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsDust() const {
        return Renderer ? Renderer->GetSunShafts().GetVolDust() : 8.0;
    }

    void ViewportWidget::SetSunShaftsDust(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolDust(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsPhaseG() const {
        return Renderer ? Renderer->GetSunShafts().GetVolPhaseG() : 0.7;
    }

    void ViewportWidget::SetSunShaftsPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolPhaseG(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    int ViewportWidget::GetSunShaftsSteps() const {
        return Renderer ? static_cast<int>(Renderer->GetSunShafts().GetVolSteps()) : 32;
    }

    void ViewportWidget::SetSunShaftsSteps(int _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolSteps(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double ViewportWidget::GetSunShaftsRange() const {
        return Renderer ? Renderer->GetSunShafts().GetVolMaxDist() : 128.0;
    }

    void ViewportWidget::SetSunShaftsRange(double _Value) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolMaxDist(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    bool ViewportWidget::AreSunShaftsTemporal() const {
        return Renderer ? Renderer->GetSunShafts().GetVolTemporal() : true;
    }

    void ViewportWidget::SetSunShaftsTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetSunShafts().SetVolTemporal(_Enabled);
        emit SunShaftsSettingsChanged();
    }

    bool ViewportWidget::IsVolFogEnabled() const {
        return Renderer ? Renderer->GetUseVolumetricFog() : true;
    }

    void ViewportWidget::SetVolFogEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseVolumetricFog(_Enabled);
        emit VolFogSettingsChanged();
    }

    double ViewportWidget::GetVolFogDistance() const {
        return Renderer ? Renderer->GetVolumetricFog().GetMaxDistance() : 100.0;
    }

    void ViewportWidget::SetVolFogDistance(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetMaxDistance(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
    }

    double ViewportWidget::GetVolFogPhaseG() const {
        return Renderer ? Renderer->GetVolumetricFog().GetPhaseG() : 0.3;
    }

    void ViewportWidget::SetVolFogPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetPhaseG(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
    }

    double ViewportWidget::GetVolFogDensity() const {
        return Renderer ? Renderer->GetVolumetricFog().GetExtinctionScale() : 1.0;
    }

    void ViewportWidget::SetVolFogDensity(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetExtinctionScale(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
    }

    double ViewportWidget::GetVolFogAmbient() const {
        return Renderer ? Renderer->GetVolumetricFog().GetAmbientIntensity() : 1.0;
    }

    void ViewportWidget::SetVolFogAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetAmbientIntensity(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
    }

    bool ViewportWidget::IsVolFogTemporal() const {
        return Renderer ? Renderer->GetVolumetricFog().GetTemporal() : true;
    }

    void ViewportWidget::SetVolFogTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetTemporal(_Enabled);
        emit VolFogSettingsChanged();
    }

    double ViewportWidget::GetVolFogLights() const {
        return Renderer ? Renderer->GetVolumetricFog().GetLightsIntensity() : 1.0;
    }

    void ViewportWidget::SetVolFogLights(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetLightsIntensity(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
    }

    bool ViewportWidget::IsVolFogConsDepth() const {
        return Renderer ? Renderer->GetVolumetricFog().GetConservativeDepth() : true;
    }

    void ViewportWidget::SetVolFogConsDepth(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricFog().SetConservativeDepth(_Enabled);
        emit VolFogSettingsChanged();
    }

    bool ViewportWidget::AreCloudsEnabled() const {
        return Renderer ? Renderer->GetUseClouds() : false;
    }

    void ViewportWidget::SetCloudsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseClouds(_Enabled);
        emit CloudSettingsChanged();
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
        emit CloudSettingsChanged();
    }

    void ViewportWidget::SetCloudsHalfRes(bool _HalfRes) {
        if (!Renderer) return;
        EnqueueRendererJob(
            QStringLiteral("cloud-half-res"),
            [_HalfRes](Smile::Renderer& _Renderer) {
                _Renderer.SetCloudsHalfRes(_HalfRes);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit CloudSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
    }

    double ViewportWidget::GetCloudCoverage() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetCoverage() : 0.45;
    }

    void ViewportWidget::SetCloudCoverage(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetCoverage(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudDensity() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetDensityScale() : 1.6;
    }

    void ViewportWidget::SetCloudDensity(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetDensityScale(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudWindSpeed() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetWindSpeed() : 0.01;
    }

    void ViewportWidget::SetCloudWindSpeed(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetWindSpeed(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudErosion() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetErosion() : 0.45;
    }

    void ViewportWidget::SetCloudErosion(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetErosion(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudPhaseG() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetPhaseG() : 0.8;
    }

    void ViewportWidget::SetCloudPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetPhaseG(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudPowder() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetPowder() : 0.5;
    }

    void ViewportWidget::SetCloudPowder(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetPowder(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudAmbient() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetAmbientScale() : 1.0;
    }

    void ViewportWidget::SetCloudAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetAmbientScale(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudTypeBias() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetCloudTypeBias() : 0.0;
    }

    void ViewportWidget::SetCloudTypeBias(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetCloudTypeBias(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudPeakVariation() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetPeakVariation() : 1.0;
    }

    void ViewportWidget::SetCloudPeakVariation(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetPeakVariation(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
    }

    int ViewportWidget::GetCloudWeatherSeed() const {
        return Renderer ? static_cast<int>(Renderer->GetCloudWeatherSeed()) : 1337;
    }

    void ViewportWidget::SetCloudWeatherSeed(int _Seed) {
        if (!Renderer) return;
        const Smile::u32 Seed = static_cast<Smile::u32>(_Seed < 0 ? 0 : _Seed);
        EnqueueRendererJob(
            QStringLiteral("cloud-weather-seed"),
            [Seed](Smile::Renderer& _Renderer) {
                _Renderer.SetCloudWeatherSeed(Seed);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
    }

    int ViewportWidget::GetCloudWeatherCells() const {
        return Renderer ? static_cast<int>(Renderer->GetCloudWeatherCells()) : 3;
    }

    void ViewportWidget::SetCloudWeatherCells(int _Mult) {
        if (!Renderer) return;
        const Smile::u32 Cells = static_cast<Smile::u32>(_Mult < 1 ? 1 : _Mult);
        EnqueueRendererJob(
            QStringLiteral("cloud-weather-cells"),
            [Cells](Smile::Renderer& _Renderer) {
                _Renderer.SetCloudWeatherCells(Cells);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
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
        const std::wstring NativePath = Path.toStdWString();
        EnqueueRendererJob(
            {},
            [NativePath](Smile::Renderer& _Renderer) {
                RenderThread::JobCompletion Result;
                Result.Success = _Renderer.LoadCloudWeatherTexture(NativePath);
                if (!Result.Success) Result.Error = "falha ao carregar weather map";
                return Result;
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
    }

    void ViewportWidget::ClearCloudWeatherTexture() {
        if (!Renderer) return;
        EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                _Renderer.ClearCloudWeatherTexture();
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) emit CloudSettingsChanged();
            });
    }

    bool ViewportWidget::AreCloudShadowsEnabled() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetShadowsEnabled() : true;
    }

    void ViewportWidget::SetCloudShadowsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetShadowsEnabled(_Enabled);
        emit CloudSettingsChanged();
    }

    double ViewportWidget::GetCloudShadowStrength() const {
        return Renderer ? Renderer->GetVolumetricClouds().GetShadowStrength() : 1.0;
    }

    void ViewportWidget::SetCloudShadowStrength(double _Value) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetShadowStrength(static_cast<Smile::f32>(_Value));
        emit CloudSettingsChanged();
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
        emit CloudSettingsChanged();
    }

    int ViewportWidget::GetCloudMarchSteps() const {
        return Renderer ? static_cast<int>(Renderer->GetVolumetricClouds().GetMarchSteps()) : 128;
    }

    void ViewportWidget::SetCloudMarchSteps(int _Steps) {
        if (!Renderer) return;
        Renderer->GetVolumetricClouds().SetMarchSteps(static_cast<Smile::f32>(_Steps));
        emit CloudSettingsChanged();
    }

    // ---- Clima (FWeather; defaults espelham o struct p/ antes do renderer existir) ----
    double ViewportWidget::GetRainAmount() const {
        return Renderer ? Renderer->GetWeather().RainAmount : 0.0;
    }
    void ViewportWidget::SetRainAmount(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().RainAmount = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit WeatherSettingsChanged();
    }

    double ViewportWidget::GetPuddleAmount() const {
        return Renderer ? Renderer->GetWeather().PuddleAmount : 0.65;
    }
    void ViewportWidget::SetPuddleAmount(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().PuddleAmount = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit WeatherSettingsChanged();
    }

    double ViewportWidget::GetPuddleScale() const {
        return Renderer ? Renderer->GetWeather().PuddleScale : 8.0;
    }
    void ViewportWidget::SetPuddleScale(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().PuddleScale = static_cast<Smile::f32>(std::clamp(_Value, 1.0, 64.0));
        emit WeatherSettingsChanged();
    }

    double ViewportWidget::GetRippleStrength() const {
        return Renderer ? Renderer->GetWeather().RippleStrength : 1.0;
    }
    void ViewportWidget::SetRippleStrength(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().RippleStrength = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 2.0));
        emit WeatherSettingsChanged();
    }

    double ViewportWidget::GetWetDarkening() const {
        return Renderer ? Renderer->GetWeather().WetDarkening : 0.85;
    }
    void ViewportWidget::SetWetDarkening(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().WetDarkening = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit WeatherSettingsChanged();
    }

    double ViewportWidget::GetCurtainAmount() const {
        return Renderer ? Renderer->GetWeather().CurtainAmount : 1.0;
    }
    void ViewportWidget::SetCurtainAmount(double _Value) {
        if (!Renderer) return;
        Renderer->GetWeather().CurtainAmount = static_cast<Smile::f32>(std::clamp(_Value, 0.0, 1.0));
        emit WeatherSettingsChanged();
    }

    bool ViewportWidget::IsRainOcclusion() const {
        return Renderer ? Renderer->GetWeather().RainOcclusion : true;
    }
    void ViewportWidget::SetRainOcclusion(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetWeather().RainOcclusion = _Enabled;
        emit WeatherSettingsChanged();
    }

    bool ViewportWidget::AreRainParticles() const {
        return Renderer ? Renderer->GetWeather().RainParticles : true;
    }
    void ViewportWidget::SetRainParticles(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetWeather().RainParticles = _Enabled;
        emit WeatherSettingsChanged();
    }

    bool ViewportWidget::IsWeatherDriveSky() const {
        return Renderer ? Renderer->GetWeather().DriveSky : true;
    }
    void ViewportWidget::SetWeatherDriveSky(bool _Enabled) {
        if (!Renderer) return;
        Renderer->GetWeather().DriveSky = _Enabled;
        emit WeatherSettingsChanged();
    }

    void ViewportWidget::SetDepthPrepassEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetDepthPrepass(_Enabled);
        emit RenderSettingsChanged();
    }

    void ViewportWidget::ResetRenderSettings() {
        if (!Renderer) return;
        // O padrao segue o backend recomendado (DLSS em NVIDIA, senao FSR, senao nativo), mas em
        // escala 1:1 (tier 0 = 100%): reconstroi/faz AA sem upscale. Bate com o default do Renderer.
        EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                const Smile::EUpscaler Recommended =
                    _Renderer.UpscalerAvailable(Smile::EUpscaler::DLSS)
                        ? Smile::EUpscaler::DLSS
                        : (_Renderer.UpscalerAvailable(Smile::EUpscaler::FSR)
                               ? Smile::EUpscaler::FSR
                               : Smile::EUpscaler::None);
                _Renderer.SetUpscalerQuality(0);
                _Renderer.SetUpscaler(Recommended);
                _Renderer.SetUseTAA(true);
                _Renderer.SetFrustumCulling(true);
                _Renderer.SetDepthPrepass(false);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    NotifyDebugTargetsChanged();
                }
            });
    }

    void ViewportWidget::RequestSettings() {
        emit SettingsRequested();
    }

    void ViewportWidget::EnsureRendererIsInitialized() {
        if (RendererShutdownRequested || RendererStoppedFlag || RendererThread.IsStarted()) return;
        const HWND hWnd = reinterpret_cast<HWND>(winId());
        // O viewport e um HWND pintado diretamente pelo D3D. Limpa-o antes de iniciar a worker
        // para que nem um unico WM_PAINT exponha o fundo branco da janela nativa.
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
        {
            auto RendererAccess = Renderer.Lock();
            if (RendererAccess && RendererAccess->IsInitialized())
                CaptureTelemetry(*RendererAccess);
        }
        FrameTimer.restart();
        emit RendererInitialized();
        emit DebugTargetsChanged();   // os alvos foram publicados na criacao dos targets internos
        if (isVisible() && RedrawTimer) RedrawTimer->start();
    }

    void ViewportWidget::OnRendererInitializationFailed(const QString& _Error) {
        Q_UNUSED(_Error);
        Initialized = false;
        if (RedrawTimer) RedrawTimer->stop();
    }

    bool ViewportWidget::CommitPreparedSceneAsync(
            std::shared_ptr<Smile::FPreparedCookedScene> _Prepared,
            bool _Additive,
            SceneCommitCallback _Completion) {
        if (!_Prepared) return false;
        return EnqueueRendererJob(
            {},
            [Prepared = std::move(_Prepared), _Additive](Smile::Renderer& _Renderer) mutable {
                RenderThread::JobCompletion Result;
                Result.Success = _Renderer.CommitCookedScene(
                    std::move(Prepared), _Additive);
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

        // Sliders e toggles podem publicar outro valor enquanto um rebuild anterior trabalha.
        // Conserva apenas o ultimo job ainda nao iniciado da mesma familia.
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

                        // Primeiro publica bridges/sinais; so depois permite que outro job ou
                        // frame observe o novo estado do renderer.
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
        // Espera o primeiro layout terminar antes de fixar o tamanho inicial da swapchain. O
        // fundo Qt ja e preto; evitar um ResizeBuffers logo depois do Initialize remove um flush
        // caro da GUI sem atrasar perceptivelmente o boot pesado do renderer.
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
        // Sai primeiro do callback nativo WM_EXITSIZEMOVE; Flush/ResizeBuffers no proprio
        // window-proc prolongaria o dispatch do Windows e aumentaria a chance de reentrancia.
        ResizeDebounce->start(0);
    }

    void ViewportWidget::ApplyPendingResize() {
        if (!Initialized || InteractiveResize || !Renderer) return;

        if (RendererThread.HasFrameInFlight() || RendererThread.HasJobInFlight()) return;
        auto RendererAccess = Renderer.TryLock();
        if (!RendererAccess) {
            // Nao congela a GUI esperando Present; conserva PendingResizeSize e tenta de novo
            // assim que a thread de renderizacao liberar o Renderer.
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
        // Resize realoca SRVs: os alvos foram re-registrados com slots novos.
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

        // Nunca espera o Present da thread de renderizacao. Se outro comando sincronizado da
        // UI estiver usando o Renderer, este tick e simplesmente coalescido com o proximo.
        auto RendererAccess = Renderer.TryLock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) {
            if (isVisible() && RedrawTimer) RedrawTimer->start(1);
            return;
        }

        // Resolucao de NANOSEGUNDOS: a >200 FPS o dt em ms inteiros (4/5/6ms) fazia o
        // FPS pular feito louco. nsecsElapsed da precisao sub-ms p/ dt e FPS estaveis.
        float DeltaTime = static_cast<float>(static_cast<double>(FrameTimer.nsecsElapsed()) / 1.0e9);
        FrameTimer.restart();
        DeltaTime = Smile::Clamp(DeltaTime, 0.0001f, 0.1f);
        LastFrameDeltaTime = DeltaTime;

        Smile::CameraInput CameraInput;
        CameraInput.Look  = MouseLookActive
            ? Smile::Vec2{ MouseDelta.X * kMouseSensitivity,
                          -MouseDelta.Y * kMouseSensitivity }   
            : Smile::Vec2::Zero();
        // Consome apenas o delta submetido. Eventos de mouse que chegarem enquanto a render
        // thread trabalha acumulam para o proximo frame, em vez de serem apagados no callback.
        MouseDelta = Smile::Vec2::Zero();
        CameraInput.Move  = Smile::Vec3{
            static_cast<float>(IsHeld(Qt::Key_D) - IsHeld(Qt::Key_A)),   
            static_cast<float>(IsHeld(Qt::Key_E) - IsHeld(Qt::Key_Q)),   
            static_cast<float>(IsHeld(Qt::Key_W) - IsHeld(Qt::Key_S)),   
        };
        CameraInput.Speed = IsHeld(Qt::Key_Shift) ? 4.0f : 1.0f;

        RendererAccess->UpdateCamera(CameraInput, DeltaTime);
        FlushPendingGizmoInput(*RendererAccess);
        // Gizmo (editor-side): submete as setas ao DebugDraw da Engine ANTES do RenderFrame, que
        // as desenha e limpa. Geometria world-space -> projetada com a VP do frame (sem lag).
        GizmoCtrl.Submit(*RendererAccess);
        if (RendererThread.RequestFrame()) {
            if (!RendererOwnsSurface) {
                RendererOwnsSurface = true;
                setAutoFillBackground(false);
                setAttribute(Qt::WA_NoSystemBackground, true);
                setAttribute(Qt::WA_OpaquePaintEvent, true);
            }
            // Um timer de intervalo zero ficaria acordando a GUI em busy-loop durante o
            // Present. O callback do frame rearma o timer quando o slot unico fica livre.
            // Single-shot: so o callback rearma o proximo tick.
        }
    }

    void ViewportWidget::OnFrameCompleted(bool _Success, bool _Terminal,
                                          const QString& _Error) {
        if (!_Success && !_Error.isEmpty() && _Terminal)
            Smile::LogError("Render thread encerrada apos falhas consecutivas: " +
                            _Error.toStdString());

        if (_Success && !RendererShutdownRequested) OnFrameRendered();

        // Ownership unico de FrameOutstanding: toda conclusao de frame passa por aqui,
        // independentemente de sucesso, falha recuperavel ou shutdown concorrente.
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
        // Conserva o lock ate FrameReady terminar de atualizar todas as bridges. O proximo
        // frame so pode ser solicitado depois deste callback voltar ao event loop.
        auto RendererAccess = Renderer.Lock();
        if (!RendererAccess || !RendererAccess->IsInitialized()) return;

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
                emit DebugSettingsChanged();
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
                    Smile::LogDebug("Selecionado [" + std::to_string(PickedIndex) + "] " +
                                   Renderables[static_cast<size_t>(PickedIndex)].Name);
                }
            } else {
                Renderer->ClearSelection();
                Smile::LogDebug("Selecao limpa (clique no vazio)");
            }
            emit ObjectSelected(PickedIndex);
        }

        // FPS suavizado por media exponencial (EMA) — leitura estavel em vez do valor
        // instantaneo 1/dt (que oscila muito frame a frame).
        const float InstFPS = LastFrameDeltaTime > 0.0f ? 1.0f / LastFrameDeltaTime : 0.0f;
        LastFPS = (LastFPS > 0.0f) ? (LastFPS * 0.96f + InstFPS * 0.04f) : InstFPS;

        // QML nao precisa reler/formatar 17 propriedades de telemetria a cada frame. O pulso
        // FrameReady permanece imediato para preview assincrono, picking, gizmos e bridges.
        if (!TelemetryTimer.isValid() || TelemetryTimer.elapsed() >= 200) {
            TelemetryTimer.restart();
            CaptureTelemetry(*RendererAccess);
            emit TelemetryUpdated();
        }
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
        auto RendererAccess = Renderer.Lock();
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

    void ViewportWidget::FlushPendingGizmoInput(Smile::Renderer& _Renderer) {
        if (GizmoMousePending) {
            GizmoCtrl.OnMouseMove(_Renderer, PendingGizmoMouseX, PendingGizmoMouseY);
            GizmoMousePending = false;
        }
        if (GizmoReleasePending) {
            GizmoCtrl.OnMouseRelease();
            GizmoReleasePending = false;
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
        // Clique esquerdo (fora do modo camera) = picking. O backbuffer/IDTarget tem o tamanho
        // logico do widget (Resize usa size() logico), entao a posicao do evento mapeia 1:1.
        // O passe de ID roda no proximo frame; o resultado e coletado em OnRenderTimer.
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
                // Resolve hover/release coalescidos antes de iniciar uma nova interacao.
                FlushPendingGizmoInput(*RendererAccess);
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
                if (!GizmoCtrl.OnMousePress(*RendererAccess, Px, Py)) {
                    const int LightHit = PickLightMarker(Px, Py);
                    if (LightHit >= 0) {
                        Renderer->SetSelectedLight(LightHit);
                        Renderer->ClearSelection(); // selecoes exclusivas
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
                GizmoReleasePending = true;

                // Finaliza imediatamente se o frame ja liberou o Renderer; caso contrario,
                // OnRenderTimer aplica primeiro a ultima posicao e depois encerra o drag.
                auto RendererAccess = Renderer.TryLock();
                if (RendererAccess && RendererAccess->IsInitialized())
                    FlushPendingGizmoInput(*RendererAccess);
            } else {
                GizmoCtrl.OnMouseRelease();
            }
        }
        QWidget::mouseReleaseEvent(_Event);
    }

    void ViewportWidget::mouseMoveEvent(QMouseEvent* _Event) {
        if (!MouseLookActive) {
            // Sem camera-look: roteia pro gizmo. Arrastando -> move o objeto; senao -> hover (destaca
            // o eixo sob o cursor). Se a render thread estiver ocupada, conserva a posicao mais
            // recente para o proximo frame, sem bloquear nem perder o movimento do drag.
            if (!GizmoReleasePending) {
                const QPointF P = _Event->position();
                PendingGizmoMouseX = static_cast<Smile::u32>(P.x() > 0.0 ? P.x() : 0.0);
                PendingGizmoMouseY = static_cast<Smile::u32>(P.y() > 0.0 ? P.y() : 0.0);
                GizmoMousePending = true;
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
