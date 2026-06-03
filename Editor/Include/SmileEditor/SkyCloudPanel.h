#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QCheckBox;

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Editor dock for the physical sky + volumetric clouds: sun placement (azimuth/
    // elevation → sunset), atmosphere sun disk/glare, and the cloud parameters
    // (coverage, density, altitude, wind, phase, powder, erosion). All live.
    class SkyCloudPanel : public QWidget {
        Q_OBJECT

    public:
        explicit SkyCloudPanel(QWidget* Parent = nullptr);
        ~SkyCloudPanel() override = default;

        void InitializeWithRenderer(Smile::Renderer* Renderer);

    private slots:
        void OnSunChanged();           // azimuth + elevation
        void OnSunIntensityChanged(double Value);
        void OnAtmosphereToggled(bool Checked);
        void OnSunDiskChanged(double Value);
        void OnGlareChanged(double Value);
        void OnAtmoAmbientToggled(bool Checked);
        void OnAtmoAmbientChanged(double Value);

        void OnCloudsToggled(bool Checked);
        void OnCoverageChanged(double Value);
        void OnDensityChanged(double Value);
        void OnCloudAltitudeChanged();  // bottom + thickness
        void OnWindChanged(double Value);
        void OnPhaseChanged(double Value);
        void OnPowderChanged(double Value);
        void OnErosionChanged(double Value);

    private:
        void BuildUI();

        Smile::Renderer* RendererPtr = nullptr;

        // Sun + atmosphere
        QDoubleSpinBox* AzimuthSpin      = nullptr;
        QDoubleSpinBox* ElevationSpin    = nullptr;
        QDoubleSpinBox* SunIntensitySpin = nullptr;
        QCheckBox*      AtmosphereCheck  = nullptr;
        QDoubleSpinBox* SunDiskSpin      = nullptr;
        QDoubleSpinBox* GlareSpin        = nullptr;
        QCheckBox*      AtmoAmbientCheck = nullptr;
        QDoubleSpinBox* AtmoAmbientSpin  = nullptr;

        // Clouds
        QCheckBox*      CloudsCheck   = nullptr;
        QDoubleSpinBox* CoverageSpin  = nullptr;
        QDoubleSpinBox* DensitySpin   = nullptr;
        QDoubleSpinBox* AltBottomSpin = nullptr;
        QDoubleSpinBox* ThicknessSpin = nullptr;
        QDoubleSpinBox* WindSpin      = nullptr;
        QDoubleSpinBox* PhaseSpin     = nullptr;
        QDoubleSpinBox* PowderSpin    = nullptr;
        QDoubleSpinBox* ErosionSpin   = nullptr;
    };
}
