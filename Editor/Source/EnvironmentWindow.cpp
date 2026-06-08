#include "SmileEditor/EnvironmentWindow.h"
#include "SmileEditor/LucideIcon.h"
#include "SmileEditor/SmileLogo.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Math/Math.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace SmileEditor {
    namespace {
        constexpr int kSliderSteps = 1000;
        constexpr QColor kIconColor(221, 216, 202);
        constexpr QColor kAccentColor(217, 165, 20);

        int ValueToSlider(double Value, double Min, double Max) {
            if (Max <= Min) return 0;
            const double T = (Value - Min) / (Max - Min);
            return std::clamp(static_cast<int>(std::round(T * kSliderSteps)), 0, kSliderSteps);
        }

        double SliderToValue(int SliderValue, double Min, double Max) {
            const double T = static_cast<double>(SliderValue) / static_cast<double>(kSliderSteps);
            return Min + (Max - Min) * T;
        }

        QLabel* MakeIconLabel(const QString& IconName, const QColor& Color, int Size, QWidget* Parent) {
            auto* Icon = new QLabel(Parent);
            Icon->setObjectName("EnvironmentIconLabel");
            Icon->setFixedSize(Size, Size);
            Icon->setPixmap(MakeLucideIcon(IconName, Color, Size).pixmap(Size, Size));
            Icon->setAlignment(Qt::AlignCenter);
            return Icon;
        }
    }

    EnvironmentWindow::EnvironmentWindow(QWidget* _Parent)
        : QDialog(_Parent)
    {
        setWindowTitle(tr("Ambiente & Céu"));
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setMinimumSize(780, 580);
        resize(900, 680);

        auto* OuterLayout = new QVBoxLayout(this);
        OuterLayout->setContentsMargins(10, 10, 10, 10);
        OuterLayout->setSpacing(0);

        auto* Root = new QFrame(this);
        Root->setObjectName("EnvironmentWindowRoot");
        OuterLayout->addWidget(Root);

        auto* RootLayout = new QVBoxLayout(Root);
        RootLayout->setContentsMargins(0, 0, 0, 0);
        RootLayout->setSpacing(0);

        RootLayout->addWidget(BuildTitleBar());

        auto* Body = new QWidget(Root);
        Body->setObjectName("EnvironmentWindowBody");
        auto* BodyLayout = new QHBoxLayout(Body);
        BodyLayout->setContentsMargins(14, 14, 14, 14);
        BodyLayout->setSpacing(14);

        auto* Content = BuildContent();
        auto* Sidebar = BuildSidebar();
        BodyLayout->addWidget(Sidebar);
        BodyLayout->addWidget(Content, 1);
        RootLayout->addWidget(Body, 1);
    }

    QWidget* EnvironmentWindow::BuildTitleBar() {
        TitleBar = new QWidget(this);
        TitleBar->setObjectName("EnvironmentTitleBar");
        TitleBar->setFixedHeight(52);
        TitleBar->installEventFilter(this);

        auto* Layout = new QHBoxLayout(TitleBar);
        Layout->setContentsMargins(16, 0, 12, 0);
        Layout->setSpacing(10);

        auto* Logo = new QLabel(TitleBar);
        Logo->setObjectName("EnvironmentTitleLogo");
        Logo->setFixedSize(30, 30);
        Logo->setPixmap(MakeSmileLogoPixmap(30, false));
        Logo->setScaledContents(true);
        Layout->addWidget(Logo);

        auto* Title = new QLabel(tr("Ambiente & Céu"), TitleBar);
        Title->setObjectName("EnvironmentTitle");
        Layout->addWidget(Title);
        Layout->addStretch(1);

        auto MakeWindowButton = [&](const QString& Icon, const QString& Tip) {
            auto* Button = new QToolButton(TitleBar);
            Button->setObjectName("EnvironmentWindowButton");
            Button->setToolTip(Tip);
            Button->setIcon(MakeLucideIcon(Icon, kIconColor, 16));
            Button->setIconSize(QSize(16, 16));
            Button->setFixedSize(30, 30);
            return Button;
        };

        auto* Minimize = MakeWindowButton(QStringLiteral("minus"), tr("Minimizar"));
        auto* Maximize = MakeWindowButton(QStringLiteral("square"), tr("Maximizar"));
        auto* Close = MakeWindowButton(QStringLiteral("x"), tr("Fechar"));
        Close->setObjectName("EnvironmentCloseButton");

        connect(Minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
        connect(Maximize, &QToolButton::clicked, this, [this] {
            isMaximized() ? showNormal() : showMaximized();
        });
        connect(Close, &QToolButton::clicked, this, &QWidget::close);

        Layout->addWidget(Minimize);
        Layout->addWidget(Maximize);
        Layout->addWidget(Close);

        return TitleBar;
    }

    QWidget* EnvironmentWindow::BuildSidebar() {
        auto* Sidebar = new QFrame(this);
        Sidebar->setObjectName("EnvironmentSidebar");
        Sidebar->setFixedWidth(230);

        auto* Layout = new QVBoxLayout(Sidebar);
        Layout->setContentsMargins(12, 12, 12, 12);
        Layout->setSpacing(7);

        SidebarGroup = new QButtonGroup(Sidebar);
        SidebarGroup->setExclusive(true);

        Layout->addWidget(AddSidebarButton(QStringLiteral("sun"), tr("Sol"), SunSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("cloud-sun"), tr("Céu & Atmosfera"), AtmosphereSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("cloud"), tr("Nuvens"), CloudSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("waves"), tr("Oceano"), OceanSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("globe"), tr("Ambiente"), EnvironmentSection));
        Layout->addStretch(1);

        auto* ResetButton = new QPushButton(tr("Redefinir tudo"), Sidebar);
        ResetButton->setObjectName("EnvironmentResetButton");
        ResetButton->setIcon(MakeLucideIcon(QStringLiteral("rotate-ccw"), kIconColor, 16));
        ResetButton->setIconSize(QSize(16, 16));
        ResetButton->setMinimumHeight(38);
        connect(ResetButton, &QPushButton::clicked, this, &EnvironmentWindow::SetDefaults);
        Layout->addWidget(ResetButton);

        if (auto* First = qobject_cast<QPushButton*>(SidebarGroup->buttons().value(0))) {
            First->setChecked(true);
        }

        return Sidebar;
    }

    QPushButton* EnvironmentWindow::AddSidebarButton(const QString& Icon, const QString& Text, QWidget* Target) {
        auto* Button = new QPushButton(Text, this);
        Button->setObjectName("EnvironmentNavButton");
        Button->setCheckable(true);
        Button->setIcon(MakeLucideIcon(Icon, kIconColor, 19));
        Button->setIconSize(QSize(19, 19));
        Button->setMinimumHeight(50);
        Button->setCursor(Qt::PointingHandCursor);
        SidebarGroup->addButton(Button);

        connect(Button, &QPushButton::clicked, this, [this, Target] {
            if (ContentScroll && Target) {
                ContentScroll->ensureWidgetVisible(Target, 0, 16);
            }
        });
        return Button;
    }

    QWidget* EnvironmentWindow::BuildContent() {
        auto* Panel = new QFrame(this);
        Panel->setObjectName("EnvironmentContentPanel");

        auto* Layout = new QVBoxLayout(Panel);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(0);

        ContentScroll = new QScrollArea(Panel);
        ContentScroll->setObjectName("EnvironmentContentScroll");
        ContentScroll->setWidgetResizable(true);
        ContentScroll->setFrameShape(QFrame::NoFrame);

        ContentWidget = new QWidget(ContentScroll);
        ContentWidget->setObjectName("EnvironmentContentWidget");
        auto* ContentLayout = new QVBoxLayout(ContentWidget);
        ContentLayout->setContentsMargins(0, 0, 0, 0);
        ContentLayout->setSpacing(0);

        ContentLayout->addWidget(BuildSection(QStringLiteral("sun"), tr("SOL"), BuildSunSection(), &SunSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("orbit"), tr("ATMOSFERA"), BuildAtmosphereSection(), &AtmosphereSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("cloud"), tr("NUVENS"), BuildCloudSection(), &CloudSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("waves"), tr("OCEANO"), BuildOceanSection(), &OceanSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("globe"), tr("AMBIENTE"), BuildEnvironmentSection(), &EnvironmentSection));
        ContentLayout->addStretch(1);

        ContentScroll->setWidget(ContentWidget);
        Layout->addWidget(ContentScroll, 1);
        Layout->addWidget(BuildFooter());

        return Panel;
    }

    QFrame* EnvironmentWindow::BuildSection(const QString& Icon, const QString& Title, QWidget* Body, QWidget** SectionOut) {
        auto* Section = new QFrame(this);
        Section->setObjectName("EnvironmentSection");
        if (SectionOut) *SectionOut = Section;

        auto* Layout = new QVBoxLayout(Section);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(0);

        auto* Header = new QWidget(Section);
        Header->setObjectName("EnvironmentSectionHeader");
        auto* HeaderLayout = new QHBoxLayout(Header);
        HeaderLayout->setContentsMargins(18, 11, 16, 6);
        HeaderLayout->setSpacing(8);
        HeaderLayout->addWidget(MakeIconLabel(Icon, kIconColor, 16, Header));

        auto* TitleLabel = new QLabel(Title, Header);
        TitleLabel->setObjectName("EnvironmentSectionTitle");
        HeaderLayout->addWidget(TitleLabel);
        HeaderLayout->addStretch(1);

        auto* CollapseButton = new QToolButton(Header);
        CollapseButton->setObjectName("EnvironmentCollapseButton");
        CollapseButton->setIcon(MakeLucideIcon(QStringLiteral("chevron-up"), kIconColor, 16));
        CollapseButton->setIconSize(QSize(16, 16));
        CollapseButton->setCheckable(true);
        CollapseButton->setFixedSize(24, 24);
        HeaderLayout->addWidget(CollapseButton);

        Body->setObjectName("EnvironmentSectionBody");
        connect(CollapseButton, &QToolButton::toggled, Body, [Body](bool Collapsed) {
            Body->setVisible(!Collapsed);
        });

        Layout->addWidget(Header);
        Layout->addWidget(Body);

        return Section;
    }

    QWidget* EnvironmentWindow::BuildSunSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        auto SunChanged = [this](double) {
            if (RendererPtr && AzimuthSpin && ElevationSpin) {
                RendererPtr->SetSunAzimuthElevation(static_cast<float>(AzimuthSpin->value()),
                                                    static_cast<float>(ElevationSpin->value()));
            }
        };
        Layout->addWidget(MakeNumericRow(tr("Azimute"), -180.0, 180.0, 1.0, 1, 0.0, QStringLiteral("°"), &AzimuthSpin, SunChanged));
        Layout->addWidget(MakeNumericRow(tr("Elevação"), -5.0, 90.0, 0.5, 1, 45.0, QStringLiteral("°"), &ElevationSpin, SunChanged));
        Layout->addWidget(MakeNumericRow(tr("Intensidade"), 0.0, 10.0, 0.1, 2, 2.5, QString(), &SunIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunIntensity(static_cast<float>(Value));
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildAtmosphereSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        Layout->addWidget(MakeToggleRow(tr("Ativar atmosfera física"), &AtmosphereCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseAtmosphereSky(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Disco do sol"), 0.1, 3.0, 0.05, 2, 0.7, QStringLiteral("°"), &SunDiskSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunDiskSize(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Glare"), 0.0, 12.0, 0.1, 1, 4.0, QString(), &GlareSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunGlare(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Ambiente da atmosfera"), &AtmosphereAmbientCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseAtmosphereAmbient(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Intens. ambiente"), 0.0, 4.0, 0.05, 2, 1.0, QString(), &AtmosphereAmbientSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetAtmosphereAmbientIntensity(static_cast<float>(Value));
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildCloudSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        Layout->addWidget(MakeToggleRow(tr("Nuvens volumétricas"), &CloudsCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseClouds(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Cobertura"), 0.0, 1.0, 0.01, 2, 0.45, QString(), &CoverageSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetCloudCoverage(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Densidade"), 0.1, 4.0, 0.05, 2, 1.6, QString(), &DensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetCloudDensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Altitude"), 0.5, 6.0, 0.05, 2, 2.0, QStringLiteral(" km"), &CloudAltitudeSpin, [this](double) {
            ApplyCloudAltitude();
        }));
        Layout->addWidget(MakeNumericRow(tr("Espessura"), 0.5, 5.0, 0.05, 2, 3.0, QStringLiteral(" km"), &CloudThicknessSpin, [this](double) {
            ApplyCloudAltitude();
        }));
        Layout->addWidget(MakeNumericRow(tr("Vento"), 0.0, 0.1, 0.001, 3, 0.01, QString(), &CloudWindSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetCloudWind(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Fase"), 0.0, 0.95, 0.01, 2, 0.8, QString(), &PhaseSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetCloudPhaseG(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Powder"), 0.0, 1.0, 0.01, 2, 0.5, QString(), &PowderSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetCloudPowder(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Erosão"), 0.0, 1.0, 0.01, 2, 0.45, QString(), &ErosionSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetCloudErosion(static_cast<float>(Value));
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildOceanSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        Layout->addWidget(MakeToggleRow(tr("Ativar oceano"), &OceanCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseWater(Checked);
        }));

        auto* ModeRow = new QWidget(Body);
        ModeRow->setObjectName("EnvironmentControlRow");
        auto* ModeLayout = new QHBoxLayout(ModeRow);
        ModeLayout->setContentsMargins(0, 0, 0, 0);
        ModeLayout->setSpacing(14);
        auto* ModeLabel = new QLabel(tr("Modo debug"), ModeRow);
        ModeLabel->setObjectName("EnvironmentRowLabel");
        ModeLayout->addWidget(ModeLabel, 1);
        WaterDebugCombo = new QComboBox(ModeRow);
        WaterDebugCombo->setObjectName("EnvironmentCombo");
        WaterDebugCombo->addItems(QStringList{ tr("Desligado"), tr("Aramado"), tr("Tiles / LOD"), tr("Deslocamento"),
                                               tr("Normal"), tr("Fresnel"), tr("Corpo"), tr("Reflexo"),
                                               tr("Espuma / J"), tr("Profundidade") });
        WaterDebugCombo->setFixedWidth(156);
        ModeLayout->addWidget(WaterDebugCombo);
        ModeLayout->addSpacing(200);
        connect(WaterDebugCombo, &QComboBox::currentIndexChanged, this, [this](int Index) {
            if (RendererPtr) {
                RendererPtr->GetWater().SetDebugMode(static_cast<Smile::FWaterRenderer::EDebugMode>(Index));
            }
        });
        Layout->addWidget(ModeRow);

        Layout->addWidget(MakeToggleRow(tr("Espuma"), &FoamCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->GetWater().SetUseFoam(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Cobertura espuma"), -2.0, 2.0, 0.01, 2, 0.62, QString(), &FoamCoverageSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetFoamCoverage(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Intensidade espuma"), 0.0, 4.0, 0.05, 2, 1.0, QString(), &FoamIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetFoamIntensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Vento"), 0.1, 30.0, 0.1, 2, 4.0, QString(), &OceanWindSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetWindSpeed(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Ondas"), 0.1, 5.0, 0.05, 2, 1.5, QString(), &WavesAmountSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetWavesAmount(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Altura ondas"), 0.0, 4.0, 0.05, 2, 1.0, QString(), &WaveHeightSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetFFTDisplacementScale(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Choppy"), 0.0, 4.0, 0.05, 2, 1.5, QString(), &ChoppySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetFFTChoppyScale(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Reflexão"), 0.0, 3.0, 0.05, 2, 1.0, QString(), &ReflectionScaleSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetReflectionScale(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Refl. bump"), 0.0, 1.0, 0.01, 2, 0.18, QString(), &ReflectionBumpSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetReflectionBumpScale(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Refr. bump"), 0.0, 0.5, 0.01, 2, 0.10, QString(), &RefractionBumpSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetRefractionBumpScale(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Bump fino"), 0.0, 2.0, 0.05, 2, 0.5, QString(), &BumpStrengthSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetBumpStrength(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Fog água"), 0.0, 2.0, 0.02, 2, 0.10, QString(), &FogDensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetFogDensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("In-scatter"), 0.0, 6.0, 0.05, 2, 1.5, QString(), &InScatterDensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetInScatterDensity(static_cast<float>(Value));
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildEnvironmentSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        auto* PathRow = new QWidget(Body);
        PathRow->setObjectName("EnvironmentHDRRow");
        auto* PathLayout = new QHBoxLayout(PathRow);
        PathLayout->setContentsMargins(0, 0, 0, 0);
        PathLayout->setSpacing(10);

        CurrentPathLabel = new QLabel(tr("(nenhum HDRI)"), PathRow);
        CurrentPathLabel->setObjectName("EnvironmentHDRLabel");
        CurrentPathLabel->setWordWrap(true);
        PathLayout->addWidget(CurrentPathLabel, 1);

        auto* Browse = new QPushButton(tr("Buscar..."), PathRow);
        Browse->setObjectName("EnvironmentMiniButton");
        auto* Clear = new QPushButton(tr("Limpar"), PathRow);
        Clear->setObjectName("EnvironmentMiniButton");
        connect(Browse, &QPushButton::clicked, this, &EnvironmentWindow::BrowseHDR);
        connect(Clear, &QPushButton::clicked, this, &EnvironmentWindow::ClearHDRLabel);
        PathLayout->addWidget(Browse);
        PathLayout->addWidget(Clear);
        Layout->addWidget(PathRow);

        Layout->addWidget(MakeNumericRow(tr("Intensidade IBL"), 0.0, 16.0, 0.1, 2, 1.0, QString(), &IBLIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetIBLIntensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Rotação Y"), -360.0, 360.0, 1.0, 1, 0.0, QStringLiteral("°"), &IBLRotationSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetIBLRotation(static_cast<float>(Value) * Smile::ToRad);
        }));
        Layout->addWidget(MakeToggleRow(tr("Mostrar skybox"), &ShowSkyboxCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetShowSkybox(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Bloom"), 0.0, 2.0, 0.01, 3, 0.04, QString(), &BloomIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetBloomIntensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Exposição"), 0.01, 10.0, 0.05, 2, 1.0, QString(), &ExposureSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetExposure(static_cast<float>(Value));
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildFooter() {
        auto* Footer = new QWidget(this);
        Footer->setObjectName("EnvironmentFooter");
        auto* Layout = new QHBoxLayout(Footer);
        Layout->setContentsMargins(18, 9, 18, 9);
        Layout->setSpacing(10);
        Layout->addWidget(MakeIconLabel(QStringLiteral("info"), QColor(155, 151, 142), 18, Footer));

        auto* Text = new QLabel(tr("Ajuste a posição do sol, atmosfera, nuvens, oceano e HDRI para controlar a iluminação e o ambiente da cena."), Footer);
        Text->setObjectName("EnvironmentFooterText");
        Text->setWordWrap(true);
        Layout->addWidget(Text, 1);
        return Footer;
    }

    QWidget* EnvironmentWindow::MakeToggleRow(const QString& Label, QCheckBox** OutCheck, bool Checked,
                                              const std::function<void(bool)>& OnChanged) {
        auto* Row = new QWidget(this);
        Row->setObjectName("EnvironmentControlRow");
        auto* Layout = new QHBoxLayout(Row);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(14);

        auto* LabelWidget = new QLabel(Label, Row);
        LabelWidget->setObjectName("EnvironmentRowLabel");
        Layout->addWidget(LabelWidget, 1);

        auto* Toggle = new QCheckBox(Row);
        Toggle->setObjectName("EnvironmentToggle");
        Toggle->setChecked(Checked);
        Layout->addWidget(Toggle);
        if (OutCheck) *OutCheck = Toggle;

        connect(Toggle, &QCheckBox::toggled, this, [OnChanged](bool Value) {
            OnChanged(Value);
        });
        return Row;
    }

    QWidget* EnvironmentWindow::MakeNumericRow(const QString& Label, double Min, double Max, double Step,
                                               int Decimals, double Value, const QString& Suffix,
                                               QDoubleSpinBox** OutSpin,
                                               const std::function<void(double)>& OnChanged) {
        auto* Row = new QWidget(this);
        Row->setObjectName("EnvironmentControlRow");
        auto* Layout = new QHBoxLayout(Row);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(14);

        auto* LabelWidget = new QLabel(Label, Row);
        LabelWidget->setObjectName("EnvironmentRowLabel");
        Layout->addWidget(LabelWidget, 1);

        auto* Spin = new QDoubleSpinBox(Row);
        Spin->setObjectName("EnvironmentValueSpin");
        Spin->setRange(Min, Max);
        Spin->setSingleStep(Step);
        Spin->setDecimals(Decimals);
        Spin->setValue(Value);
        Spin->setSuffix(Suffix);
        Spin->setFixedWidth(112);
        Layout->addWidget(Spin);

        auto* Slider = new QSlider(Qt::Horizontal, Row);
        Slider->setObjectName("EnvironmentSlider");
        Slider->setRange(0, kSliderSteps);
        Slider->setValue(ValueToSlider(Value, Min, Max));
        Slider->setMinimumWidth(200);
        Layout->addWidget(Slider);

        connect(Spin, &QDoubleSpinBox::valueChanged, this, [Slider, Min, Max, OnChanged](double NewValue) {
            const QSignalBlocker Blocker(Slider);
            Slider->setValue(ValueToSlider(NewValue, Min, Max));
            OnChanged(NewValue);
        });
        connect(Slider, &QSlider::valueChanged, this, [Spin, Min, Max, OnChanged](int SliderValue) {
            const double NewValue = SliderToValue(SliderValue, Min, Max);
            const QSignalBlocker Blocker(Spin);
            Spin->setValue(NewValue);
            OnChanged(NewValue);
        });

        if (OutSpin) *OutSpin = Spin;
        return Row;
    }

    void EnvironmentWindow::InitializeWithRenderer(Smile::Renderer* _Renderer) {
        RendererPtr = _Renderer;
        SyncFromRenderer();
    }

    void EnvironmentWindow::SetCurrentHDRPath(const QString& _Path) {
        if (!CurrentPathLabel) return;
        if (_Path.isEmpty()) {
            CurrentPathLabel->setText(tr("(nenhum HDRI)"));
            CurrentPathLabel->setToolTip(QString());
            return;
        }
        CurrentPathLabel->setText(QFileInfo(_Path).fileName());
        CurrentPathLabel->setToolTip(_Path);
    }

    void EnvironmentWindow::SyncFromRenderer() {
        if (!RendererPtr) return;

        const Smile::Vec3 SunDir = RendererPtr->GetSunDirection();
        const double Elevation = std::asin(std::clamp(SunDir.Y, -1.0f, 1.0f)) / Smile::ToRad;
        const double Azimuth = std::atan2(SunDir.X, SunDir.Z) / Smile::ToRad;

        if (AzimuthSpin) AzimuthSpin->setValue(Azimuth);
        if (ElevationSpin) ElevationSpin->setValue(Elevation);
        if (SunIntensitySpin) SunIntensitySpin->setValue(RendererPtr->GetSunIntensity());
        if (AtmosphereCheck) AtmosphereCheck->setChecked(RendererPtr->GetUseAtmosphereSky());
        if (SunDiskSpin) SunDiskSpin->setValue(RendererPtr->GetSunDiskSize());
        if (GlareSpin) GlareSpin->setValue(RendererPtr->GetSunGlare());
        if (AtmosphereAmbientCheck) AtmosphereAmbientCheck->setChecked(RendererPtr->GetUseAtmosphereAmbient());
        if (AtmosphereAmbientSpin) AtmosphereAmbientSpin->setValue(RendererPtr->GetAtmosphereAmbientIntensity());

        if (CloudsCheck) CloudsCheck->setChecked(RendererPtr->GetUseClouds());
        if (CoverageSpin) CoverageSpin->setValue(RendererPtr->GetCloudCoverage());
        if (DensitySpin) DensitySpin->setValue(RendererPtr->GetCloudDensity());
        if (CloudAltitudeSpin) CloudAltitudeSpin->setValue(RendererPtr->GetCloudBottomAltitude());
        if (CloudThicknessSpin) CloudThicknessSpin->setValue(RendererPtr->GetCloudThickness());
        if (CloudWindSpin) CloudWindSpin->setValue(RendererPtr->GetCloudWind());
        if (PhaseSpin) PhaseSpin->setValue(RendererPtr->GetCloudPhaseG());
        if (PowderSpin) PowderSpin->setValue(RendererPtr->GetCloudPowder());
        if (ErosionSpin) ErosionSpin->setValue(RendererPtr->GetCloudErosion());

        auto& Water = RendererPtr->GetWater();
        if (OceanCheck) OceanCheck->setChecked(RendererPtr->GetUseWater());
        if (WaterDebugCombo) WaterDebugCombo->setCurrentIndex(static_cast<int>(Water.GetDebugMode()));
        if (FoamCheck) FoamCheck->setChecked(Water.GetUseFoam());
        if (FoamCoverageSpin) FoamCoverageSpin->setValue(Water.GetFoamCoverage());
        if (FoamIntensitySpin) FoamIntensitySpin->setValue(Water.GetFoamIntensity());
        if (OceanWindSpin) OceanWindSpin->setValue(Water.GetWindSpeed());
        if (WavesAmountSpin) WavesAmountSpin->setValue(Water.GetWavesAmount());
        if (WaveHeightSpin) WaveHeightSpin->setValue(Water.GetFFTDisplacementScale());
        if (ChoppySpin) ChoppySpin->setValue(Water.GetFFTChoppyScale());
        if (ReflectionScaleSpin) ReflectionScaleSpin->setValue(Water.GetReflectionScale());
        if (ReflectionBumpSpin) ReflectionBumpSpin->setValue(Water.GetReflectionBumpScale());
        if (RefractionBumpSpin) RefractionBumpSpin->setValue(Water.GetRefractionBumpScale());
        if (BumpStrengthSpin) BumpStrengthSpin->setValue(Water.GetBumpStrength());
        if (FogDensitySpin) FogDensitySpin->setValue(Water.GetFogDensity());
        if (InScatterDensitySpin) InScatterDensitySpin->setValue(Water.GetInScatterDensity());

        if (IBLIntensitySpin) IBLIntensitySpin->setValue(RendererPtr->GetIBLIntensity());
        if (IBLRotationSpin) IBLRotationSpin->setValue(RendererPtr->GetIBLRotation() / Smile::ToRad);
        if (ShowSkyboxCheck) ShowSkyboxCheck->setChecked(RendererPtr->GetShowSkybox());
        if (BloomIntensitySpin) BloomIntensitySpin->setValue(RendererPtr->GetBloomIntensity());
        if (ExposureSpin) ExposureSpin->setValue(RendererPtr->GetExposure());
    }

    void EnvironmentWindow::SetDefaults() {
        if (AzimuthSpin) AzimuthSpin->setValue(30.0);
        if (ElevationSpin) ElevationSpin->setValue(3.0);
        if (SunIntensitySpin) SunIntensitySpin->setValue(2.5);
        if (AtmosphereCheck) AtmosphereCheck->setChecked(true);
        if (SunDiskSpin) SunDiskSpin->setValue(0.7);
        if (GlareSpin) GlareSpin->setValue(4.0);
        if (AtmosphereAmbientCheck) AtmosphereAmbientCheck->setChecked(true);
        if (AtmosphereAmbientSpin) AtmosphereAmbientSpin->setValue(1.0);

        if (CloudsCheck) CloudsCheck->setChecked(true);
        if (CoverageSpin) CoverageSpin->setValue(0.45);
        if (DensitySpin) DensitySpin->setValue(1.6);
        if (CloudAltitudeSpin) CloudAltitudeSpin->setValue(2.0);
        if (CloudThicknessSpin) CloudThicknessSpin->setValue(3.0);
        if (CloudWindSpin) CloudWindSpin->setValue(0.01);
        if (PhaseSpin) PhaseSpin->setValue(0.8);
        if (PowderSpin) PowderSpin->setValue(0.5);
        if (ErosionSpin) ErosionSpin->setValue(0.45);

        if (OceanCheck) OceanCheck->setChecked(true);
        if (WaterDebugCombo) WaterDebugCombo->setCurrentIndex(0);
        if (FoamCheck) FoamCheck->setChecked(true);
        if (FoamCoverageSpin) FoamCoverageSpin->setValue(0.62);
        if (FoamIntensitySpin) FoamIntensitySpin->setValue(1.0);
        if (OceanWindSpin) OceanWindSpin->setValue(4.0);
        if (WavesAmountSpin) WavesAmountSpin->setValue(1.5);
        if (WaveHeightSpin) WaveHeightSpin->setValue(1.0);
        if (ChoppySpin) ChoppySpin->setValue(1.5);
        if (ReflectionScaleSpin) ReflectionScaleSpin->setValue(1.0);
        if (ReflectionBumpSpin) ReflectionBumpSpin->setValue(0.18);
        if (RefractionBumpSpin) RefractionBumpSpin->setValue(0.10);
        if (BumpStrengthSpin) BumpStrengthSpin->setValue(0.5);
        if (FogDensitySpin) FogDensitySpin->setValue(0.10);
        if (InScatterDensitySpin) InScatterDensitySpin->setValue(1.5);

        if (IBLIntensitySpin) IBLIntensitySpin->setValue(1.0);
        if (IBLRotationSpin) IBLRotationSpin->setValue(0.0);
        if (ShowSkyboxCheck) ShowSkyboxCheck->setChecked(true);
        if (BloomIntensitySpin) BloomIntensitySpin->setValue(0.04);
        if (ExposureSpin) ExposureSpin->setValue(1.0);
    }

    void EnvironmentWindow::ApplyCloudAltitude() {
        if (RendererPtr && CloudAltitudeSpin && CloudThicknessSpin) {
            RendererPtr->SetCloudAltitude(static_cast<float>(CloudAltitudeSpin->value()),
                                          static_cast<float>(CloudThicknessSpin->value()));
        }
    }

    void EnvironmentWindow::BrowseHDR() {
        if (!RendererPtr) return;
        const QString File = QFileDialog::getOpenFileName(
            this,
            tr("Selecionar HDR equirectangular"),
            QString(),
            tr("HDR (*.hdr);;Todos os arquivos (*.*)"));
        if (File.isEmpty()) return;

        if (RendererPtr->LoadHDREnvironment(File.toStdWString())) {
            SetCurrentHDRPath(File);
            emit HDRChanged(File);
        }
    }

    void EnvironmentWindow::ClearHDRLabel() {
        SetCurrentHDRPath(QString());
        emit HDRChanged(QString());
    }

    bool EnvironmentWindow::eventFilter(QObject* _Object, QEvent* _Event) {
        if (_Object != TitleBar) {
            return QDialog::eventFilter(_Object, _Event);
        }

        if (_Event->type() == QEvent::MouseButtonPress) {
            auto* Mouse = static_cast<QMouseEvent*>(_Event);
            if (Mouse->button() == Qt::LeftButton) {
                Dragging = true;
                DragStartPosition = Mouse->globalPosition().toPoint() - frameGeometry().topLeft();
                return true;
            }
        }
        if (_Event->type() == QEvent::MouseMove && Dragging && !isMaximized()) {
            auto* Mouse = static_cast<QMouseEvent*>(_Event);
            move(Mouse->globalPosition().toPoint() - DragStartPosition);
            return true;
        }
        if (_Event->type() == QEvent::MouseButtonRelease) {
            Dragging = false;
            return true;
        }
        if (_Event->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }

        return QDialog::eventFilter(_Object, _Event);
    }
}
