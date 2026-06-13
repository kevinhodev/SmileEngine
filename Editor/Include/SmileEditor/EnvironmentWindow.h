#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <functional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;
class QWidget;

namespace Smile { class Renderer; }

namespace SmileEditor {
    class EnvironmentWindow : public QDialog {
        Q_OBJECT

    public:
        explicit EnvironmentWindow(QWidget* Parent = nullptr);
        ~EnvironmentWindow() override = default;

        void InitializeWithRenderer(Smile::Renderer* Renderer);
        void SetCurrentHDRPath(const QString& Path);

    signals:
        void HDRChanged(const QString& Path);

    protected:
        bool eventFilter(QObject* Object, QEvent* Event) override;

    private:
        QWidget* BuildTitleBar();
        QWidget* BuildSidebar();
        QWidget* BuildContent();
        QFrame* BuildSection(const QString& Icon, const QString& Title, QWidget* Body, QWidget** SectionOut = nullptr);
        QWidget* BuildSunSection();
        QWidget* BuildShadowSection();
        QWidget* BuildAtmosphereSection();
        QWidget* BuildCloudSection();
        QWidget* BuildFogSection();
        QWidget* BuildOceanSection();
        QWidget* BuildGISection();
        QWidget* BuildEnvironmentSection();
        QWidget* BuildFooter();

        QPushButton* AddSidebarButton(const QString& Icon, const QString& Text, QWidget* Target);
        QWidget* MakeToggleRow(const QString& Label, QCheckBox** OutCheck, bool Checked,
                               const std::function<void(bool)>& OnChanged);
        QWidget* MakeNumericRow(const QString& Label, double Min, double Max, double Step,
                                int Decimals, double Value, const QString& Suffix,
                                QDoubleSpinBox** OutSpin,
                                const std::function<void(double)>& OnChanged);
        void SetDefaults();
        void SyncFromRenderer();
        void ApplyCloudAltitude();
        void BrowseHDR();
        void ClearHDRLabel();

        Smile::Renderer* RendererPtr = nullptr;
        QButtonGroup* SidebarGroup = nullptr;
        QScrollArea* ContentScroll = nullptr;
        QWidget* ContentWidget = nullptr;
        QWidget* TitleBar = nullptr;
        QPoint DragStartPosition;
        bool Dragging = false;

        QLabel* CurrentPathLabel = nullptr;
        QWidget* SunSection = nullptr;
        QWidget* ShadowSection = nullptr;
        QWidget* AtmosphereSection = nullptr;
        QWidget* CloudSection = nullptr;
        QWidget* FogSection = nullptr;
        QWidget* OceanSection = nullptr;
        QWidget* GISection = nullptr;
        QWidget* EnvironmentSection = nullptr;

        QDoubleSpinBox* AzimuthSpin = nullptr;
        QDoubleSpinBox* ElevationSpin = nullptr;
        QDoubleSpinBox* SunIntensitySpin = nullptr;
        // Time-of-Day (F1): relogio dirige a posicao do sol.
        QCheckBox*      TODCheck = nullptr;
        QCheckBox*      TODPauseCheck = nullptr;
        QDoubleSpinBox* TODHourSpin = nullptr;
        QDoubleSpinBox* TODDayLengthSpin = nullptr;
        QDoubleSpinBox* TODLatitudeSpin = nullptr;
        QDoubleSpinBox* TODDayOfYearSpin = nullptr;
        QDoubleSpinBox* TODNorthSpin = nullptr;
        // Noite / lua / estrelas (F2).
        QCheckBox*      MoonCheck = nullptr;
        QDoubleSpinBox* MoonIntensitySpin = nullptr;
        QDoubleSpinBox* StarIntensitySpin = nullptr;
        QDoubleSpinBox* MoonPhaseSpin = nullptr;
        QDoubleSpinBox* MoonSizeSpin = nullptr;
        QCheckBox* AtmosphereCheck = nullptr;
        QDoubleSpinBox* SunDiskSpin = nullptr;
        QDoubleSpinBox* GlareSpin = nullptr;
        QCheckBox* AtmosphereAmbientCheck = nullptr;
        QDoubleSpinBox* AtmosphereAmbientSpin = nullptr;

        // CSM (sombra do sol)
        QCheckBox*      ShadowsCheck = nullptr;
        QDoubleSpinBox* ShadowDistanceSpin = nullptr;
        QDoubleSpinBox* ShadowPenumbraSpin = nullptr;
        QDoubleSpinBox* ShadowNormalOffsetSpin = nullptr;
        QDoubleSpinBox* ShadowBiasSpin = nullptr;
        QDoubleSpinBox* ShadowBlendSpin = nullptr;
        QCheckBox*      ShadowDebugCheck = nullptr;

        // GI (DDGI — iluminacao global difusa)
        QCheckBox*      GICheck = nullptr;
        QDoubleSpinBox* GIIntensitySpin = nullptr;
        QCheckBox*      GIChebyshevCheck = nullptr;
        QCheckBox*      GISkipInactiveCheck = nullptr;
        QDoubleSpinBox* GIDeactivThreshSpin = nullptr;
        QCheckBox*      GIFallbackCheck = nullptr;
        QCheckBox*      GIAdaptiveRaysCheck = nullptr;
        QDoubleSpinBox* GIMaxRaysSpin = nullptr;
        QDoubleSpinBox* GIMinRaysSpin = nullptr;
        QCheckBox*      GIRealHitCheck = nullptr;
        QCheckBox*      GIRelocationCheck = nullptr;
        QDoubleSpinBox* GIHysteresisSpin = nullptr;
        QCheckBox*      GIDebugCheck = nullptr;
        // Debug DDGI (passe separado: esferas de probe + wireframe do volume)
        QCheckBox*      GIDebugProbesCheck = nullptr;
        QComboBox*      GIDebugModeCombo = nullptr;
        QDoubleSpinBox* GIDebugProbeSizeSpin = nullptr;
        QCheckBox*      GIDebugVolumeCheck = nullptr;
        QCheckBox*      GIDebugRaysCheck = nullptr;
        QDoubleSpinBox* GIDebugRayRadiusSpin = nullptr;

        // Specular GI (reflexoes ray-traced, estilo Lumen)
        QCheckBox*      ReflectionsCheck = nullptr;
        QDoubleSpinBox* ReflectionMaxRoughSpin = nullptr;
        QCheckBox*      ReflectionTemporalCheck = nullptr;

        // AO (GTAO — oclusao de contato em screen-space)
        QCheckBox*      AOCheck = nullptr;
        QDoubleSpinBox* AORadiusSpin = nullptr;
        QDoubleSpinBox* AOIntensitySpin = nullptr;
        QDoubleSpinBox* AOPowerSpin = nullptr;
        QDoubleSpinBox* AOFadeStartSpin = nullptr;
        QDoubleSpinBox* AOFadeEndSpin = nullptr;
        QCheckBox*      AODebugCheck = nullptr;

        QCheckBox* CloudsCheck = nullptr;
        QDoubleSpinBox* CoverageSpin = nullptr;
        QDoubleSpinBox* DensitySpin = nullptr;
        QDoubleSpinBox* CloudAltitudeSpin = nullptr;
        QDoubleSpinBox* CloudThicknessSpin = nullptr;
        QDoubleSpinBox* CloudWindSpin = nullptr;
        QDoubleSpinBox* PhaseSpin = nullptr;
        QDoubleSpinBox* PowderSpin = nullptr;
        QDoubleSpinBox* ErosionSpin = nullptr;

        QCheckBox* OceanCheck = nullptr;
        QComboBox* WaterDebugCombo = nullptr;
        QCheckBox* QuadtreeCullCheck = nullptr;
        QDoubleSpinBox* QuadtreeBaseSpin = nullptr;
        QDoubleSpinBox* QuadtreeDepthSpin = nullptr;
        QDoubleSpinBox* QuadtreeRingSpin = nullptr;
        QCheckBox* FoamCheck = nullptr;
        QDoubleSpinBox* FoamCoverageSpin = nullptr;
        QDoubleSpinBox* FoamIntensitySpin = nullptr;
        QDoubleSpinBox* OceanWindSpin = nullptr;
        QDoubleSpinBox* WavesAmountSpin = nullptr;
        QDoubleSpinBox* WaveHeightSpin = nullptr;
        QDoubleSpinBox* ChoppySpin = nullptr;
        QDoubleSpinBox* ReflectionScaleSpin = nullptr;
        QDoubleSpinBox* ReflectionBumpSpin = nullptr;
        QDoubleSpinBox* RefractionBumpSpin = nullptr;
        QDoubleSpinBox* BumpStrengthSpin = nullptr;
        QDoubleSpinBox* FogDensitySpin = nullptr;
        QDoubleSpinBox* InScatterDensitySpin = nullptr;
        QDoubleSpinBox* SSSStrengthSpin = nullptr;
        QDoubleSpinBox* ShoreFoamSpin = nullptr;
        QDoubleSpinBox* WaterClaritySpin = nullptr;

        QCheckBox*      HeightFogCheck = nullptr;
        QCheckBox*      AerialFogCheck = nullptr;
        QDoubleSpinBox* AtmFogDensitySpin = nullptr;
        QDoubleSpinBox* AtmFogFalloffSpin = nullptr;
        QDoubleSpinBox* AtmFogHeightSpin  = nullptr;
        QDoubleSpinBox* AtmFogOpacitySpin = nullptr;
        QDoubleSpinBox* AtmFogStartSpin   = nullptr;

        QDoubleSpinBox* IBLIntensitySpin = nullptr;
        QDoubleSpinBox* IBLRotationSpin = nullptr;
        QCheckBox* ShowSkyboxCheck = nullptr;
        QDoubleSpinBox* BloomIntensitySpin = nullptr;
        QDoubleSpinBox* ExposureSpin = nullptr;
        QCheckBox*      TAACheck = nullptr;
        QDoubleSpinBox* TAABlendSpin = nullptr;
        QDoubleSpinBox* TAAVarianceSpin = nullptr;
        QDoubleSpinBox* TAASharpnessSpin = nullptr;
        QDoubleSpinBox* TAAMotionSpin = nullptr;
        QDoubleSpinBox* TAAAntiFlickerSpin = nullptr;
        QDoubleSpinBox* TAADebugSpin = nullptr;
        QDoubleSpinBox* FlickerModeSpin = nullptr;
        QDoubleSpinBox* FlickerScaleSpin = nullptr;
        QDoubleSpinBox* FlickerAlphaSpin = nullptr;
    };
}
