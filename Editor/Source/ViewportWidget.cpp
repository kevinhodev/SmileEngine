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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace SmileEditor {
    static constexpr float kMouseSensitivity = 0.15f;  

    namespace {
        constexpr const char* kGBufferLabels[] = {
            "",
            "Base Color",
            "World Normal",
            "Roughness",
            "Metallic",
            "Subsurface",
            "Ambient Occlusion",
            "Shading Model",
            "Motion Vectors"
        };
    }

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
        switch (CurrentViewMode) {
        case GBuffer:
            if (CurrentGBufferMode >= 1 && CurrentGBufferMode <= 8)
                return QString::fromLatin1(kGBufferLabels[CurrentGBufferMode]);
            return QStringLiteral("GBuffer");
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

    bool ViewportWidget::IsReSTIRGIVisibilityEnabled() const {
        return Renderer && Renderer->GetReSTIRGI().GetVisibility();
    }

    bool ViewportWidget::AreGIFoliageShadowsEnabled() const {
        return Renderer && Renderer->GetDDGI().GetFoliageShadows();
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

    int ViewportWidget::GetUpscalerMode() const {
        return Renderer ? static_cast<int>(Renderer->GetUpscaler()) : 0;  // 0=None 1=FSR 2=DLSS
    }

    bool ViewportWidget::IsFsrAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->UpscalerAvailable(Smile::EUpscaler::FSR);
    }

    bool ViewportWidget::IsDlssAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->UpscalerAvailable(Smile::EUpscaler::DLSS);
    }

    int ViewportWidget::GetFsrQuality() const {
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

    bool ViewportWidget::IsMergeByMaterialEnabled() const {
        return Renderer && Renderer->GetMergeByMaterial();
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
        const double Pct = VM.LocalBudget > 0
            ? 100.0 * static_cast<double>(VM.LocalUsage) / static_cast<double>(VM.LocalBudget)
            : 0.0;
        return Loc.toString(static_cast<double>(VM.LocalUsage) / GiB, 'f', 2) +
               QStringLiteral(" / ") +
               Loc.toString(static_cast<double>(VM.LocalBudget) / GiB, 'f', 1) +
               QStringLiteral(" GB (") + Loc.toString(Pct, 'f', 0) + QStringLiteral("%)");
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
        if (!Renderer || !Renderer->IsInitialized()) return QStringLiteral("—");
        for (const auto& R : Renderer->GetGpuProfiler().Results())
            if (std::strcmp(R.Name, kGpuFrameScope) == 0)
                return QLocale(QLocale::Portuguese, QLocale::Brazil)
                           .toString(R.Milliseconds, 'f', 2) + QStringLiteral(" ms");
        return QStringLiteral("—");
    }

    // Tabela "GPU por passe": escopos do FGpuProfiler (sem o total, que vira o header),
    // ordenados do mais caro pro mais barato; frac relativo ao frame total de GPU.
    QVariantList ViewportWidget::GetGpuTimings() const {
        QVariantList Rows;
        if (!Renderer || !Renderer->IsInitialized()) return Rows;
        const auto& Results = Renderer->GetGpuProfiler().Results();
        if (Results.empty()) return Rows;

        double FrameMs = 0.0;
        for (const auto& R : Results)
            if (std::strcmp(R.Name, kGpuFrameScope) == 0) FrameMs = R.Milliseconds;

        // Passes da fila de COMPUTE (DDGI async) entram na mesma tabela — o tempo e
        // medido na fila propria e o frac continua relativo ao frame da fila direta
        // (mostra quanto do frame o trabalho sobreposto ocupa).
        const auto ComputeResults = Renderer->GetAsyncComputeTimings();

        std::vector<const Smile::FGpuProfiler::FScopeResult*> Sorted;
        Sorted.reserve(Results.size() + ComputeResults.size());
        for (const auto& R : Results)
            if (std::strcmp(R.Name, kGpuFrameScope) != 0) Sorted.push_back(&R);
        for (const auto& R : ComputeResults) Sorted.push_back(&R);
        std::sort(Sorted.begin(), Sorted.end(),
                  [](const auto* A, const auto* B) { return A->Milliseconds > B->Milliseconds; });

        const QLocale Loc(QLocale::Portuguese, QLocale::Brazil);
        for (const auto* R : Sorted) {
            QVariantMap Row;
            Row.insert(QStringLiteral("name"), QString::fromUtf8(R->Name));
            Row.insert(QStringLiteral("text"), Loc.toString(R->Milliseconds, 'f', 2) +
                                               QStringLiteral(" ms"));
            Row.insert(QStringLiteral("frac"),
                       FrameMs > 0.0 ? std::min(1.0, R->Milliseconds / FrameMs) : 0.0);
            Rows.push_back(Row);
        }
        return Rows;
    }

    void ViewportWidget::SelectLit() {
        if (!Renderer) return;
        Renderer->SetGBufferDebugMode(0);
        Renderer->SetFlickerMode(0);
        CurrentViewMode = Lit;
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SelectGBuffer(int _Mode) {
        if (!Renderer) return;
        const int Mode = qBound(1, _Mode, 8);
        Renderer->SetFlickerMode(0);
        Renderer->SetGBufferDebugMode(static_cast<Smile::u32>(Mode));
        CurrentGBufferMode = Mode;
        CurrentViewMode = GBuffer;
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SelectReflectionHeatmap() {
        if (!Renderer) return;
        Renderer->SetGBufferDebugMode(0);
        // Ainda nao ha um heatmap exclusivo dos raios de reflexao. O heatmap temporal
        // existente e a visualizacao funcional mais proxima para este slot do mockup.
        Renderer->SetFlickerMode(2);
        CurrentViewMode = ReflectionHeatmap;
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

    void ViewportWidget::SetUpscalerMode(int _Mode) {
        if (!Renderer) return;
        Renderer->SetUpscaler(static_cast<Smile::EUpscaler>(_Mode));  // 0=None 1=FSR 2=DLSS; cai p/ None se indisponivel
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetFsrQuality(int _Quality) {
        if (!Renderer) return;
        Renderer->SetUpscalerQuality(_Quality);  // qualidade compartilhada FSR/DLSS
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetRenderScale(double _Scale) {
        if (!Renderer) return;
        Renderer->SetRenderScale(static_cast<float>(_Scale));
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

    void ViewportWidget::SetMergeByMaterialEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetMergeByMaterial(_Enabled);
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
        Renderer->SetMergeByMaterial(false);
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
