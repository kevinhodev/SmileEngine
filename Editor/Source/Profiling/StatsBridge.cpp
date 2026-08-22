#include "SmileEditor/Profiling/StatsBridge.h"
#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Debug/VramTracker.h"
#include <QLocale>
#include <QVariantMap>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace SmileEditor {
    StatsBridge::StatsBridge(QObject* _Parent) : QObject(_Parent) {}

    void StatsBridge::SetViewport(ViewportWidget* _Value) {
        if (Viewport == _Value) return;
        if (Viewport) disconnect(Viewport, nullptr, this, nullptr);

        Viewport = _Value;
        Renderer = Viewport ? Viewport->GetRenderer() : RendererHandle{};
        RefreshTimer.invalidate();
        GpuTimingOrder.clear();
        Snapshot = FSnapshot{};

        if (!Viewport) {
            emit Updated();
            return;
        }

        connect(Viewport, &ViewportWidget::FrameReady, this, &StatsBridge::Refresh);
        connect(Viewport, &ViewportWidget::RendererInitialized, this, [this]() {
            RefreshTimer.invalidate();
            Refresh();
        });
        connect(Viewport, &QObject::destroyed, this, [this]() {
            Viewport = nullptr;
            Renderer = RendererHandle{};
            Snapshot = FSnapshot{};
            emit Updated();
        });
        Refresh();
    }

    void StatsBridge::Refresh() {
        if (!Viewport || !Renderer) return;
        if (RefreshTimer.isValid() && RefreshTimer.elapsed() < 200) return;

        {
            auto Access = Renderer.Lock();
            if (!Access || !Access->IsInitialized()) return;

            RefreshTimer.restart();
            Capture(*Access);
        }
        emit Updated();
    }

    double StatsBridge::GetFPS() const {
        return Snapshot.FPS;
    }

    double StatsBridge::GetFrameTimeMs() const {
        return Snapshot.FPS > 0.0 ? 1000.0 / Snapshot.FPS : 0.0;
    }

    int StatsBridge::GetVisibleDrawCount() const {
        return Snapshot.VisibleDrawCount;
    }

    int StatsBridge::GetTotalDrawCount() const {
        return Snapshot.TotalDrawCount;
    }

    int StatsBridge::GetOccludedDrawCount() const {
        return Snapshot.OccludedDrawCount;
    }

    QString StatsBridge::GetInternalResolution() const {
        return Snapshot.InternalResolution;
    }

    QString StatsBridge::GetOutputResolution() const {
        return Snapshot.OutputResolution;
    }

    QString StatsBridge::GetGPUName() const {
        return Snapshot.GPUName;
    }

    QString StatsBridge::GetVRAMText() const {
        return Snapshot.VRAMText;
    }

    QString StatsBridge::GetVRAMUsageText() const {
        return Snapshot.VRAMUsageText;
    }

    bool StatsBridge::IsVRAMOverBudget() const {
        return Snapshot.VRAMOverBudget;
    }

    double StatsBridge::GetVRAMBudgetFrac() const {
        return Snapshot.VRAMBudgetFrac;
    }

    namespace {
        QString FormatBytes(Smile::u64 _Bytes) {
            const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
            const double MiB = static_cast<double>(_Bytes) / (1024.0 * 1024.0);
            if (MiB >= 1024.0) return Loc.toString(MiB / 1024.0, 'f', 2) + QStringLiteral(" GB");
            return Loc.toString(MiB, 'f', 1) + QStringLiteral(" MB");
        }
    }

    QString StatsBridge::GetVRAMNonLocalText() const {
        return Snapshot.VRAMNonLocalText;
    }

    QVariantList StatsBridge::GetVRAMBreakdown() const {
        return Snapshot.VRAMBreakdown;
    }

    // Categorias usam o total DXGI; a diferença aparece como memória não rastreada.
    QVariantList StatsBridge::BuildVRAMBreakdown(Smile::Renderer& _Renderer) const {
        QVariantList Rows;
        const auto VM    = _Renderer.GetGpuMemoryInfo();
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

        // A participação dos filhos é relativa à própria categoria.
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
            // Fecha a soma quando a categoria possui alocações sem rótulo individual.
            if (!Children.isEmpty() && Bytes > Labeled) {
                const Smile::u64 Rest = Bytes - Labeled;
                QVariantMap Child;
                Child.insert(QStringLiteral("name"), QStringLiteral("Outros (sem rótulo)"));
                Child.insert(QStringLiteral("text"), FormatBytes(Rest));
                Child.insert(QStringLiteral("shareText"),
                             QString::number(100.0 * static_cast<double>(Rest) /
                                             static_cast<double>(Bytes), 'f', 0) + "%");
                Child.insert(QStringLiteral("active"), false);
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

    QString StatsBridge::GetGpuFrameText() const {
        return Snapshot.GPUFrameText;
    }

    double StatsBridge::GetGpuFrameMs() const {
        return Snapshot.GPUFrameMs;
    }

    QVariantList StatsBridge::GetGpuTimings() const {
        return Snapshot.GpuTimings;
    }

    QVariantList StatsBridge::GetShadowCascades() const {
        return Snapshot.ShadowCascades;
    }

    // Estatísticas de caster pertencem à última atualização da cascata; cascatas congeladas
    // preservam esses valores até a próxima rasterização.
    QVariantList StatsBridge::BuildShadowCascades(Smile::Renderer& _Renderer) const {
        QVariantList Rows;
        const Smile::FSunShadows& Csm = _Renderer.GetSunShadows();
        if (!Csm.IsInitialized()) return Rows;

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        const int Submitted = static_cast<int>(Csm.GetSubmittedCasterCount());
        double MeanPerFrame = 0.0, MeanDynamic = 0.0;

        for (Smile::u32 C = 0; C < Smile::FSunShadows::kNumCascades; ++C) {
            const auto& S = Csm.CascadeStats(C);
            // Popcount da janela de 64 frames sem depender de C++20.
            int Updates = 0;
            for (Smile::u64 B = S.StaticHistory; B; B &= B - 1) ++Updates;
            const double Rate = Updates / 64.0;
            // Estáticos respeitam a taxa do cache; dinâmicos são redesenhados todo frame.
            MeanPerFrame += S.SubmittedStatic * Rate + S.SubmittedDynamic;
            MeanDynamic  += S.SubmittedDynamic;

            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QStringLiteral("Cascata %1").arg(C));
            Row.insert(QStringLiteral("text"),
                       QStringLiteral("%1 · %2")
                           .arg(Loc.toString(static_cast<int>(S.SubmittedStatic)))
                           .arg(Loc.toString(static_cast<int>(S.SubmittedDynamic))));
            Row.insert(QStringLiteral("shareText"),
                       QStringLiteral("%1/64 fr").arg(Updates));
            Row.insert(QStringLiteral("cullText"),
                       QStringLiteral("−%1 tam · −%2 fatia · Δ%3 tx")
                           .arg(S.CulledSize).arg(S.CulledPlanes)
                           .arg(Loc.toString(S.SnapDeltaTexels, 'f', 0)));
            Row.insert(QStringLiteral("frac"),
                       Submitted > 0 ? static_cast<double>(S.Submitted) / Submitted : 0.0);
            Row.insert(QStringLiteral("frozen"), !S.UpdatedThisFrame);
            Rows.push_back(Row);
        }

        // Um objeto pode contribuir com um draw em cada cascata.
        QVariantMap Total;
        Total.insert(QStringLiteral("name"), QStringLiteral("Draws de sombra/frame"));
        Total.insert(QStringLiteral("text"), Loc.toString(MeanPerFrame, 'f', 0));
        Total.insert(QStringLiteral("shareText"),
                     QStringLiteral("de %1").arg(
                         Loc.toString(Submitted * static_cast<int>(
                             Smile::FSunShadows::kNumCascades))));
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

    // Ordena passes por custo com histerese para evitar oscilações visuais entre empates.
    QVariantList StatsBridge::BuildGpuTimings(Smile::Renderer& _Renderer) {
        QVariantList Rows;
        const auto& Results = _Renderer.GetGpuProfiler().Results();
        if (Results.empty()) return Rows;

        double FrameMs = 0.0;
        for (const auto& R : Results)
            if (std::strcmp(R.Name, kGpuFrameScope) == 0) FrameMs = R.Milliseconds;

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
            // Profundidades maiores que um são achatadas no pai visual imediato.
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
            // Passes novos entram no fim do ranking anterior e sobem pela mesma histerese.
            std::stable_sort(Groups.begin(), Groups.end(),
                             [&](const FTimingGroup& A, const FTimingGroup& B) {
                                 const int IA = GpuTimingOrder.indexOf(GroupName(A));
                                 const int IB = GpuTimingOrder.indexOf(GroupName(B));
                                 const int KA = IA >= 0 ? IA : GpuTimingOrder.size();
                                 const int KB = IB >= 0 ? IB : GpuTimingOrder.size();
                                 return KA < KB;
                             });
            // Diferenças menores que 5% ou 0,02 ms preservam a ordem anterior.
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
                // Mantém linhas ausentes para a árvore não oscilar quando o CSM usa cache.
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

    void StatsBridge::Capture(Smile::Renderer& _Renderer) {
        FSnapshot Next;
        Next.FPS = Viewport ? static_cast<double>(Viewport->GetFPS()) : 0.0;
        Next.VisibleDrawCount = static_cast<int>(_Renderer.GetVisibleCount());
        Next.TotalDrawCount = static_cast<int>(_Renderer.GetDrawCount());
        Next.OccludedDrawCount = static_cast<int>(_Renderer.GetOccludedCount());
        Next.InternalResolution = QStringLiteral("%1×%2")
            .arg(_Renderer.RenderWidth()).arg(_Renderer.RenderHeight());
        Next.OutputResolution = QStringLiteral("%1×%2")
            .arg(_Renderer.OutputWidth()).arg(_Renderer.OutputHeight());
        Next.GPUName = QString::fromStdWString(
            _Renderer.GetGpuDescription());

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
        Next.VRAMText = Loc.toString(
            static_cast<double>(_Renderer.GetDedicatedVideoMemory()) / GiB,
            'f', 1) + QStringLiteral(" GB");

        const auto VM = _Renderer.GetGpuMemoryInfo();
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
        Snapshot = std::move(Next);
    }

}
