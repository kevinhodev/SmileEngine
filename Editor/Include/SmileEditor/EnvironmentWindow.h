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
        QWidget* BuildAtmosphereSection();
        QWidget* BuildCloudSection();
        QWidget* BuildOceanSection();
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
        QWidget* AtmosphereSection = nullptr;
        QWidget* CloudSection = nullptr;
        QWidget* OceanSection = nullptr;
        QWidget* EnvironmentSection = nullptr;

        QDoubleSpinBox* AzimuthSpin = nullptr;
        QDoubleSpinBox* ElevationSpin = nullptr;
        QDoubleSpinBox* SunIntensitySpin = nullptr;
        QCheckBox* AtmosphereCheck = nullptr;
        QDoubleSpinBox* SunDiskSpin = nullptr;
        QDoubleSpinBox* GlareSpin = nullptr;
        QCheckBox* AtmosphereAmbientCheck = nullptr;
        QDoubleSpinBox* AtmosphereAmbientSpin = nullptr;

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

        QDoubleSpinBox* IBLIntensitySpin = nullptr;
        QDoubleSpinBox* IBLRotationSpin = nullptr;
        QCheckBox* ShowSkyboxCheck = nullptr;
    };
}
