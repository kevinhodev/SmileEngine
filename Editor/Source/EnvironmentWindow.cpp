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
        Layout->addWidget(AddSidebarButton(QStringLiteral("moon"), tr("Sombras"), ShadowSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("cloud-sun"), tr("Céu & Atmosfera"), AtmosphereSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("cloud"), tr("Nuvens"), CloudSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("wind"), tr("Névoa"), FogSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("waves"), tr("Oceano"), OceanSection));
        Layout->addWidget(AddSidebarButton(QStringLiteral("sparkles"), tr("GI"), GISection));
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
        ContentLayout->addWidget(BuildSection(QStringLiteral("moon"), tr("SOMBRAS DO SOL"), BuildShadowSection(), &ShadowSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("orbit"), tr("ATMOSFERA"), BuildAtmosphereSection(), &AtmosphereSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("cloud"), tr("NUVENS"), BuildCloudSection(), &CloudSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("wind"), tr("NÉVOA"), BuildFogSection(), &FogSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("waves"), tr("OCEANO"), BuildOceanSection(), &OceanSection));
        ContentLayout->addWidget(BuildSection(QStringLiteral("sparkles"), tr("GI (ILUMINAÇÃO GLOBAL)"), BuildGISection(), &GISection));
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

        // --- Time-of-Day (F1): o relogio dirige a posicao do sol ---
        Layout->addWidget(MakeToggleRow(tr("Ciclo dia/noite (TOD)"), &TODCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().Enabled = Checked;
            // Quando ligado, o relogio sobrepoe Azimute/Elevacao manuais.
            if (AzimuthSpin)   AzimuthSpin->setEnabled(!Checked);
            if (ElevationSpin) ElevationSpin->setEnabled(!Checked);
        }));
        Layout->addWidget(MakeToggleRow(tr("Pausar relógio"), &TODPauseCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().Running = !Checked;
        }));
        Layout->addWidget(MakeNumericRow(tr("Hora"), 0.0, 24.0, 0.25, 2, 10.0, QStringLiteral(" h"), &TODHourSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().TimeHours = static_cast<float>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Duração do dia"), 2.0, 600.0, 1.0, 0, 120.0, QStringLiteral(" s"), &TODDayLengthSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().DayLengthSec = static_cast<float>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Latitude"), -90.0, 90.0, 1.0, 1, 40.0, QStringLiteral("°"), &TODLatitudeSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().LatitudeDeg = static_cast<float>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Dia do ano"), 1.0, 365.0, 1.0, 0, 172.0, QString(), &TODDayOfYearSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().DayOfYear = static_cast<int>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Norte (offset)"), -180.0, 180.0, 1.0, 1, 0.0, QStringLiteral("°"), &TODNorthSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().NorthOffsetDeg = static_cast<float>(Value);
        }));

        // --- Noite: lua (2a direcional) + estrelas (F2) ---
        Layout->addWidget(MakeToggleRow(tr("Lua + estrelas"), &MoonCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().MoonEnabled = Checked;
        }));
        Layout->addWidget(MakeNumericRow(tr("Intensidade do luar"), 0.0, 2.0, 0.02, 2, 0.25, QString(), &MoonIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().MoonIntensity = static_cast<float>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Intensidade estrelas"), 0.0, 4.0, 0.1, 2, 1.0, QString(), &StarIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().StarIntensity = static_cast<float>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Fase da lua"), 0.0, 24.0, 0.5, 1, 12.0, QStringLiteral(" h"), &MoonPhaseSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().MoonPhaseOffsetHours = static_cast<float>(Value);
        }));
        Layout->addWidget(MakeNumericRow(tr("Tamanho da lua"), 0.3, 5.0, 0.1, 1, 1.5, QStringLiteral("×"), &MoonSizeSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetTimeOfDay().MoonDiskSize = static_cast<float>(Value);
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildShadowSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        Layout->addWidget(MakeToggleRow(tr("Ativar sombras (CSM)"), &ShadowsCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseSunShadows(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Alcance"), 50.0, 5000.0, 50.0, 0, 800.0, QStringLiteral(" u"), &ShadowDistanceSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunShadowMaxDistance(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Suavidade"), 0.5, 8.0, 0.25, 2, 2.5, tr(" texels"), &ShadowPenumbraSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunShadowPenumbra(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Normal-offset"), 0.0, 8.0, 0.25, 2, 2.5, tr(" texels"), &ShadowNormalOffsetSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunShadowNormalOffset(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Bias"), 0.0, 0.005, 0.0001, 4, 0.0006, QString(), &ShadowBiasSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunShadowDepthBias(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Blend cascatas"), 0.0, 0.4, 0.01, 2, 0.1, QString(), &ShadowBlendSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetSunShadowBlendBand(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Debug: cores das cascatas"), &ShadowDebugCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetSunShadowDebug(Checked);
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

    QWidget* EnvironmentWindow::BuildFogSection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        Layout->addWidget(MakeToggleRow(tr("Aerial perspective"), &AerialFogCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseAerialPerspective(Checked);
        }));
        Layout->addWidget(MakeToggleRow(tr("Height fog"), &HeightFogCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseHeightFog(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Densidade"), 0.0, 0.01, 0.0001, 5, 0.0002, QString(), &AtmFogDensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetFog().SetDensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Falloff (alt.)"), 0.0, 0.05, 0.0001, 5, 0.0005, QString(), &AtmFogFalloffSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetFog().SetHeightFalloff(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Altura base"), -500.0, 2000.0, 10.0, 0, 0.0, QStringLiteral(" m"), &AtmFogHeightSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetFog().SetFogHeight(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Opacidade máx"), 0.0, 1.0, 0.01, 2, 0.85, QString(), &AtmFogOpacitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetFog().SetMaxOpacity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Dist. início"), 0.0, 20000.0, 100.0, 0, 0.0, QStringLiteral(" m"), &AtmFogStartSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetFog().SetStartDistance(static_cast<float>(Value));
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

        Layout->addWidget(MakeToggleRow(tr("Cull frustum"), &QuadtreeCullCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->GetWater().SetGpuFrustumCull(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Tile base"), 16.0, 256.0, 1.0, 0, 64.0, QStringLiteral(" m"), &QuadtreeBaseSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetTileBaseSize(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Profundidade QT"), 6.0, 10.0, 1.0, 0, 9.0, QString(), &QuadtreeDepthSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetTileMaxDepth(static_cast<Smile::u32>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Raio anéis"), 4.0, 16.0, 1.0, 0, 8.0, QString(), &QuadtreeRingSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetGpuRingRadius(static_cast<Smile::u32>(Value));
        }));

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
        Layout->addWidget(MakeNumericRow(tr("Translucência"), 0.0, 3.0, 0.05, 2, 0.6, QString(), &SSSStrengthSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetSSSStrength(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Espuma costa"), 0.0, 4.0, 0.05, 2, 1.0, QString(), &ShoreFoamSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetShoreFoamIntensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Clareza"), 0.1, 60.0, 0.5, 1, 8.0, QStringLiteral(" m"), &WaterClaritySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->GetWater().SetWaterClarity(static_cast<float>(Value));
        }));
        return Body;
    }

    QWidget* EnvironmentWindow::BuildGISection() {
        auto* Body = new QWidget(this);
        auto* Layout = new QVBoxLayout(Body);
        Layout->setContentsMargins(18, 0, 18, 12);
        Layout->setSpacing(6);

        Layout->addWidget(MakeToggleRow(tr("Ativar GI (DDGI)"), &GICheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseGI(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Intensidade"), 0.0, 8.0, 0.05, 2, 1.0, QString(), &GIIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIIntensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Visibilidade (Chebyshev, anti-leak)"), &GIChebyshevCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIChebyshev(Checked);
        }));
        Layout->addWidget(MakeToggleRow(tr("Pular probes inativas (anti-leak/flicker)"), &GISkipInactiveCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGISkipInactiveProbes(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Limiar de desativação (menor = + agressivo)"), 0.05, 0.60, 0.01, 2, 0.20, QString(), &GIDeactivThreshSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIDeactivationThreshold(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Fallback de vizinha (anti-buraco)"), &GIFallbackCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGISkipInactiveFallback(Checked);
        }));
        Layout->addWidget(MakeToggleRow(tr("Raios adaptativos (perf — menos raios longe de geom.)"), &GIAdaptiveRaysCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIAdaptiveRays(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Raios máx (perto)"), 8.0, 64.0, 8.0, 0, 64.0, QString(), &GIMaxRaysSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIMaxRays(static_cast<int>(Value + 0.5));
        }));
        Layout->addWidget(MakeNumericRow(tr("Raios mín (longe)"), 8.0, 256.0, 8.0, 0, 16.0, QString(), &GIMinRaysSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIMinRays(static_cast<int>(Value + 0.5));
        }));
        Layout->addWidget(MakeToggleRow(tr("Sombreamento real no hit (normal geométrica)"), &GIRealHitCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIRealHitShading(Checked);
        }));
        Layout->addWidget(MakeToggleRow(tr("Relocação de probes (anti-leak/parede)"), &GIRelocationCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIRelocation(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Estabilidade temporal (anti-flicker)"), 0.80, 0.99, 0.01, 2, 0.97, QString(), &GIHysteresisSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIHysteresis(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Debug: ver irradiância dos probes"), &GIDebugCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIDebug(Checked);
        }));

        // --- Debug DDGI: passe separado que desenha as probes como esferas + wireframe do volume ---
        Layout->addWidget(MakeToggleRow(tr("Desenhar probes (debug)"), &GIDebugProbesCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIDebugProbes(Checked);
        }));
        {
            auto* ModeRow = new QWidget(Body);
            ModeRow->setObjectName("EnvironmentControlRow");
            auto* ModeLayout = new QHBoxLayout(ModeRow);
            ModeLayout->setContentsMargins(0, 0, 0, 0);
            ModeLayout->setSpacing(14);
            auto* ModeLabel = new QLabel(tr("Modo debug"), ModeRow);
            ModeLabel->setObjectName("EnvironmentRowLabel");
            ModeLayout->addWidget(ModeLabel, 1);
            GIDebugModeCombo = new QComboBox(ModeRow);
            GIDebugModeCombo->setObjectName("EnvironmentCombo");
            GIDebugModeCombo->addItems(QStringList{ tr("Irradiância"), tr("Distância / depth"),
                                                    tr("Relocação"), tr("Classificação"),
                                                    tr("Estabilidade") });
            GIDebugModeCombo->setFixedWidth(156);
            ModeLayout->addWidget(GIDebugModeCombo);
            ModeLayout->addSpacing(200);
            connect(GIDebugModeCombo, &QComboBox::currentIndexChanged, this, [this](int Index) {
                if (RendererPtr) RendererPtr->SetGIDebugMode(static_cast<Smile::u32>(Index));
            });
            Layout->addWidget(ModeRow);
        }
        Layout->addWidget(MakeNumericRow(tr("Tamanho das probes"), 0.05, 0.5, 0.01, 2, 0.10, QString(), &GIDebugProbeSizeSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIDebugProbeSize(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Mostrar volume (AABB)"), &GIDebugVolumeCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIDebugShowVolume(Checked);
        }));
        Layout->addWidget(MakeToggleRow(tr("Mostrar nº de raios (rótulo na probe)"), &GIDebugRaysCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetGIDebugShowRays(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Alcance dos rótulos"), 1.0, 30.0, 0.5, 1, 6.0, QStringLiteral(" u"), &GIDebugRayRadiusSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetGIDebugRayRadius(static_cast<float>(Value));
        }));

        // --- Specular GI (reflexoes ray-traced, estilo Lumen). So sem MSAA; reusa o DDGI. ---
        Layout->addWidget(MakeToggleRow(tr("Ativar Specular GI (reflexões RT)"), &ReflectionsCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseReflections(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Reflexão: roughness máx (trace)"), 0.05, 1.0, 0.05, 2, 0.6, QString(), &ReflectionMaxRoughSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetReflectionMaxRoughness(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Reflexão: acumulação temporal"), &ReflectionTemporalCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetReflectionTemporal(Checked);
        }));

        // --- AO (GTAO): oclusao de contato em screen-space, aplicada ao indireto ---
        Layout->addWidget(MakeToggleRow(tr("Ativar AO de contato (GTAO)"), &AOCheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseAO(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("AO: raio (m)"), 0.05, 5.0, 0.05, 2, 0.6, QString(), &AORadiusSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetAORadius(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("AO: intensidade"), 0.0, 1.0, 0.05, 2, 1.0, QString(), &AOIntensitySpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetAOIntensity(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("AO: contraste (power)"), 0.5, 4.0, 0.1, 2, 2.0, QString(), &AOPowerSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetAOPower(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("AO: fade início (m)"), 1.0, 500.0, 5.0, 0, 50.0, QString(), &AOFadeStartSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetAOFadeStart(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("AO: fade fim (m)"), 1.0, 1000.0, 5.0, 0, 300.0, QString(), &AOFadeEndSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetAOFadeEnd(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeToggleRow(tr("Debug: ver oclusão (cinza)"), &AODebugCheck, false, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetAODebug(Checked);
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
        // TAA: anti-aliasing temporal (acumula a cor; mata o serrilhado e o shimmer da DDGI).
        // Mutuamente exclusivo com MSAA. Blend = fracao de history (mais alto = mais estavel/lento).
        Layout->addWidget(MakeToggleRow(tr("Anti-aliasing temporal (TAA)"), &TAACheck, true, [this](bool Checked) {
            if (RendererPtr) RendererPtr->SetUseTAA(Checked);
        }));
        Layout->addWidget(MakeNumericRow(tr("Acúmulo do TAA"), 0.50, 0.98, 0.01, 2, 0.90, QString(), &TAABlendSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetTAABlend(static_cast<float>(Value));
        }));
        // Fase 2: variance clipping (caixa γ·σ; ↑ = mais estável, menos tremor de borda, mais ghosting)
        // + sharpening (compensa a suavização do TAA).
        Layout->addWidget(MakeNumericRow(tr("Estabilidade TAA (variância)"), 0.50, 3.0, 0.05, 2, 1.25, QString(), &TAAVarianceSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetTAAVarianceGamma(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Nitidez do TAA"), 0.0, 1.0, 0.05, 2, 0.20, QString(), &TAASharpnessSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetTAASharpness(static_cast<float>(Value));
        }));
        // Acúmulo quando a câmera anda (↓ = menos blur de movimento). Resolve o "borrão ao andar".
        Layout->addWidget(MakeNumericRow(tr("Acúmulo em movimento"), 0.40, 0.95, 0.01, 2, 0.70, QString(), &TAAMotionSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetTAAMotionBlend(static_cast<float>(Value));
        }));
        // Anti-cintilamento: mata glint especular sub-pixel (janelas/postes). ↑ = menos cintila,
        // highlights mais chapados. 0 = off.
        Layout->addWidget(MakeNumericRow(tr("Anti-cintilamento (glint)"), 0.0, 1.0, 0.05, 2, 0.60, QString(), &TAAAntiFlickerSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetTAAAntiFlicker(static_cast<float>(Value));
        }));
        // Debug TAA: 0=off 1=offset reproj(px) 2=|atual-history| 3=dist clamp 4=peso history
        // 5=sigma 6=só history 7=só atual. (a saída passa pelo bloom/tonemap — leitura relativa)
        Layout->addWidget(MakeNumericRow(tr("Debug TAA (0=off)"), 0.0, 7.0, 1.0, 0, 0.0, QString(), &TAADebugSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetTAADebug(static_cast<unsigned>(Value + 0.5));
        }));
        // Heatmap de flicker (debug ao vivo): variância temporal por pixel. 0=off, 1=overlay
        // (calor sobre a cena), 2=puro. Escala = sensibilidade; Janela = suavização (frames).
        Layout->addWidget(MakeNumericRow(tr("Heatmap flicker (0/1/2)"), 0.0, 2.0, 1.0, 0, 0.0, QString(), &FlickerModeSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetFlickerMode(static_cast<unsigned>(Value + 0.5));
        }));
        Layout->addWidget(MakeNumericRow(tr("Heatmap: sensibilidade"), 0.02, 2.0, 0.02, 2, 0.30, QString(), &FlickerScaleSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetFlickerScale(static_cast<float>(Value));
        }));
        Layout->addWidget(MakeNumericRow(tr("Heatmap: janela (α)"), 0.02, 0.40, 0.01, 2, 0.08, QString(), &FlickerAlphaSpin, [this](double Value) {
            if (RendererPtr) RendererPtr->SetFlickerAlpha(static_cast<float>(Value));
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

        // Time-of-Day (F1): espelha o estado do relogio. Quando ligado, o sol e dirigido por ele
        // (azimute/elevacao manuais ficam desabilitados acima).
        {
            const Smile::FTimeOfDay& TOD = RendererPtr->GetTimeOfDay();
            if (TODCheck)         TODCheck->setChecked(TOD.Enabled);
            if (TODPauseCheck)    TODPauseCheck->setChecked(!TOD.Running);
            if (TODHourSpin)      TODHourSpin->setValue(TOD.TimeHours);
            if (TODDayLengthSpin) TODDayLengthSpin->setValue(TOD.DayLengthSec);
            if (TODLatitudeSpin)  TODLatitudeSpin->setValue(TOD.LatitudeDeg);
            if (TODDayOfYearSpin) TODDayOfYearSpin->setValue(TOD.DayOfYear);
            if (TODNorthSpin)     TODNorthSpin->setValue(TOD.NorthOffsetDeg);
            if (AzimuthSpin)      AzimuthSpin->setEnabled(!TOD.Enabled);
            if (ElevationSpin)    ElevationSpin->setEnabled(!TOD.Enabled);
            if (MoonCheck)         MoonCheck->setChecked(TOD.MoonEnabled);
            if (MoonIntensitySpin) MoonIntensitySpin->setValue(TOD.MoonIntensity);
            if (StarIntensitySpin) StarIntensitySpin->setValue(TOD.StarIntensity);
            if (MoonPhaseSpin)     MoonPhaseSpin->setValue(TOD.MoonPhaseOffsetHours);
            if (MoonSizeSpin)      MoonSizeSpin->setValue(TOD.MoonDiskSize);
        }
        if (AtmosphereCheck) AtmosphereCheck->setChecked(RendererPtr->GetUseAtmosphereSky());
        if (SunDiskSpin) SunDiskSpin->setValue(RendererPtr->GetSunDiskSize());
        if (GlareSpin) GlareSpin->setValue(RendererPtr->GetSunGlare());
        if (AtmosphereAmbientCheck) AtmosphereAmbientCheck->setChecked(RendererPtr->GetUseAtmosphereAmbient());
        if (AtmosphereAmbientSpin) AtmosphereAmbientSpin->setValue(RendererPtr->GetAtmosphereAmbientIntensity());

        if (ShadowsCheck) ShadowsCheck->setChecked(RendererPtr->GetUseSunShadows());
        if (ShadowDistanceSpin) ShadowDistanceSpin->setValue(RendererPtr->GetSunShadowMaxDistance());
        if (ShadowPenumbraSpin) ShadowPenumbraSpin->setValue(RendererPtr->GetSunShadowPenumbra());
        if (ShadowNormalOffsetSpin) ShadowNormalOffsetSpin->setValue(RendererPtr->GetSunShadowNormalOffset());
        if (ShadowBiasSpin) ShadowBiasSpin->setValue(RendererPtr->GetSunShadowDepthBias());
        if (ShadowBlendSpin) ShadowBlendSpin->setValue(RendererPtr->GetSunShadowBlendBand());
        if (ShadowDebugCheck) ShadowDebugCheck->setChecked(RendererPtr->GetSunShadowDebug());

        if (GICheck) GICheck->setChecked(RendererPtr->GetUseGI());
        if (GIIntensitySpin) GIIntensitySpin->setValue(RendererPtr->GetGIIntensity());
        if (GIChebyshevCheck) GIChebyshevCheck->setChecked(RendererPtr->GetGIChebyshev());
        if (GISkipInactiveCheck) GISkipInactiveCheck->setChecked(RendererPtr->GetGISkipInactiveProbes());
        if (GIDeactivThreshSpin) GIDeactivThreshSpin->setValue(RendererPtr->GetGIDeactivationThreshold());
        if (GIFallbackCheck) GIFallbackCheck->setChecked(RendererPtr->GetGISkipInactiveFallback());
        if (GIAdaptiveRaysCheck) GIAdaptiveRaysCheck->setChecked(RendererPtr->GetGIAdaptiveRays());
        if (GIMaxRaysSpin) GIMaxRaysSpin->setValue(RendererPtr->GetGIMaxRays());
        if (GIMinRaysSpin) GIMinRaysSpin->setValue(RendererPtr->GetGIMinRays());
        if (GIRealHitCheck) GIRealHitCheck->setChecked(RendererPtr->GetGIRealHitShading());
        if (GIRelocationCheck) GIRelocationCheck->setChecked(RendererPtr->GetGIRelocation());
        if (GIHysteresisSpin) GIHysteresisSpin->setValue(RendererPtr->GetGIHysteresis());
        if (GIDebugCheck) GIDebugCheck->setChecked(RendererPtr->GetGIDebug());
        if (GIDebugProbesCheck) GIDebugProbesCheck->setChecked(RendererPtr->GetGIDebugProbes());
        if (GIDebugModeCombo) GIDebugModeCombo->setCurrentIndex(static_cast<int>(RendererPtr->GetGIDebugMode()));
        if (GIDebugProbeSizeSpin) GIDebugProbeSizeSpin->setValue(RendererPtr->GetGIDebugProbeSize());
        if (GIDebugVolumeCheck) GIDebugVolumeCheck->setChecked(RendererPtr->GetGIDebugShowVolume());
        if (GIDebugRaysCheck) GIDebugRaysCheck->setChecked(RendererPtr->GetGIDebugShowRays());
        if (GIDebugRayRadiusSpin) GIDebugRayRadiusSpin->setValue(RendererPtr->GetGIDebugRayRadius());

        if (ReflectionsCheck) ReflectionsCheck->setChecked(RendererPtr->GetUseReflections());
        if (ReflectionMaxRoughSpin) ReflectionMaxRoughSpin->setValue(RendererPtr->GetReflectionMaxRoughness());
        if (ReflectionTemporalCheck) ReflectionTemporalCheck->setChecked(RendererPtr->GetReflectionTemporal());

        if (AOCheck) AOCheck->setChecked(RendererPtr->GetUseAO());
        if (AORadiusSpin) AORadiusSpin->setValue(RendererPtr->GetAORadius());
        if (AOIntensitySpin) AOIntensitySpin->setValue(RendererPtr->GetAOIntensity());
        if (AOPowerSpin) AOPowerSpin->setValue(RendererPtr->GetAOPower());
        if (AOFadeStartSpin) AOFadeStartSpin->setValue(RendererPtr->GetAOFadeStart());
        if (AOFadeEndSpin) AOFadeEndSpin->setValue(RendererPtr->GetAOFadeEnd());
        if (AODebugCheck) AODebugCheck->setChecked(RendererPtr->GetAODebug());

        if (CloudsCheck) CloudsCheck->setChecked(RendererPtr->GetUseClouds());
        if (CoverageSpin) CoverageSpin->setValue(RendererPtr->GetCloudCoverage());
        if (DensitySpin) DensitySpin->setValue(RendererPtr->GetCloudDensity());
        if (CloudAltitudeSpin) CloudAltitudeSpin->setValue(RendererPtr->GetCloudBottomAltitude());
        if (CloudThicknessSpin) CloudThicknessSpin->setValue(RendererPtr->GetCloudThickness());
        if (CloudWindSpin) CloudWindSpin->setValue(RendererPtr->GetCloudWind());
        if (PhaseSpin) PhaseSpin->setValue(RendererPtr->GetCloudPhaseG());
        if (PowderSpin) PowderSpin->setValue(RendererPtr->GetCloudPowder());
        if (ErosionSpin) ErosionSpin->setValue(RendererPtr->GetCloudErosion());

        if (AerialFogCheck) AerialFogCheck->setChecked(RendererPtr->GetUseAerialPerspective());
        if (HeightFogCheck) HeightFogCheck->setChecked(RendererPtr->GetUseHeightFog());
        auto& AtmFog = RendererPtr->GetFog();
        if (AtmFog.IsInitialized()) {
            if (AtmFogDensitySpin) AtmFogDensitySpin->setValue(AtmFog.GetDensity());
            if (AtmFogFalloffSpin) AtmFogFalloffSpin->setValue(AtmFog.GetHeightFalloff());
            if (AtmFogHeightSpin)  AtmFogHeightSpin->setValue(AtmFog.GetFogHeight());
            if (AtmFogOpacitySpin) AtmFogOpacitySpin->setValue(AtmFog.GetMaxOpacity());
        }

        auto& Water = RendererPtr->GetWater();
        if (OceanCheck) OceanCheck->setChecked(RendererPtr->GetUseWater());
        if (WaterDebugCombo) WaterDebugCombo->setCurrentIndex(static_cast<int>(Water.GetDebugMode()));
        if (QuadtreeCullCheck) QuadtreeCullCheck->setChecked(Water.GetGpuFrustumCull());
        if (QuadtreeBaseSpin) QuadtreeBaseSpin->setValue(Water.GetTileBaseSize());
        if (QuadtreeDepthSpin) QuadtreeDepthSpin->setValue(Water.GetTileMaxDepth());
        if (QuadtreeRingSpin) QuadtreeRingSpin->setValue(Water.GetGpuRingRadius());
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
        if (SSSStrengthSpin) SSSStrengthSpin->setValue(Water.GetSSSStrength());
        if (ShoreFoamSpin) ShoreFoamSpin->setValue(Water.GetShoreFoamIntensity());
        if (WaterClaritySpin) WaterClaritySpin->setValue(Water.GetWaterClarity());

        if (IBLIntensitySpin) IBLIntensitySpin->setValue(RendererPtr->GetIBLIntensity());
        if (IBLRotationSpin) IBLRotationSpin->setValue(RendererPtr->GetIBLRotation() / Smile::ToRad);
        if (ShowSkyboxCheck) ShowSkyboxCheck->setChecked(RendererPtr->GetShowSkybox());
        if (BloomIntensitySpin) BloomIntensitySpin->setValue(RendererPtr->GetBloomIntensity());
        if (ExposureSpin) ExposureSpin->setValue(RendererPtr->GetExposure());
        if (TAACheck) TAACheck->setChecked(RendererPtr->GetUseTAA());
        if (TAABlendSpin) TAABlendSpin->setValue(RendererPtr->GetTAABlend());
        if (TAAVarianceSpin) TAAVarianceSpin->setValue(RendererPtr->GetTAAVarianceGamma());
        if (TAASharpnessSpin) TAASharpnessSpin->setValue(RendererPtr->GetTAASharpness());
        if (TAAMotionSpin) TAAMotionSpin->setValue(RendererPtr->GetTAAMotionBlend());
        if (TAAAntiFlickerSpin) TAAAntiFlickerSpin->setValue(RendererPtr->GetTAAAntiFlicker());
        if (TAADebugSpin) TAADebugSpin->setValue(RendererPtr->GetTAADebug());
        if (FlickerModeSpin) FlickerModeSpin->setValue(RendererPtr->GetFlickerMode());
        if (FlickerScaleSpin) FlickerScaleSpin->setValue(RendererPtr->GetFlickerScale());
        if (FlickerAlphaSpin) FlickerAlphaSpin->setValue(RendererPtr->GetFlickerAlpha());
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

        if (ShadowsCheck) ShadowsCheck->setChecked(true);
        if (ShadowDistanceSpin) ShadowDistanceSpin->setValue(800.0);
        if (ShadowPenumbraSpin) ShadowPenumbraSpin->setValue(2.5);
        if (ShadowNormalOffsetSpin) ShadowNormalOffsetSpin->setValue(2.5);
        if (ShadowBiasSpin) ShadowBiasSpin->setValue(0.0006);
        if (ShadowBlendSpin) ShadowBlendSpin->setValue(0.1);
        if (ShadowDebugCheck) ShadowDebugCheck->setChecked(false);

        if (GICheck) GICheck->setChecked(true);
        if (GIIntensitySpin) GIIntensitySpin->setValue(1.0);
        if (GIChebyshevCheck) GIChebyshevCheck->setChecked(true);
        if (GISkipInactiveCheck) GISkipInactiveCheck->setChecked(true);
        if (GIDeactivThreshSpin) GIDeactivThreshSpin->setValue(0.20);
        if (GIFallbackCheck) GIFallbackCheck->setChecked(false);
        if (GIAdaptiveRaysCheck) GIAdaptiveRaysCheck->setChecked(false);
        if (GIMaxRaysSpin) GIMaxRaysSpin->setValue(64);
        if (GIMinRaysSpin) GIMinRaysSpin->setValue(16);
        if (GIDebugCheck) GIDebugCheck->setChecked(false);
        if (GIDebugProbesCheck) GIDebugProbesCheck->setChecked(false);
        if (GIDebugModeCombo) GIDebugModeCombo->setCurrentIndex(0);
        if (GIDebugProbeSizeSpin) GIDebugProbeSizeSpin->setValue(0.10);
        if (GIDebugVolumeCheck) GIDebugVolumeCheck->setChecked(true);
        if (GIDebugRaysCheck) GIDebugRaysCheck->setChecked(false);
        if (GIDebugRayRadiusSpin) GIDebugRayRadiusSpin->setValue(6.0);

        if (AOCheck) AOCheck->setChecked(true);
        if (AORadiusSpin) AORadiusSpin->setValue(0.6);
        if (AOIntensitySpin) AOIntensitySpin->setValue(1.0);
        if (AOPowerSpin) AOPowerSpin->setValue(2.0);
        if (AOFadeStartSpin) AOFadeStartSpin->setValue(50.0);
        if (AOFadeEndSpin) AOFadeEndSpin->setValue(300.0);
        if (AODebugCheck) AODebugCheck->setChecked(false);

        if (CloudsCheck) CloudsCheck->setChecked(true);
        if (CoverageSpin) CoverageSpin->setValue(0.45);
        if (DensitySpin) DensitySpin->setValue(1.6);
        if (CloudAltitudeSpin) CloudAltitudeSpin->setValue(2.0);
        if (CloudThicknessSpin) CloudThicknessSpin->setValue(3.0);
        if (CloudWindSpin) CloudWindSpin->setValue(0.01);
        if (PhaseSpin) PhaseSpin->setValue(0.8);
        if (PowderSpin) PowderSpin->setValue(0.5);
        if (ErosionSpin) ErosionSpin->setValue(0.45);

        if (AerialFogCheck) AerialFogCheck->setChecked(true);
        if (HeightFogCheck) HeightFogCheck->setChecked(true);
        if (AtmFogDensitySpin) AtmFogDensitySpin->setValue(0.0002);
        if (AtmFogFalloffSpin) AtmFogFalloffSpin->setValue(0.0005);
        if (AtmFogHeightSpin)  AtmFogHeightSpin->setValue(0.0);
        if (AtmFogOpacitySpin) AtmFogOpacitySpin->setValue(0.85);
        if (AtmFogStartSpin)   AtmFogStartSpin->setValue(0.0);

        if (OceanCheck) OceanCheck->setChecked(true);
        if (WaterDebugCombo) WaterDebugCombo->setCurrentIndex(0);
        if (QuadtreeCullCheck) QuadtreeCullCheck->setChecked(true);
        if (QuadtreeBaseSpin) QuadtreeBaseSpin->setValue(64.0);
        if (QuadtreeDepthSpin) QuadtreeDepthSpin->setValue(9.0);
        if (QuadtreeRingSpin) QuadtreeRingSpin->setValue(8.0);
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
        if (TAACheck) TAACheck->setChecked(true);
        if (TAABlendSpin) TAABlendSpin->setValue(0.90);
        if (TAAVarianceSpin) TAAVarianceSpin->setValue(1.25);
        if (TAASharpnessSpin) TAASharpnessSpin->setValue(0.20);
        if (TAAMotionSpin) TAAMotionSpin->setValue(0.70);
        if (TAAAntiFlickerSpin) TAAAntiFlickerSpin->setValue(0.60);
        if (TAADebugSpin) TAADebugSpin->setValue(0);
        if (FlickerModeSpin) FlickerModeSpin->setValue(0);
        if (FlickerScaleSpin) FlickerScaleSpin->setValue(0.30);
        if (FlickerAlphaSpin) FlickerAlphaSpin->setValue(0.08);
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
