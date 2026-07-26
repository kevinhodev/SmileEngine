#pragma once

#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QPoint>
#include <QElapsedTimer>
#include <QString>
#include <QVariantList>
#include <QStringList>
#include <array>
#include <memory>
#include "Smile/Math/Math.h"
#include "SmileEditor/GizmoController.h"

class QTimer;
class QPaintEngine;
class QKeyEvent;
class QMouseEvent;

namespace Smile { class Renderer; }

namespace SmileEditor {
    class ViewportWidget : public QWidget {
        Q_OBJECT
        Q_PROPERTY(int viewMode READ GetViewMode NOTIFY ViewSettingsChanged)
        // Visualizador de render targets: lista publicada por DebugTargets (nomes) + selecao.
        Q_PROPERTY(QStringList debugTargetNames READ GetDebugTargetNames NOTIFY DebugTargetsChanged)
        Q_PROPERTY(int debugTargetIndex READ GetDebugTargetIndex NOTIFY ViewSettingsChanged)
        // Janela de debug: selecao multipla (indices), colunas da grade e exposicao.
        Q_PROPERTY(QVariantList debugSelection READ GetDebugSelection NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int debugColumns READ GetDebugColumns NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double debugExposure READ GetDebugExposure NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int debugPreviewSeq READ GetDebugPreviewSeq NOTIFY DebugPreviewUpdated)
        Q_PROPERTY(bool debugPreviewReady READ IsDebugPreviewReady NOTIFY DebugPreviewUpdated)
        Q_PROPERTY(bool debugProbeInspecting READ IsDebugProbeInspecting NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int debugProbeIndex READ GetDebugProbeIndex NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QString debugProbeCoord READ GetDebugProbeCoord NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QString debugProbeWorld READ GetDebugProbeWorld NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QString debugProbeGrid READ GetDebugProbeGrid NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QString debugProbeDistanceRange READ GetDebugProbeDistanceRange NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QString debugProbeDirection READ GetDebugProbeDirection NOTIFY DebugProbeDirectionChanged)
        Q_PROPERTY(QString debugProbeSample READ GetDebugProbeSample NOTIFY DebugProbeSampleChanged)
        Q_PROPERTY(bool debugProbePointPickArmed READ IsDebugProbePointPickArmed NOTIFY DebugProbePointChanged)
        Q_PROPERTY(QString debugProbePointSummary READ GetDebugProbePointSummary NOTIFY DebugProbePointChanged)
        Q_PROPERTY(QVariantList debugProbeContributors READ GetDebugProbeContributors NOTIFY DebugProbePointChanged)
        Q_PROPERTY(QString viewModeLabel READ GetViewModeLabel NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool ddgiEnabled READ IsDDGIEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool restirGIEnabled READ IsReSTIRGIEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool restirGIVisibilityEnabled READ IsReSTIRGIVisibilityEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool giFoliageShadows READ AreGIFoliageShadowsEnabled NOTIFY ViewSettingsChanged)
        // Back-face culling nos raios de reflexao (politica por passe).
        Q_PROPERTY(bool reflectionsCullBackface READ IsReflectionsCullBackfaceEnabled NOTIFY ViewSettingsChanged)
        // Politica de backface do gather do ReSTIR (retrace + terminacao preta).
        Q_PROPERTY(bool giBackfacePolicy READ IsGIBackfacePolicyEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool gtaoEnabled READ IsGTAOEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool gtaoHalfRes READ IsGTAOHalfRes NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool reflectionsEnabled READ AreReflectionsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool nrdEnabled READ IsNrdEnabled NOTIFY ViewSettingsChanged)
        // Eixo de denoiser {0=Nenhum, 1=NRD, 2=DLSS RR}. rrAvailable gateia a opcao RR na UI.
        Q_PROPERTY(int denoiserMode READ GetDenoiserMode NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool rrAvailable READ IsRRAvailable NOTIFY RendererInitialized)
        Q_PROPERTY(int upscalerMode READ GetUpscalerMode NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool fsrAvailable READ IsFsrAvailable NOTIFY RendererInitialized)
        Q_PROPERTY(bool dlssAvailable READ IsDlssAvailable NOTIFY RendererInitialized)
        // Preset compartilhado FSR/DLSS (nao e so do FSR) — ver Renderer::SetUpscalerQuality.
        Q_PROPERTY(int upscalerQuality READ GetUpscalerQuality NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int recommendedUpscalerMode READ GetRecommendedUpscalerMode NOTIFY RendererInitialized)
        Q_PROPERTY(QString recommendedUpscalerName READ GetRecommendedUpscalerName NOTIFY RendererInitialized)
        Q_PROPERTY(double renderScale READ GetRenderScale NOTIFY ViewSettingsChanged)
        // Knobs de calibracao dos epsilons de raio (pagina "Iluminacao global"). Lista uniforme
        // em vez de 9 propriedades nomeadas: sao todos do mesmo formato (label/valor/faixa/unidade)
        // e a UI e um Repeater — acrescentar ou tirar um knob vira uma linha na tabela do .cpp.
        Q_PROPERTY(QVariantList rayEpsilons READ GetRayEpsilons NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool taaEnabled READ IsTAAEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool frustumCullingEnabled READ IsFrustumCullingEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool occlusionCullingEnabled READ IsOcclusionCullingEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool sunShadowsEnabled READ AreSunShadowsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool shadowCacheEnabled READ IsShadowCacheEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool shadowDebugCascades READ IsShadowDebugCascades NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowMaxDistance READ GetShadowMaxDistance NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowDepthBias READ GetShadowDepthBias NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowMinCasterTexels READ GetShadowMinCasterTexels NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QVariantList shadowCascadeBias READ GetShadowCascadeBias NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowSunAngle READ GetShadowSunAngle NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool sunShaftsEnabled READ AreSunShaftsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double sunShaftsIntensity READ GetSunShaftsIntensity NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double sunShaftsDust READ GetSunShaftsDust NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double sunShaftsPhaseG READ GetSunShaftsPhaseG NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int sunShaftsSteps READ GetSunShaftsSteps NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double sunShaftsRange READ GetSunShaftsRange NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool sunShaftsTemporal READ AreSunShaftsTemporal NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool volFogEnabled READ IsVolFogEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double volFogDistance READ GetVolFogDistance NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double volFogPhaseG READ GetVolFogPhaseG NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double volFogDensity READ GetVolFogDensity NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double volFogAmbient READ GetVolFogAmbient NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool volFogTemporal READ IsVolFogTemporal NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double volFogLights READ GetVolFogLights NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool volFogConsDepth READ IsVolFogConsDepth NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool cloudsEnabled READ AreCloudsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool cloudsHalfRes READ AreCloudsHalfRes NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool cloudsTemporal READ AreCloudsTemporal NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudCoverage READ GetCloudCoverage NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudDensity READ GetCloudDensity NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudWindSpeed READ GetCloudWindSpeed NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudErosion READ GetCloudErosion NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudPhaseG READ GetCloudPhaseG NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudPowder READ GetCloudPowder NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudAmbient READ GetCloudAmbient NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudTypeBias READ GetCloudTypeBias NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudPeakVariation READ GetCloudPeakVariation NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int cloudWeatherSeed READ GetCloudWeatherSeed NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int cloudWeatherCells READ GetCloudWeatherCells NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool cloudWeatherAuthored READ IsCloudWeatherAuthored NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool cloudShadows READ AreCloudShadowsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudShadowStrength READ GetCloudShadowStrength NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudBottomKm READ GetCloudBottomKm NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudThicknessKm READ GetCloudThicknessKm NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int cloudMarchSteps READ GetCloudMarchSteps NOTIFY ViewSettingsChanged)
        // Clima (FWeather do Renderer) — pagina Clima do SettingsWindow.
        Q_PROPERTY(double rainAmount READ GetRainAmount NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double puddleAmount READ GetPuddleAmount NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double puddleScale READ GetPuddleScale NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double rippleStrength READ GetRippleStrength NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double wetDarkening READ GetWetDarkening NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double curtainAmount READ GetCurtainAmount NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool rainOcclusion READ IsRainOcclusion NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool rainParticles READ AreRainParticles NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool weatherDriveSky READ IsWeatherDriveSky NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool depthPrepassEnabled READ IsDepthPrepassEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double fps READ GetFPS NOTIFY FrameReady)
        Q_PROPERTY(double frameTimeMs READ GetFrameTimeMs NOTIFY FrameReady)
        Q_PROPERTY(int visibleDrawCount READ GetVisibleDrawCount NOTIFY FrameReady)
        Q_PROPERTY(int occludedDrawCount READ GetOccludedDrawCount NOTIFY FrameReady)
        Q_PROPERTY(int totalDrawCount READ GetTotalDrawCount NOTIFY FrameReady)
        Q_PROPERTY(QString internalResolution READ GetInternalResolution NOTIFY FrameReady)
        Q_PROPERTY(QString outputResolution READ GetOutputResolution NOTIFY FrameReady)
        Q_PROPERTY(QString gpuName READ GetGPUName NOTIFY RendererInitialized)
        Q_PROPERTY(QString vramText READ GetVRAMText NOTIFY RendererInitialized)
        Q_PROPERTY(QString vramUsageText READ GetVRAMUsageText NOTIFY FrameReady)
        Q_PROPERTY(bool vramOverBudget READ IsVRAMOverBudget NOTIFY FrameReady)
        Q_PROPERTY(double vramBudgetFrac READ GetVRAMBudgetFrac NOTIFY FrameReady)
        Q_PROPERTY(QString vramNonLocalText READ GetVRAMNonLocalText NOTIFY FrameReady)
        Q_PROPERTY(QVariantList vramBreakdown READ GetVRAMBreakdown NOTIFY FrameReady)
        Q_PROPERTY(QString gpuFrameText READ GetGpuFrameText NOTIFY FrameReady)
        Q_PROPERTY(QVariantList gpuTimings READ GetGpuTimings NOTIFY FrameReady)

    public:
        // Valores explicitos preservados (o QML compara viewMode com inteiros fixos; o 1 era o
        // path tracer experimental e o 2 o menu de buffers do G-buffer, ambos removidos — os
        // campos do G-buffer agora saem pelo visualizador de render targets).
        enum ViewMode {
            Lit = 0,
            ReflectionHeatmap = 3
        };
        Q_ENUM(ViewMode)

        explicit ViewportWidget(QWidget* parent = nullptr);
        ~ViewportWidget() override;

        Smile::Renderer* GetRenderer() const { return Renderer.get(); }
        float            GetFPS()      const { return LastFPS; }
        int               GetViewMode() const { return CurrentViewMode; }
        QStringList       GetDebugTargetNames() const;
        int               GetDebugTargetIndex() const;
        QVariantList      GetDebugSelection() const;
        int               GetDebugColumns() const;
        double            GetDebugExposure() const;
        int               GetDebugPreviewSeq() const { return DebugPreviewSeq; }
        bool              IsDebugPreviewReady() const;
        QImage            DebugPreviewImageCopy() const;
        void              SetDebugPreviewEnabled(bool enabled);
        bool              IsDebugProbeInspecting() const { return DebugProbeSessionActive; }
        int               GetDebugProbeIndex() const;
        QString           GetDebugProbeCoord() const;
        QString           GetDebugProbeWorld() const;
        QString           GetDebugProbeGrid() const;
        QString           GetDebugProbeDistanceRange() const;
        QString           GetDebugProbeDirection() const { return DebugProbeDirection; }
        QString           GetDebugProbeSample() const { return DebugProbeSample; }
        bool              IsDebugProbePointPickArmed() const { return DebugProbePointPickArmed; }
        QString           GetDebugProbePointSummary() const { return DebugProbePointSummary; }
        QVariantList      GetDebugProbeContributors() const { return DebugProbeContributors; }
        QString           GetViewModeLabel() const;
        bool              IsDDGIEnabled() const;
        bool              IsReSTIRGIEnabled() const;
        bool              IsReSTIRGIVisibilityEnabled() const;
        bool              AreGIFoliageShadowsEnabled() const;
        bool              IsReflectionsCullBackfaceEnabled() const;
        bool              IsGIBackfacePolicyEnabled() const;
        bool              IsGTAOEnabled() const;
        bool              IsGTAOHalfRes() const;
        bool              AreReflectionsEnabled() const;
        bool              IsNrdEnabled() const;
        int               GetDenoiserMode() const;   // 0=Nenhum 1=NRD 2=DLSS Ray Reconstruction
        bool              IsRRAvailable() const;      // DLSS RR suportado (NVIDIA RTX + SDK)
        int               GetUpscalerMode() const;
        bool              IsFsrAvailable() const;
        bool              IsDlssAvailable() const;
        int               GetUpscalerQuality() const;
        int               GetRecommendedUpscalerMode() const;
        QString           GetRecommendedUpscalerName() const;
        double            GetRenderScale() const;
        QVariantList      GetRayEpsilons() const;
        bool              IsTAAEnabled() const;
        bool              IsFrustumCullingEnabled() const;
        bool              IsOcclusionCullingEnabled() const;
        bool              AreSunShadowsEnabled() const;
        bool              IsShadowCacheEnabled() const;
        bool              IsShadowDebugCascades() const;
        double            GetShadowMaxDistance() const;
        double            GetShadowDepthBias() const;
        double            GetShadowMinCasterTexels() const;
        QVariantList      GetShadowCascadeBias() const;
        double            GetShadowSunAngle() const;
        bool              AreSunShaftsEnabled() const;
        double            GetSunShaftsIntensity() const;
        double            GetSunShaftsDust() const;
        double            GetSunShaftsPhaseG() const;
        int               GetSunShaftsSteps() const;
        double            GetSunShaftsRange() const;
        bool              AreSunShaftsTemporal() const;
        bool              IsVolFogEnabled() const;
        double            GetVolFogDistance() const;
        double            GetVolFogPhaseG() const;
        double            GetVolFogDensity() const;
        double            GetVolFogAmbient() const;
        bool              IsVolFogTemporal() const;
        double            GetVolFogLights() const;
        bool              IsVolFogConsDepth() const;
        bool              AreCloudsEnabled() const;
        bool              AreCloudsHalfRes() const;
        bool              AreCloudsTemporal() const;
        double            GetCloudCoverage() const;
        double            GetCloudDensity() const;
        double            GetCloudWindSpeed() const;
        double            GetCloudErosion() const;
        double            GetCloudPhaseG() const;
        double            GetCloudPowder() const;
        double            GetCloudAmbient() const;
        double            GetCloudTypeBias() const;
        double            GetCloudPeakVariation() const;
        int               GetCloudWeatherSeed() const;
        int               GetCloudWeatherCells() const;
        bool              IsCloudWeatherAuthored() const;
        bool              AreCloudShadowsEnabled() const;
        double            GetCloudShadowStrength() const;
        double            GetCloudBottomKm() const;
        double            GetCloudThicknessKm() const;
        int               GetCloudMarchSteps() const;
        double            GetRainAmount() const;
        double            GetPuddleAmount() const;
        double            GetPuddleScale() const;
        double            GetRippleStrength() const;
        double            GetWetDarkening() const;
        double            GetCurtainAmount() const;
        bool              IsRainOcclusion() const;
        bool              AreRainParticles() const;
        bool              IsWeatherDriveSky() const;
        bool              IsDepthPrepassEnabled() const;
        double            GetFrameTimeMs() const;
        int               GetVisibleDrawCount() const;
        int               GetTotalDrawCount() const;
        int               GetOccludedDrawCount() const;
        QString           GetInternalResolution() const;
        QString           GetOutputResolution() const;
        QString           GetGPUName() const;
        QString           GetVRAMText() const;
        QString           GetVRAMUsageText() const;
        bool              IsVRAMOverBudget() const;
        double            GetVRAMBudgetFrac() const;
        QString           GetVRAMNonLocalText() const;
        QVariantList      GetVRAMBreakdown() const;
        QString           GetGpuFrameText() const;
        QVariantList      GetGpuTimings() const;

        Q_INVOKABLE void SelectLit();
        Q_INVOKABLE void SelectReflectionHeatmap();
        // Visualizador: -1 desliga; senao e o indice em debugTargetNames.
        Q_INVOKABLE void SelectDebugTarget(int index);
        Q_INVOKABLE void ToggleDebugSelection(int index);   // liga/desliga um alvo na grade
        Q_INVOKABLE void ClearDebugSelection();
        Q_INVOKABLE void SetDebugColumns(int columns);      // 0 = grade automatica
        Q_INVOKABLE void SetDebugExposure(double exposure); // multiplicador global
        Q_INVOKABLE void InspectDDGIProbe(int targetIndex, double u, double v,
                                          double tileAspect);
        Q_INVOKABLE void StepDebugProbe(int dx, int dy, int dz);
        Q_INVOKABLE void ClearDebugProbeInspection();
        Q_INVOKABLE void UpdateDebugProbeDirection(double u, double v);
        Q_INVOKABLE void ArmDebugProbePointPick();
        Q_INVOKABLE void ClearDebugProbePoint();
        Q_INVOKABLE void SelectDebugProbeContributor(int probeIndex);

        // DDGI e ReSTIR GI so ganham SRV ao carregar cena (SetupForScene guarda em
        // DDGI.IsReady()), entao a lista de alvos muda DEPOIS do load. Sem este aviso a QML
        // segue exibindo a lista do boot, sem eles. Chamado pelo MainWindow apos LoadCookedScene.
        void NotifyDebugTargetsChanged() {
            if (DebugProbeSessionActive) ClearDebugProbeInspection();
            emit DebugTargetsChanged();
            // RegisterDebugTargets remapeia selecoes por nome quando a disponibilidade de
            // DDGI/ReSTIR muda; a QML precisa reler tambem os indices selecionados.
            emit ViewSettingsChanged();
        }
        Q_INVOKABLE void ToggleDDGI();
        Q_INVOKABLE void ToggleReSTIRGI();
        Q_INVOKABLE void ToggleReSTIRGIVisibility();
        Q_INVOKABLE void ToggleGIFoliageShadows();
        Q_INVOKABLE void ToggleReflectionsCullBackface();
        Q_INVOKABLE void ToggleGIBackfacePolicy();
        Q_INVOKABLE void ToggleGTAO();
        Q_INVOKABLE void ToggleGTAOHalfRes();
        Q_INVOKABLE void ToggleReflections();
        Q_INVOKABLE void ToggleNrd();
        Q_INVOKABLE void SetDenoiserMode(int mode);   // 0=Nenhum 1=NRD 2=DLSS RR (RR acopla upscaler=DLSS)
        Q_INVOKABLE void SetUpscalerMode(int mode);
        Q_INVOKABLE void SetUpscalerQuality(int quality);
        Q_INVOKABLE void SetRenderScale(double scale);
        // Valor em UNIDADE DE UI (mm p/ os offsets, frames p/ a idade) — a conversao p/ metros
        // fica na tabela. Cada mudanca invalida reservoirs + historico do denoiser.
        Q_INVOKABLE void SetRayEpsilon(const QString& key, double uiValue);
        Q_INVOKABLE void ResetRayEpsilons();
        Q_INVOKABLE void SetTAAEnabled(bool enabled);
        Q_INVOKABLE void SetFrustumCullingEnabled(bool enabled);
        Q_INVOKABLE void SetOcclusionCullingEnabled(bool enabled);
        Q_INVOKABLE void SetSunShadowsEnabled(bool enabled);
        Q_INVOKABLE void SetShadowCacheEnabled(bool enabled);
        Q_INVOKABLE void SetShadowDebugCascades(bool enabled);
        Q_INVOKABLE void SetShadowMaxDistance(double distance);
        Q_INVOKABLE void SetShadowDepthBias(double bias);
        Q_INVOKABLE void SetShadowMinCasterTexels(double texels);
        Q_INVOKABLE void SetShadowCascadeBiasScale(int cascade, double scale);
        Q_INVOKABLE void SetShadowSunAngle(double degrees);
        Q_INVOKABLE void SetSunShaftsEnabled(bool enabled);
        Q_INVOKABLE void SetSunShaftsIntensity(double value);
        Q_INVOKABLE void SetSunShaftsDust(double value);
        Q_INVOKABLE void SetSunShaftsPhaseG(double value);
        Q_INVOKABLE void SetSunShaftsSteps(int value);
        Q_INVOKABLE void SetSunShaftsRange(double value);
        Q_INVOKABLE void SetSunShaftsTemporal(bool enabled);
        Q_INVOKABLE void SetVolFogEnabled(bool enabled);
        Q_INVOKABLE void SetVolFogDistance(double value);
        Q_INVOKABLE void SetVolFogPhaseG(double value);
        Q_INVOKABLE void SetVolFogDensity(double value);
        Q_INVOKABLE void SetVolFogAmbient(double value);
        Q_INVOKABLE void SetVolFogTemporal(bool enabled);
        Q_INVOKABLE void SetVolFogLights(double value);
        Q_INVOKABLE void SetVolFogConsDepth(bool enabled);
        Q_INVOKABLE void SetCloudsEnabled(bool enabled);
        Q_INVOKABLE void SetCloudsHalfRes(bool halfRes);
        Q_INVOKABLE void SetCloudsTemporal(bool enabled);
        Q_INVOKABLE void SetCloudCoverage(double value);
        Q_INVOKABLE void SetCloudDensity(double value);
        Q_INVOKABLE void SetCloudWindSpeed(double value);
        Q_INVOKABLE void SetCloudErosion(double value);
        Q_INVOKABLE void SetCloudPhaseG(double value);
        Q_INVOKABLE void SetCloudPowder(double value);
        Q_INVOKABLE void SetCloudAmbient(double value);
        Q_INVOKABLE void SetCloudTypeBias(double value);
        Q_INVOKABLE void SetCloudPeakVariation(double value);
        Q_INVOKABLE void SetCloudWeatherSeed(int seed);
        Q_INVOKABLE void SetCloudWeatherCells(int mult);
        Q_INVOKABLE void LoadCloudWeatherTexture();  // abre QFileDialog
        Q_INVOKABLE void ClearCloudWeatherTexture();
        Q_INVOKABLE void SetCloudShadowsEnabled(bool enabled);
        Q_INVOKABLE void SetCloudShadowStrength(double value);
        Q_INVOKABLE void SetCloudAltitude(double bottomKm, double thicknessKm);
        Q_INVOKABLE void SetCloudMarchSteps(int steps);
        Q_INVOKABLE void SetRainAmount(double value);
        Q_INVOKABLE void SetPuddleAmount(double value);
        Q_INVOKABLE void SetPuddleScale(double value);
        Q_INVOKABLE void SetRippleStrength(double value);
        Q_INVOKABLE void SetWetDarkening(double value);
        Q_INVOKABLE void SetCurtainAmount(double value);
        Q_INVOKABLE void SetRainOcclusion(bool enabled);
        Q_INVOKABLE void SetRainParticles(bool enabled);
        Q_INVOKABLE void SetWeatherDriveSky(bool enabled);
        Q_INVOKABLE void SetDepthPrepassEnabled(bool enabled);
        Q_INVOKABLE void ResetRenderSettings();
        Q_INVOKABLE void RequestSettings();

        QPaintEngine* paintEngine() const override;

    protected:
        void showEvent(QShowEvent* event)   override;
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void hideEvent(QHideEvent* event)   override;

        void keyPressEvent(QKeyEvent* event)     override;
        void keyReleaseEvent(QKeyEvent* event)   override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event)  override;

    signals:
        void FrameReady();
        void RendererInitialized(); // emitted once when D3D12 renderer is ready
        void ObjectSelected(int sceneIndex); // clique no viewport: indice na cena (-1 = vazio)
        void ViewSettingsChanged();
        void SettingsRequested();
        // A lista de alvos so muda quando o Renderer recria os targets (boot/resize/troca de
        // cena) — separada de ViewSettingsChanged p/ a QML nao reconstruir o combo a cada frame.
        void DebugTargetsChanged();
        void DebugPreviewUpdated();
        void DebugProbeDirectionChanged();
        void DebugProbeSampleChanged();
        void DebugProbePointChanged();

    private slots:
        void OnRenderTimer();

    private:
        void EnsureRendererIsInitialized();
        void InvalidateDebugPreview();
        void ResetDebugProbePoint(bool CancelRendererRequest = true);
        bool GetDebugProbeCoordValues(int& X, int& Y, int& Z,
                                      int& CountX, int& CountY, int& CountZ) const;

        // Teste 2D em tela do clique contra os markers das luzes (nao estao no ID-buffer do
        // picking GPU). Retorna o indice em Scene.Lights() ou -1; empate = o mais proximo.
        int PickLightMarker(unsigned int X, unsigned int Y) const;

        bool IsHeld(int key) const { return HeldKeys.contains(key); }

        std::unique_ptr<Smile::Renderer> Renderer;
        GizmoController GizmoCtrl; // logica do gizmo de translacao (editor-side)
        QTimer*       RedrawTimer       = nullptr;
        bool          Initialized       = false;

        QSet<int>     HeldKeys;
        Smile::Vec2   MouseDelta       = Smile::Vec2::Zero();
        bool          MouseLookActive  = false;
        bool          IgnoreNextMove   = false;
        float         LastFPS          = 0.0f;
        QElapsedTimer FrameTimer;
        int           CurrentViewMode  = Lit;
        QImage         DebugPreviewImage;
        mutable QMutex DebugPreviewMutex;
        int            DebugPreviewSeq = 0;
        bool           DebugProbeSessionActive = false;
        QStringList    DebugProbePreviousTargets;
        int            DebugProbePreviousColumns = 0;
        QString        DebugProbeDirection;
        QString        DebugProbeSample;
        bool           DebugProbePointPickArmed = false;
        QString        DebugProbePointSummary;
        QVariantList   DebugProbeContributors;
        std::array<Smile::u32, 8> DebugProbeContributorIndices{};
        std::array<float, 8>      DebugProbeContributorWeights{};
        Smile::u32     DebugProbeContributorCount = 0;
        int            DebugProbeContributorRiskSlot = -1;
    };

    // O numero de sequencia no id serve apenas para furar o cache do QML. O provider
    // devolve uma copia protegida porque requestImage pode rodar na render thread do Quick.
    class DebugTargetPreviewImageProvider : public QQuickImageProvider {
    public:
        explicit DebugTargetPreviewImageProvider(ViewportWidget* Viewport)
            : QQuickImageProvider(QQuickImageProvider::Image), Viewport(Viewport) {}

        QImage requestImage(const QString&, QSize* Size, const QSize&) override {
            QImage Image = Viewport ? Viewport->DebugPreviewImageCopy() : QImage();
            if (Size) *Size = Image.size();
            return Image;
        }

    private:
        ViewportWidget* Viewport = nullptr;
    };
} 
