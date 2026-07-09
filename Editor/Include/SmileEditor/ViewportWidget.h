#pragma once

#include <QWidget>
#include <QSet>
#include <QPoint>
#include <QElapsedTimer>
#include <QString>
#include <QVariantList>
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
        Q_PROPERTY(int gBufferMode READ GetGBufferMode NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QString viewModeLabel READ GetViewModeLabel NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool ddgiEnabled READ IsDDGIEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool restirGIEnabled READ IsReSTIRGIEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool restirGIVisibilityEnabled READ IsReSTIRGIVisibilityEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool gtaoEnabled READ IsGTAOEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool gtaoHalfRes READ IsGTAOHalfRes NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool reflectionsEnabled READ AreReflectionsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool nrdEnabled READ IsNrdEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool fsr2Enabled READ IsFsr2Enabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool fsr2Available READ IsFsr2Available NOTIFY RendererInitialized)
        Q_PROPERTY(int fsr2Quality READ GetFsr2Quality NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double renderScale READ GetRenderScale NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool taaEnabled READ IsTAAEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool frustumCullingEnabled READ IsFrustumCullingEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool sunShadowsEnabled READ AreSunShadowsEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool shadowCacheEnabled READ IsShadowCacheEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool shadowDebugCascades READ IsShadowDebugCascades NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowMaxDistance READ GetShadowMaxDistance NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowDepthBias READ GetShadowDepthBias NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowMinCasterTexels READ GetShadowMinCasterTexels NOTIFY ViewSettingsChanged)
        Q_PROPERTY(QVariantList shadowCascadeBias READ GetShadowCascadeBias NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double shadowSunAngle READ GetShadowSunAngle NOTIFY ViewSettingsChanged)
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
        Q_PROPERTY(double cloudBottomKm READ GetCloudBottomKm NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double cloudThicknessKm READ GetCloudThicknessKm NOTIFY ViewSettingsChanged)
        Q_PROPERTY(int cloudMarchSteps READ GetCloudMarchSteps NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool depthPrepassEnabled READ IsDepthPrepassEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(bool mergeByMaterialEnabled READ IsMergeByMaterialEnabled NOTIFY ViewSettingsChanged)
        Q_PROPERTY(double fps READ GetFPS NOTIFY FrameReady)
        Q_PROPERTY(double frameTimeMs READ GetFrameTimeMs NOTIFY FrameReady)
        Q_PROPERTY(int visibleDrawCount READ GetVisibleDrawCount NOTIFY FrameReady)
        Q_PROPERTY(int totalDrawCount READ GetTotalDrawCount NOTIFY FrameReady)
        Q_PROPERTY(QString internalResolution READ GetInternalResolution NOTIFY FrameReady)
        Q_PROPERTY(QString outputResolution READ GetOutputResolution NOTIFY FrameReady)
        Q_PROPERTY(QString gpuName READ GetGPUName NOTIFY RendererInitialized)
        Q_PROPERTY(QString vramText READ GetVRAMText NOTIFY RendererInitialized)

    public:
        // Valores explicitos preservados (o QML compara viewMode com inteiros fixos; o 1 era o
        // path tracer experimental, removido).
        enum ViewMode {
            Lit = 0,
            GBuffer = 2,
            ReflectionHeatmap = 3
        };
        Q_ENUM(ViewMode)

        explicit ViewportWidget(QWidget* parent = nullptr);
        ~ViewportWidget() override;

        Smile::Renderer* GetRenderer() const { return Renderer.get(); }
        float            GetFPS()      const { return LastFPS; }
        int               GetViewMode() const { return CurrentViewMode; }
        int               GetGBufferMode() const { return CurrentGBufferMode; }
        QString           GetViewModeLabel() const;
        bool              IsDDGIEnabled() const;
        bool              IsReSTIRGIEnabled() const;
        bool              IsReSTIRGIVisibilityEnabled() const;
        bool              IsGTAOEnabled() const;
        bool              IsGTAOHalfRes() const;
        bool              AreReflectionsEnabled() const;
        bool              IsNrdEnabled() const;
        bool              IsFsr2Enabled() const;
        bool              IsFsr2Available() const;
        int               GetFsr2Quality() const;
        double            GetRenderScale() const;
        bool              IsTAAEnabled() const;
        bool              IsFrustumCullingEnabled() const;
        bool              AreSunShadowsEnabled() const;
        bool              IsShadowCacheEnabled() const;
        bool              IsShadowDebugCascades() const;
        double            GetShadowMaxDistance() const;
        double            GetShadowDepthBias() const;
        double            GetShadowMinCasterTexels() const;
        QVariantList      GetShadowCascadeBias() const;
        double            GetShadowSunAngle() const;
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
        double            GetCloudBottomKm() const;
        double            GetCloudThicknessKm() const;
        int               GetCloudMarchSteps() const;
        bool              IsDepthPrepassEnabled() const;
        bool              IsMergeByMaterialEnabled() const;
        double            GetFrameTimeMs() const;
        int               GetVisibleDrawCount() const;
        int               GetTotalDrawCount() const;
        QString           GetInternalResolution() const;
        QString           GetOutputResolution() const;
        QString           GetGPUName() const;
        QString           GetVRAMText() const;

        Q_INVOKABLE void SelectLit();
        Q_INVOKABLE void SelectGBuffer(int mode);
        Q_INVOKABLE void SelectReflectionHeatmap();
        Q_INVOKABLE void ToggleDDGI();
        Q_INVOKABLE void ToggleReSTIRGI();
        Q_INVOKABLE void ToggleReSTIRGIVisibility();
        Q_INVOKABLE void ToggleGTAO();
        Q_INVOKABLE void ToggleGTAOHalfRes();
        Q_INVOKABLE void ToggleReflections();
        Q_INVOKABLE void ToggleNrd();
        Q_INVOKABLE void SetFsr2Enabled(bool enabled);
        Q_INVOKABLE void SetFsr2Quality(int quality);
        Q_INVOKABLE void SetRenderScale(double scale);
        Q_INVOKABLE void SetTAAEnabled(bool enabled);
        Q_INVOKABLE void SetFrustumCullingEnabled(bool enabled);
        Q_INVOKABLE void SetSunShadowsEnabled(bool enabled);
        Q_INVOKABLE void SetShadowCacheEnabled(bool enabled);
        Q_INVOKABLE void SetShadowDebugCascades(bool enabled);
        Q_INVOKABLE void SetShadowMaxDistance(double distance);
        Q_INVOKABLE void SetShadowDepthBias(double bias);
        Q_INVOKABLE void SetShadowMinCasterTexels(double texels);
        Q_INVOKABLE void SetShadowCascadeBiasScale(int cascade, double scale);
        Q_INVOKABLE void SetShadowSunAngle(double degrees);
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
        Q_INVOKABLE void SetCloudAltitude(double bottomKm, double thicknessKm);
        Q_INVOKABLE void SetCloudMarchSteps(int steps);
        Q_INVOKABLE void SetDepthPrepassEnabled(bool enabled);
        Q_INVOKABLE void SetMergeByMaterialEnabled(bool enabled);
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

    private slots:
        void OnRenderTimer();

    private:
        void EnsureRendererIsInitialized();

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
        int           CurrentGBufferMode = 1;
    };
} 
