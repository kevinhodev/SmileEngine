#include "SmileEditor/EnvironmentPanel.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Core/Logger.h"
#include "Smile/Math/Math.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>

namespace SmileEditor {
    EnvironmentPanel::EnvironmentPanel(QWidget* _Parent)
        : QWidget(_Parent)
    {
        setMinimumWidth(240);
        setMaximumWidth(320);
        BuildUI();
    }

    void EnvironmentPanel::BuildUI() {
        auto* RootLayout = new QVBoxLayout(this);
        RootLayout->setContentsMargins(0, 0, 0, 0);
        RootLayout->setSpacing(0);

        auto* Header = new QLabel(tr("Environment (IBL)"), this);
        Header->setObjectName("MaterialHeader");
        RootLayout->addWidget(Header);

        auto* Container = new QWidget(this);
        auto* Layout    = new QVBoxLayout(Container);
        Layout->setContentsMargins(6, 6, 6, 6);
        Layout->setSpacing(6);

        // --- HDR file group ---
        auto* FileGroup = new QGroupBox(tr("HDR Texture"), Container);
        auto* FileLayout = new QVBoxLayout(FileGroup);
        FileLayout->setContentsMargins(4, 8, 4, 4);
        FileLayout->setSpacing(4);

        CurrentPathLabel = new QLabel(tr("(nenhum)"), FileGroup);
        CurrentPathLabel->setWordWrap(true);
        CurrentPathLabel->setStyleSheet("color: #888;");
        FileLayout->addWidget(CurrentPathLabel);

        auto* BtnRow = new QHBoxLayout();
        BrowseBtn = new QPushButton(tr("Browse..."), FileGroup);
        ClearBtn  = new QPushButton(tr("Clear"),     FileGroup);
        BtnRow->addWidget(BrowseBtn);
        BtnRow->addWidget(ClearBtn);
        FileLayout->addLayout(BtnRow);
        Layout->addWidget(FileGroup);

        connect(BrowseBtn, &QPushButton::clicked, this, &EnvironmentPanel::OnBrowseHDR);
        connect(ClearBtn,  &QPushButton::clicked, this, &EnvironmentPanel::OnClearHDR);

        // --- Parameters group ---
        auto* ParamsGroup  = new QGroupBox(tr("Parameters"), Container);
        auto* ParamsLayout = new QFormLayout(ParamsGroup);
        ParamsLayout->setContentsMargins(4, 8, 4, 4);
        ParamsLayout->setHorizontalSpacing(6);
        ParamsLayout->setVerticalSpacing(4);

        IntensitySpin = new QDoubleSpinBox(ParamsGroup);
        IntensitySpin->setRange(0.0, 16.0);
        IntensitySpin->setSingleStep(0.1);
        IntensitySpin->setDecimals(2);
        IntensitySpin->setValue(1.0);
        ParamsLayout->addRow(tr("Intensidade"), IntensitySpin);

        RotationSpin = new QDoubleSpinBox(ParamsGroup);
        RotationSpin->setRange(-360.0, 360.0);
        RotationSpin->setSingleStep(5.0);
        RotationSpin->setDecimals(1);
        RotationSpin->setSuffix(QStringLiteral("°"));
        RotationSpin->setValue(0.0);
        ParamsLayout->addRow(tr("Rotação Y"), RotationSpin);

        ShowSkyboxCheck = new QCheckBox(tr("Mostrar skybox"), ParamsGroup);
        ShowSkyboxCheck->setChecked(true);
        ParamsLayout->addRow(QString(), ShowSkyboxCheck);

        Layout->addWidget(ParamsGroup);

        auto* WaterGroup  = new QGroupBox(tr("Water Debug"), Container);
        auto* WaterLayout = new QFormLayout(WaterGroup);
        WaterLayout->setContentsMargins(4, 8, 4, 4);
        WaterLayout->setHorizontalSpacing(6);
        WaterLayout->setVerticalSpacing(4);

        WaterDebugCombo = new QComboBox(WaterGroup);
        WaterDebugCombo->addItem(tr("Off"));
        WaterDebugCombo->addItem(tr("Wireframe"));
        WaterDebugCombo->addItem(tr("Tiles / LOD"));
        WaterDebugCombo->addItem(tr("Displacement"));
        WaterDebugCombo->addItem(tr("Normal"));
        WaterDebugCombo->addItem(tr("Fresnel"));
        WaterDebugCombo->addItem(tr("Body"));
        WaterDebugCombo->addItem(tr("Reflection"));
        WaterDebugCombo->addItem(tr("Foam / J"));
        WaterDebugCombo->addItem(tr("Depth"));
        WaterLayout->addRow(tr("Modo"), WaterDebugCombo);

        FoamCheck = new QCheckBox(tr("Espuma (foam)"), WaterGroup);
        FoamCheck->setChecked(true);
        WaterLayout->addRow(QString(), FoamCheck);

        FoamCoverageSpin = new QDoubleSpinBox(WaterGroup);
        FoamCoverageSpin->setRange(-2.0, 2.0);
        FoamCoverageSpin->setSingleStep(0.02);
        FoamCoverageSpin->setDecimals(2);
        FoamCoverageSpin->setValue(0.62);
        FoamCoverageSpin->setToolTip(tr("Limiar do Jacobiano: maior = mais espuma"));
        WaterLayout->addRow(tr("Cobertura"), FoamCoverageSpin);

        FoamIntensitySpin = new QDoubleSpinBox(WaterGroup);
        FoamIntensitySpin->setRange(0.0, 4.0);
        FoamIntensitySpin->setSingleStep(0.1);
        FoamIntensitySpin->setDecimals(2);
        FoamIntensitySpin->setValue(1.0);
        WaterLayout->addRow(tr("Intensidade"), FoamIntensitySpin);

        WindSpeedSpin = new QDoubleSpinBox(WaterGroup);
        WindSpeedSpin->setRange(0.1, 30.0);
        WindSpeedSpin->setSingleStep(0.25);
        WindSpeedSpin->setDecimals(2);
        WindSpeedSpin->setValue(4.0);
        WindSpeedSpin->setToolTip(tr("Velocidade usada no espectro Phillips"));
        WaterLayout->addRow(tr("Vento"), WindSpeedSpin);

        WavesAmountSpin = new QDoubleSpinBox(WaterGroup);
        WavesAmountSpin->setRange(0.1, 5.0);
        WavesAmountSpin->setSingleStep(0.05);
        WavesAmountSpin->setDecimals(2);
        WavesAmountSpin->setValue(1.5);
        WavesAmountSpin->setToolTip(tr("Energia/amplitude do espectro FFT"));
        WaterLayout->addRow(tr("Ondas"), WavesAmountSpin);

        // FFT (256^2 na GPU) — escala mestra do deslocamento e das componentes choppy.
        WaveHeightSpin = new QDoubleSpinBox(WaterGroup);
        WaveHeightSpin->setRange(0.0, 4.0);
        WaveHeightSpin->setSingleStep(0.05);
        WaveHeightSpin->setDecimals(2);
        WaveHeightSpin->setValue(1.0);
        WaveHeightSpin->setToolTip(tr("Escala mestra do deslocamento do FFT"));
        WaterLayout->addRow(tr("Altura ondas"), WaveHeightSpin);

        ChoppySpin = new QDoubleSpinBox(WaterGroup);
        ChoppySpin->setRange(0.0, 4.0);
        ChoppySpin->setSingleStep(0.05);
        ChoppySpin->setDecimals(2);
        ChoppySpin->setValue(1.5);
        ChoppySpin->setToolTip(tr("Escala das componentes horizontais (choppy)"));
        WaterLayout->addRow(tr("Choppy"), ChoppySpin);

        ReflectionScaleSpin = new QDoubleSpinBox(WaterGroup);
        ReflectionScaleSpin->setRange(0.0, 3.0);
        ReflectionScaleSpin->setSingleStep(0.05);
        ReflectionScaleSpin->setDecimals(2);
        ReflectionScaleSpin->setValue(1.0);
        ReflectionScaleSpin->setToolTip(tr("Peso do reflexo no composite final"));
        WaterLayout->addRow(tr("Reflexao"), ReflectionScaleSpin);

        ReflectionBumpSpin = new QDoubleSpinBox(WaterGroup);
        ReflectionBumpSpin->setRange(0.0, 1.0);
        ReflectionBumpSpin->setSingleStep(0.01);
        ReflectionBumpSpin->setDecimals(2);
        ReflectionBumpSpin->setValue(0.18);
        ReflectionBumpSpin->setToolTip(tr("Quanto a normal das ondas distorce a reflexao"));
        WaterLayout->addRow(tr("Refl. bump"), ReflectionBumpSpin);

        RefractionBumpSpin = new QDoubleSpinBox(WaterGroup);
        RefractionBumpSpin->setRange(0.0, 0.5);
        RefractionBumpSpin->setSingleStep(0.01);
        RefractionBumpSpin->setDecimals(2);
        RefractionBumpSpin->setValue(0.10);
        RefractionBumpSpin->setToolTip(tr("Quanto a normal distorce a refracao"));
        WaterLayout->addRow(tr("Refr. bump"), RefractionBumpSpin);

        BumpStrengthSpin = new QDoubleSpinBox(WaterGroup);
        BumpStrengthSpin->setRange(0.0, 2.0);
        BumpStrengthSpin->setSingleStep(0.05);
        BumpStrengthSpin->setDecimals(2);
        BumpStrengthSpin->setValue(0.5);
        BumpStrengthSpin->setToolTip(tr("Peso das normais de detalhe"));
        WaterLayout->addRow(tr("Bump fino"), BumpStrengthSpin);

        FogDensitySpin = new QDoubleSpinBox(WaterGroup);
        FogDensitySpin->setRange(0.0, 2.0);
        FogDensitySpin->setSingleStep(0.02);
        FogDensitySpin->setDecimals(2);
        FogDensitySpin->setValue(0.10);
        FogDensitySpin->setToolTip(tr("Densidade da absorcao Beer-Lambert"));
        WaterLayout->addRow(tr("Fog agua"), FogDensitySpin);

        InScatterDensitySpin = new QDoubleSpinBox(WaterGroup);
        InScatterDensitySpin->setRange(0.0, 6.0);
        InScatterDensitySpin->setSingleStep(0.1);
        InScatterDensitySpin->setDecimals(2);
        InScatterDensitySpin->setValue(1.5);
        InScatterDensitySpin->setToolTip(tr("Densidade do espalhamento turquesa"));
        WaterLayout->addRow(tr("In-scatter"), InScatterDensitySpin);

        Layout->addWidget(WaterGroup);

        Layout->addStretch();

        RootLayout->addWidget(Container);

        connect(IntensitySpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnIntensityChanged);
        connect(RotationSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnRotationChanged);
        connect(ShowSkyboxCheck, &QCheckBox::toggled, this, &EnvironmentPanel::OnShowSkyboxToggled);
        connect(WaterDebugCombo,
                static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, &EnvironmentPanel::OnWaterDebugModeChanged);
        connect(FoamCheck, &QCheckBox::toggled, this, &EnvironmentPanel::OnFoamToggled);
        connect(FoamCoverageSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnFoamCoverageChanged);
        connect(FoamIntensitySpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnFoamIntensityChanged);
        connect(WindSpeedSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnWindSpeedChanged);
        connect(WavesAmountSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnWavesAmountChanged);
        connect(WaveHeightSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnWaveHeightChanged);
        connect(ChoppySpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnChoppyChanged);
        connect(ReflectionScaleSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnReflectionScaleChanged);
        connect(ReflectionBumpSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnReflectionBumpChanged);
        connect(RefractionBumpSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnRefractionBumpChanged);
        connect(BumpStrengthSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnBumpStrengthChanged);
        connect(FogDensitySpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnFogDensityChanged);
        connect(InScatterDensitySpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnInScatterDensityChanged);
    }

    void EnvironmentPanel::InitializeWithRenderer(Smile::Renderer* _Renderer) {
        RendererPtr = _Renderer;
        if (!RendererPtr) return;
        // Sync UI to renderer defaults.
        IntensitySpin  ->setValue(RendererPtr->GetIBLIntensity());
        RotationSpin   ->setValue(RendererPtr->GetIBLRotation() / Smile::ToRad);
        ShowSkyboxCheck->setChecked(RendererPtr->GetShowSkybox());
        WaterDebugCombo->setCurrentIndex(static_cast<int>(RendererPtr->GetWater().GetDebugMode()));
        FoamCheck        ->setChecked(RendererPtr->GetWater().GetUseFoam());
        FoamCoverageSpin ->setValue(RendererPtr->GetWater().GetFoamCoverage());
        FoamIntensitySpin->setValue(RendererPtr->GetWater().GetFoamIntensity());
        WindSpeedSpin    ->setValue(RendererPtr->GetWater().GetWindSpeed());
        WavesAmountSpin  ->setValue(RendererPtr->GetWater().GetWavesAmount());
        WaveHeightSpin   ->setValue(RendererPtr->GetWater().GetFFTDisplacementScale());
        ChoppySpin       ->setValue(RendererPtr->GetWater().GetFFTChoppyScale());
        ReflectionScaleSpin->setValue(RendererPtr->GetWater().GetReflectionScale());
        ReflectionBumpSpin->setValue(RendererPtr->GetWater().GetReflectionBumpScale());
        RefractionBumpSpin->setValue(RendererPtr->GetWater().GetRefractionBumpScale());
        BumpStrengthSpin ->setValue(RendererPtr->GetWater().GetBumpStrength());
        FogDensitySpin   ->setValue(RendererPtr->GetWater().GetFogDensity());
        InScatterDensitySpin->setValue(RendererPtr->GetWater().GetInScatterDensity());

        // Auto-carrega o primeiro HDR encontrado em Assets/HDRi como ambiente padrão.
#ifdef SMILE_ASSETS_DIR
        QDir HdrDir(QString::fromUtf8(SMILE_ASSETS_DIR) + "/HDRi");
        const QStringList Hdrs = HdrDir.entryList(QStringList{ "*.hdr" }, QDir::Files, QDir::Name);
        if (!Hdrs.isEmpty()) {
            const QString File = HdrDir.filePath(Hdrs.first());
            if (RendererPtr->LoadHDREnvironment(File.toStdWString())) {
                CurrentPathLabel->setText(QFileInfo(File).fileName());
                CurrentPathLabel->setToolTip(File);
                CurrentPathLabel->setStyleSheet("color: #ddd;");
            } else {
                Smile::LogError("EnvironmentPanel: falha ao auto-carregar HDR: " + File.toStdString());
            }
        }
#endif
    }

    void EnvironmentPanel::OnBrowseHDR() {
        if (!RendererPtr) return;
        QString File = QFileDialog::getOpenFileName(
            this, tr("Selecionar HDR equirectangular"),
            QString(),
            tr("HDR (*.hdr);;Todos os arquivos (*.*)"));
        if (File.isEmpty()) return;

        std::wstring WPath = File.toStdWString();
        if (RendererPtr->LoadHDREnvironment(WPath)) {
            CurrentPathLabel->setText(QFileInfo(File).fileName());
            CurrentPathLabel->setToolTip(File);
            CurrentPathLabel->setStyleSheet("color: #ddd;");
        } else {
            Smile::LogError("EnvironmentPanel: falha ao carregar HDR: " + File.toStdString());
        }
    }

    void EnvironmentPanel::OnClearHDR() {
        // No real "unload" — the renderer just keeps the last loaded env. We
        // reset the label and force IBL contribution to zero via intensity.
        CurrentPathLabel->setText(tr("(nenhum)"));
        CurrentPathLabel->setToolTip(QString());
        CurrentPathLabel->setStyleSheet("color: #888;");
    }

    void EnvironmentPanel::OnIntensityChanged(double _Value) {
        if (RendererPtr) RendererPtr->SetIBLIntensity(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnRotationChanged(double _Degrees) {
        if (RendererPtr) RendererPtr->SetIBLRotation(static_cast<float>(_Degrees) * Smile::ToRad);
    }

    void EnvironmentPanel::OnShowSkyboxToggled(bool _Checked) {
        if (RendererPtr) RendererPtr->SetShowSkybox(_Checked);
    }

    void EnvironmentPanel::OnWaterDebugModeChanged(int _Index) {
        if (!RendererPtr) return;
        RendererPtr->GetWater().SetDebugMode(static_cast<Smile::FWaterRenderer::EDebugMode>(_Index));
    }

    void EnvironmentPanel::OnFoamToggled(bool _Checked) {
        if (RendererPtr) RendererPtr->GetWater().SetUseFoam(_Checked);
    }

    void EnvironmentPanel::OnFoamCoverageChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetFoamCoverage(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnFoamIntensityChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetFoamIntensity(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnWindSpeedChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetWindSpeed(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnWavesAmountChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetWavesAmount(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnWaveHeightChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetFFTDisplacementScale(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnChoppyChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetFFTChoppyScale(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnReflectionScaleChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetReflectionScale(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnReflectionBumpChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetReflectionBumpScale(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnRefractionBumpChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetRefractionBumpScale(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnBumpStrengthChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetBumpStrength(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnFogDensityChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetFogDensity(static_cast<float>(_Value));
    }

    void EnvironmentPanel::OnInScatterDensityChanged(double _Value) {
        if (RendererPtr) RendererPtr->GetWater().SetInScatterDensity(static_cast<float>(_Value));
    }
}
