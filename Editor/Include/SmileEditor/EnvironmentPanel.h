#pragma once

#include <QWidget>
#include <QString>

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QCheckBox;

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

    private:
        void BuildUI();

        Smile::Renderer* RendererPtr = nullptr;

        QLabel*         CurrentPathLabel = nullptr;
        QPushButton*    BrowseBtn        = nullptr;
        QPushButton*    ClearBtn         = nullptr;
        QDoubleSpinBox* IntensitySpin    = nullptr;
        QDoubleSpinBox* RotationSpin     = nullptr; // degrees
        QCheckBox*      ShowSkyboxCheck  = nullptr;
    };
}
