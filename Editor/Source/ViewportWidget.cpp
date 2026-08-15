#include "SmileEditor/ViewportWidget.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/RenderSettings.h"
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
    static constexpr double kRadiansToDegrees = 57.295779513082320876;
    static constexpr double kDegreesToRadians = 0.01745329251994329577;

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

        // Linhas-filhas por recurso, no mesmo contrato dos passes de GPU (name/text/shareText):
        // a categoria vira expansivel e mostra quem paga a conta. A fracao dos filhos e relativa
        // a PROPRIA categoria, nao ao total — dentro de "GI e reflexos" o que interessa e quanto
        // daquele bloco cada recurso ocupa.
        const auto Labels = Smile::VramTracker::LabelBreakdown();
        for (const auto& [Index, Bytes] : Sorted) {
            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QString::fromUtf8(
                Smile::VramTracker::CategoryName(static_cast<EVramCategory>(Index))));
            Row.insert(QStringLiteral("text"), FormatBytes(Bytes));
            Row.insert(QStringLiteral("frac"), static_cast<double>(Bytes) / Total);

            QVariantList Children;
            Smile::u64 Labeled = 0;
            for (const auto& E : Labels) {
                if (static_cast<size_t>(E.Category) != Index) continue;
                QVariantMap Child;
                Child.insert(QStringLiteral("name"), QString::fromUtf8(E.Label));
                Child.insert(QStringLiteral("text"), FormatBytes(E.Bytes));
                Child.insert(QStringLiteral("shareText"),
                             QString::number(Bytes ? (100.0 * static_cast<double>(E.Bytes) /
                                                      static_cast<double>(Bytes)) : 0.0, 'f', 0) + "%");
                Children.push_back(Child);
                Labeled += E.Bytes;
            }
            // "Outros" fecha a soma: sem ele os filhos nao batem com o total da categoria e a
            // leitura fica errada justamente onde falta instrumentacao.
            if (!Children.isEmpty() && Bytes > Labeled) {
                const Smile::u64 Rest = Bytes - Labeled;
                QVariantMap Child;
                Child.insert(QStringLiteral("name"), QStringLiteral("Outros (sem rótulo)"));
                Child.insert(QStringLiteral("text"), FormatBytes(Rest));
                Child.insert(QStringLiteral("shareText"),
                             QString::number(100.0 * static_cast<double>(Rest) /
                                             static_cast<double>(Bytes), 'f', 0) + "%");
                Child.insert(QStringLiteral("active"), false); // cinza, como pass inativo
                Children.push_back(Child);
            }
            Row.insert(QStringLiteral("hasChildren"), !Children.isEmpty());
            Row.insert(QStringLiteral("children"), Children);
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

    QVariantList ViewportWidget::GetShadowCascades() const {
        return Telemetry.ShadowCascades;
    }

    // Tabela "Sombra do sol (CSM)": o que cada cascata desenha e com que frequencia ela
    // dispara. As duas colunas juntas sao a medida que interessa — uma cascata cara que roda
    // 16 frames em 64 custa menos por frame que uma barata que roda nos 64 —, e e sobre elas
    // que a separacao static/dynamic vai ser avaliada.
    //
    // `casters` e do ULTIMO update daquela cascata, nao do frame corrente (ver FCascadeStats):
    // cascata congelada continua mostrando o que ela desenha quando acorda.
    QVariantList ViewportWidget::BuildShadowCascades(Smile::Renderer& _Renderer) const {
        QVariantList Rows;
        const Smile::FSunShadows& Csm = _Renderer.GetSunShadows();
        if (!Csm.IsInitialized()) return Rows;

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        const int Submitted = static_cast<int>(Csm.GetSubmittedCasterCount());
        double MeanPerFrame = 0.0, MeanDynamic = 0.0;

        for (Smile::u32 C = 0; C < Smile::FSunShadows::kNumCascades; ++C) {
            const auto& S = Csm.CascadeStats(C);
            // popcount da janela de 64 frames. Sem <bit> para nao exigir C++20 aqui.
            // A taxa que interessa é a de RE-RASTERIZAÇÃO DO ESTÁTICO: é ela que a F2 move.
            // A de runtime (UpdateHistory) fica em 64/64 sempre que houver dinâmico, porque
            // cópia mais dinâmico acontece todo frame — e é barata, que é o ponto.
            int Updates = 0;
            for (Smile::u64 B = S.StaticHistory; B; B &= B - 1) ++Updates;
            const double Rate = Updates / 64.0;
            // Estático leva a taxa; dinâmico não. Essa assimetria É o cache: o primeiro só é
            // pago quando o mapa invalida, o segundo é redesenhado todo frame de qualquer
            // forma. O piso é o que nenhum cache remove.
            MeanPerFrame += S.SubmittedStatic * Rate + S.SubmittedDynamic;
            MeanDynamic  += S.SubmittedDynamic;

            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QStringLiteral("Cascata %1").arg(C));
            // Estático · dinâmico é a leitura central: o primeiro número é o que o cache pode
            // reter, o segundo é o que sobra para redesenhar todo frame.
            Row.insert(QStringLiteral("text"),
                       QStringLiteral("%1 · %2")
                           .arg(Loc.toString(static_cast<int>(S.SubmittedStatic)))
                           .arg(Loc.toString(static_cast<int>(S.SubmittedDynamic))));
            Row.insert(QStringLiteral("shareText"),
                       QStringLiteral("%1/64 fr").arg(Updates));
            // Quanto cada filtro cortou, na ordem em que o passe aplica (tamanho e depois
            // planos): um objeto pequeno E fora da fatia conta no primeiro. O Δ fecha a linha
            // com o quanto o fit andou — é o que diz se o mapa anterior era reaproveitável.
            Row.insert(QStringLiteral("cullText"),
                       QStringLiteral("−%1 tam · −%2 fatia · Δ%3 tx")
                           .arg(S.CulledSize).arg(S.CulledPlanes)
                           .arg(Loc.toString(S.SnapDeltaTexels, 'f', 0)));
            Row.insert(QStringLiteral("frac"),
                       Submitted > 0 ? static_cast<double>(S.Submitted) / Submitted : 0.0);
            Row.insert(QStringLiteral("frozen"), !S.UpdatedThisFrame);
            Rows.push_back(Row);
        }

        // Soma das 4 cascatas: o MESMO objeto é desenhado uma vez por cascata que o aceita,
        // então isto passa de 1.542 sem contradição — a lista é de objetos, isto é de draws.
        // O teto é 4 × lista (nada cullado, nada cacheado), e é contra ele que o número tem de
        // ser lido: é o que culling e cache já cortam hoje, e a base do A/B da F2/F3.
        QVariantMap Total;
        Total.insert(QStringLiteral("name"), QStringLiteral("Draws de sombra/frame"));
        Total.insert(QStringLiteral("text"), Loc.toString(MeanPerFrame, 'f', 0));
        Total.insert(QStringLiteral("shareText"),
                     QStringLiteral("de %1").arg(
                         Loc.toString(Submitted * static_cast<int>(
                             Smile::FSunShadows::kNumCascades))));
        // O que sobraria com o cache perfeito: só o dinâmico, todo frame, em toda cascata.
        QVariantMap Floor;
        Floor.insert(QStringLiteral("name"), QStringLiteral("Piso com cache"));
        Floor.insert(QStringLiteral("text"), Loc.toString(MeanDynamic, 'f', 0));
        Floor.insert(QStringLiteral("shareText"),
                     MeanPerFrame > 0.0
                         ? QStringLiteral("%1%").arg(Loc.toString(
                               100.0 * MeanDynamic / MeanPerFrame, 'f', 0))
                         : QStringLiteral("—"));
        Floor.insert(QStringLiteral("cullText"), QString());
        Floor.insert(QStringLiteral("frac"), 0.0);
        Floor.insert(QStringLiteral("frozen"), false);
        Floor.insert(QStringLiteral("summary"), true);
        Total.insert(QStringLiteral("cullText"), QString());
        Total.insert(QStringLiteral("frac"), 0.0);
        Total.insert(QStringLiteral("frozen"), false);
        Total.insert(QStringLiteral("summary"), true);
        Rows.push_back(Total);
        Rows.push_back(Floor);
        return Rows;
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
        Next.ShadowCascades = BuildShadowCascades(_Renderer);
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

    // Decomposicao UNICA do indice de sonda que o painel inspeciona: global -> (cascata, indice
    // local, coord). Cinco consumidores dependem dela — coord, posicao de mundo, descricao do
    // grid, alcance de distancia e o stepping — e antes cada um refazia a conta assumindo indice
    // LOCAL. Em qualquer cascata acima da 0 isso dava coordenada fora do grid, posicao calculada
    // com a origem da cascata errada, e um stepping que reconstruia o indice sem a base da
    // cascata, saltando para a 0 no primeiro passo.
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
        // ARMAZENAMENTO -> GEOMETRIA (o mesmo DDGI_GeometricCoord dos shaders). O indice de sonda
        // enderessa um SLOT; com a cascata fina rolando atras da camera, a coordenada de grid e a
        // posicao de mundo que o painel mostra so batem com a sonda de verdade depois de desfazer
        // o scroll. Sem isto, o painel apontaria para uma sonda `scroll` celulas ao lado — e o
        // erro seria pior parado do que andando, porque pareceria estavel.
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
        // Origem e espacamento DA CASCATA, nao os da grossa que GridMin()/Spacing() devolvem.
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
        // A cascata entra no rotulo: sem ela, duas sondas em cascatas diferentes no mesmo
        // vertice local sao indistinguiveis.
        return QStringLiteral("cascata %1 · grid (%2, %3, %4)")
            .arg(C.Cascade).arg(C.X).arg(C.Y).arg(C.Z);
    }

    QString ViewportWidget::GetDebugProbeWorld() const {
        FDebugProbeCoord C;
        if (!GetDebugProbeCoordValues(C)) return QString();
        // Origem e espacamento DA CASCATA (ver a decomposicao): com os da grossa, a posicao da
        // sonda de uma fina sairia em outro lugar do mundo.
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
        // 2,6 espacamentos DA CASCATA — o mesmo clamp que o DDGIUpdateDist aplica aos momentos.
        // O DistanceMomentMax() do FDDGI e um valor so e descreveria a fina para uma sonda grossa.
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

        // Grade FISICA de tiles do atlas. Tem de bater com o `total = tilesX*tilesY` do
        // DebugView.ps, que e quem reempacota para a tela — daqui sai o indice que o clique
        // seleciona. Sai do DDGI e nao de CountX*CountZ: o empacotamento virou 2D e a largura
        // deixou de ser um par de eixos do grid (ver DDGI_TileOrigin).
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
        if (AtlasIndex < 0 || AtlasIndex >= Total) return; // celula vazia da ultima linha

        // Inversa do AtlasTileFromProbe, e mora no FDDGI junto com ela: o atlas tem ordem propria
        // (plano (x,z) nas colunas, y nas linhas, enrolado em bandas) e duplicar essa conta aqui
        // era o que fazia o clique apontar para outra sonda quando o layout mudava. Devolve false
        // na celula de SOBRA — a ultima banda pode ter colunas que nao sao sonda nenhuma.
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
        // O passo e GEOMETRICO — andar uma sonda quer dizer andar uma celula no MUNDO, e e o que
        // o `C` traz (o GetDebugProbeCoordValues ja desfez o scroll).
        const int Counts[3] = { C.CountX, C.CountY, C.CountZ };
        int Geo[3] = { std::clamp(C.X + _DX, 0, C.CountX - 1),
                       std::clamp(C.Y + _DY, 0, C.CountY - 1),
                       std::clamp(C.Z + _DZ, 0, C.CountZ - 1) };
        // GEOMETRIA -> ARMAZENAMENTO antes de linearizar (o DDGI_StorageCoord dos shaders). Sem
        // isto o scroll seria aplicado DUAS vezes — uma vez desfeito na leitura, nunca refeito na
        // escrita — e cada passo saltaria para uma sonda a `scroll` celulas de distancia. Parado,
        // o salto e constante e parece um bug de indexacao; e so andando que ele muda de tamanho.
        const auto& DDGI = Renderer->GetDDGI();
        for (int A = 0; A < 3; ++A) {
            Geo[A] += DDGI.CascadeScroll(static_cast<Smile::u32>(C.Cascade), A);
            if (Geo[A] >= Counts[A]) Geo[A] -= Counts[A];
        }
        // O indice reconstruido tem de voltar para o espaco GLOBAL. Sem a base da cascata, o
        // primeiro passo do navegador saltaria da cascata inspecionada para a 0, e o painel
        // passaria a mostrar outra sonda sem dizer que mudou de cascata.
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
                    // O indice publicado e GLOBAL: cascata * sondasPorCascata + local. Decodifica-lo
                    // direto daria coordenada fora do grid em qualquer cascata acima da 0 — no
                    // Bistro o Z da cascata 1 comecaria em 24, num grid que tem 24.
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
                    // A cascata entra na coordenada mostrada: sem ela, duas sondas de cascatas
                    // diferentes no mesmo vertice local apareceriam com o mesmo rotulo.
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
                // Composicao de cascatas: e ela que explica por que ha DUAS paginas de oito taps
                // e com que peso cada uma entrou. Sem isto o painel mostra o `cN` de cada tap sem
                // dizer qual foi a DECISAO do seletor — o leitor teria de reconstrui-la somando
                // pesos, que e justamente o que o diagnostico existe para poupar.
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
        // Edicao da selecao. Nao entra no HeldKeys (aquilo e o estado continuo do voo da
        // camera); sao acoes de uma vez so, e o autorepeat tem de ser ignorado — segurar Del
        // apagaria a cena inteira objeto a objeto.
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
            GizmoCtrl.OnMouseMove(_Renderer, PendingGizmoMouseX, PendingGizmoMouseY);
            GizmoMousePending = false;
        }
        if (GizmoReleasePending) {
            GizmoCtrl.OnMouseRelease(_Renderer);
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
                    const int LightHit = GizmoCtrl.PickLightIcon(*RendererAccess, Px, Py);
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
                // Sem arraste em curso o release e so reset de estado, mas passou a precisar
                // do Renderer (limpa o dragging do CSM), entao segue o mesmo caminho diferido:
                // se o frame nao liberou o Renderer agora, o FlushPendingGizmoInput pega no
                // proximo. Adiar um frame um reset de estado ja neutro nao tem efeito visivel.
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
