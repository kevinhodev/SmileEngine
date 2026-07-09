#include "SmileEditor/ViewportWidget.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Core/Logger.h"
#include <QShowEvent>
#include <QResizeEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QCursor>
#include <QLocale>
#include <QFileDialog>

namespace SmileEditor {
    static constexpr float kMouseSensitivity = 0.15f;  

    namespace {
        constexpr const char* kGBufferLabels[] = {
            "",
            "Base Color",
            "World Normal",
            "Roughness",
            "Metallic",
            "Emissive",
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

        FrameTimer.start();
    }

    ViewportWidget::~ViewportWidget() {
        if (RedrawTimer) RedrawTimer->stop();
        if (Renderer)    Renderer->Shutdown();
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

    bool ViewportWidget::IsFsr2Enabled() const {
        return Renderer && Renderer->Fsr2Available() && Renderer->GetUseFsr2();
    }

    bool ViewportWidget::IsFsr2Available() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Fsr2Available();
    }

    int ViewportWidget::GetFsr2Quality() const {
        return Renderer ? Renderer->GetFsr2Quality() : 0;
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

    void ViewportWidget::SetFsr2Enabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->SetUseFsr2(_Enabled && Renderer->Fsr2Available());
        emit ViewSettingsChanged();
    }

    void ViewportWidget::SetFsr2Quality(int _Quality) {
        if (!Renderer) return;
        Renderer->SetFsr2Quality(_Quality);
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
        Renderer->SetFsr2Quality(1);
        Renderer->SetUseFsr2(Renderer->Fsr2Available());
        if (!Renderer->Fsr2Available()) Renderer->SetRenderScale(1.0f);
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
                // 2) Senao, picking normal (seleciona o objeto sob o cursor).
                if (!GizmoCtrl.OnMousePress(*Renderer, Px, Py))
                    Renderer->RequestPick(Px, Py);
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
