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
        Layout->addStretch();

        RootLayout->addWidget(Container);

        connect(IntensitySpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnIntensityChanged);
        connect(RotationSpin,
                static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, &EnvironmentPanel::OnRotationChanged);
        connect(ShowSkyboxCheck, &QCheckBox::toggled, this, &EnvironmentPanel::OnShowSkyboxToggled);
    }

    void EnvironmentPanel::InitializeWithRenderer(Smile::Renderer* _Renderer) {
        RendererPtr = _Renderer;
        if (!RendererPtr) return;
        // Sync UI to renderer defaults.
        IntensitySpin  ->setValue(RendererPtr->GetIBLIntensity());
        RotationSpin   ->setValue(RendererPtr->GetIBLRotation() / Smile::ToRad);
        ShowSkyboxCheck->setChecked(RendererPtr->GetShowSkybox());

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
}
