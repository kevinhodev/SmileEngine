#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Editor dock for selecting the HDR environment texture and tweaking IBL
    // parameters (intensity, Y rotation, sky visibility) at runtime.
    class EnvironmentPanel : public QWidget {
        Q_OBJECT

    public:
        explicit EnvironmentPanel(QWidget* Parent = nullptr);
        ~EnvironmentPanel() override = default;

        void InitializeWithRenderer(Smile::Renderer* Renderer);

    private slots:
        void OnBrowseHDR();
        void OnClearHDR();
        void OnIntensityChanged(double Value);
        void OnRotationChanged(double Degrees);
        void OnShowSkyboxToggled(bool Checked);
        void OnWaterDebugModeChanged(int Index);
        void OnFoamToggled(bool Checked);
        void OnFoamCoverageChanged(double Value);
        void OnFoamIntensityChanged(double Value);
        void OnWindSpeedChanged(double Value);
        void OnWavesAmountChanged(double Value);
        void OnWaveHeightChanged(double Value);
        void OnChoppyChanged(double Value);
        void OnReflectionScaleChanged(double Value);
        void OnReflectionBumpChanged(double Value);
        void OnRefractionBumpChanged(double Value);
        void OnBumpStrengthChanged(double Value);
        void OnFogDensityChanged(double Value);
        void OnInScatterDensityChanged(double Value);

    private:
        void BuildUI();

        Smile::Renderer* RendererPtr = nullptr;

        QLabel*         CurrentPathLabel = nullptr;
        QPushButton*    BrowseBtn        = nullptr;
        QPushButton*    ClearBtn         = nullptr;
        QDoubleSpinBox* IntensitySpin    = nullptr;
        QDoubleSpinBox* RotationSpin     = nullptr; // degrees
        QCheckBox*      ShowSkyboxCheck  = nullptr;
        QComboBox*      WaterDebugCombo  = nullptr;
        QCheckBox*      FoamCheck        = nullptr;
        QDoubleSpinBox* FoamCoverageSpin = nullptr;
        QDoubleSpinBox* FoamIntensitySpin = nullptr;
        QDoubleSpinBox* WindSpeedSpin    = nullptr;
        QDoubleSpinBox* WavesAmountSpin  = nullptr;
        QDoubleSpinBox* WaveHeightSpin   = nullptr;
        QDoubleSpinBox* ChoppySpin       = nullptr;
        QDoubleSpinBox* ReflectionScaleSpin = nullptr;
        QDoubleSpinBox* ReflectionBumpSpin = nullptr;
        QDoubleSpinBox* RefractionBumpSpin = nullptr;
        QDoubleSpinBox* BumpStrengthSpin   = nullptr;
        QDoubleSpinBox* FogDensitySpin      = nullptr;
        QDoubleSpinBox* InScatterDensitySpin = nullptr;
    };
}
