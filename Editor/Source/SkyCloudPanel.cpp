#include "SmileEditor/SkyCloudPanel.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Math/Math.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>

namespace SmileEditor {
    namespace {
        QDoubleSpinBox* MakeSpin(QWidget* Parent, double Lo, double Hi, double Step,
                                 int Dec, double Val, const QString& Suffix = QString()) {
            auto* S = new QDoubleSpinBox(Parent);
            S->setRange(Lo, Hi);
            S->setSingleStep(Step);
            S->setDecimals(Dec);
            S->setValue(Val);
            if (!Suffix.isEmpty()) S->setSuffix(Suffix);
            return S;
        }
    }

    SkyCloudPanel::SkyCloudPanel(QWidget* _Parent) : QWidget(_Parent) {
        setMinimumWidth(240);
        setMaximumWidth(320);
        BuildUI();
    }

    void SkyCloudPanel::BuildUI() {
        auto* RootLayout = new QVBoxLayout(this);
        RootLayout->setContentsMargins(0, 0, 0, 0);
        RootLayout->setSpacing(0);

        auto* Header = new QLabel(tr("Céu & Nuvens"), this);
        Header->setObjectName("MaterialHeader");
        RootLayout->addWidget(Header);

        auto* Container = new QWidget(this);
        auto* Layout    = new QVBoxLayout(Container);
        Layout->setContentsMargins(6, 6, 6, 6);
        Layout->setSpacing(6);

        // --- Sol ---
        auto* SunGroup  = new QGroupBox(tr("Sol"), Container);
        auto* SunForm   = new QFormLayout(SunGroup);
        SunForm->setContentsMargins(4, 8, 4, 4);
        SunForm->setVerticalSpacing(4);
        AzimuthSpin      = MakeSpin(SunGroup, -180.0, 180.0, 5.0, 1, 0.0, QStringLiteral("°"));
        ElevationSpin    = MakeSpin(SunGroup, -5.0, 90.0, 1.0, 1, 45.0, QStringLiteral("°"));
        SunIntensitySpin = MakeSpin(SunGroup, 0.0, 10.0, 0.1, 2, 2.5);
        SunForm->addRow(tr("Azimute"),    AzimuthSpin);
        SunForm->addRow(tr("Elevação"),   ElevationSpin);
        SunForm->addRow(tr("Intensidade"), SunIntensitySpin);
        Layout->addWidget(SunGroup);

        // --- Céu (atmosfera) ---
        auto* SkyGroup = new QGroupBox(tr("Céu (atmosfera)"), Container);
        auto* SkyForm  = new QFormLayout(SkyGroup);
        SkyForm->setContentsMargins(4, 8, 4, 4);
        SkyForm->setVerticalSpacing(4);
        AtmosphereCheck = new QCheckBox(tr("Atmosfera física"), SkyGroup);
        AtmosphereCheck->setChecked(true);
        SkyForm->addRow(QString(), AtmosphereCheck);
        SunDiskSpin = MakeSpin(SkyGroup, 0.1, 3.0, 0.1, 2, 0.7, QStringLiteral("°"));
        GlareSpin   = MakeSpin(SkyGroup, 0.0, 12.0, 0.5, 1, 4.0);
        SkyForm->addRow(tr("Disco do sol"), SunDiskSpin);
        SkyForm->addRow(tr("Glare"),        GlareSpin);
        AtmoAmbientCheck = new QCheckBox(tr("Ambiente da atmosfera"), SkyGroup);
        AtmoAmbientCheck->setChecked(true);
        SkyForm->addRow(QString(), AtmoAmbientCheck);
        AtmoAmbientSpin = MakeSpin(SkyGroup, 0.0, 4.0, 0.1, 2, 1.0);
        SkyForm->addRow(tr("Intens. ambiente"), AtmoAmbientSpin);
        Layout->addWidget(SkyGroup);

        // --- Nuvens ---
        auto* CloudGroup = new QGroupBox(tr("Nuvens"), Container);
        auto* CloudForm  = new QFormLayout(CloudGroup);
        CloudForm->setContentsMargins(4, 8, 4, 4);
        CloudForm->setVerticalSpacing(4);
        CloudsCheck = new QCheckBox(tr("Nuvens volumétricas"), CloudGroup);
        CloudsCheck->setChecked(true);
        CloudForm->addRow(QString(), CloudsCheck);
        CoverageSpin  = MakeSpin(CloudGroup, 0.0, 1.0, 0.02, 2, 0.45);
        DensitySpin   = MakeSpin(CloudGroup, 0.1, 4.0, 0.1, 2, 1.6);
        AltBottomSpin = MakeSpin(CloudGroup, 0.5, 6.0, 0.25, 2, 2.0, QStringLiteral(" km"));
        ThicknessSpin = MakeSpin(CloudGroup, 0.5, 5.0, 0.25, 2, 3.0, QStringLiteral(" km"));
        WindSpin      = MakeSpin(CloudGroup, 0.0, 0.1, 0.005, 3, 0.01);
        PhaseSpin     = MakeSpin(CloudGroup, 0.0, 0.95, 0.05, 2, 0.8);
        PowderSpin    = MakeSpin(CloudGroup, 0.0, 1.0, 0.05, 2, 0.5);
        ErosionSpin   = MakeSpin(CloudGroup, 0.0, 1.0, 0.05, 2, 0.45);
        CloudForm->addRow(tr("Cobertura"),  CoverageSpin);
        CloudForm->addRow(tr("Densidade"),  DensitySpin);
        CloudForm->addRow(tr("Altitude"),   AltBottomSpin);
        CloudForm->addRow(tr("Espessura"),  ThicknessSpin);
        CloudForm->addRow(tr("Vento"),      WindSpin);
        CloudForm->addRow(tr("Fase (fwd)"), PhaseSpin);
        CloudForm->addRow(tr("Powder"),     PowderSpin);
        CloudForm->addRow(tr("Erosão"),     ErosionSpin);
        Layout->addWidget(CloudGroup);

        Layout->addStretch();
        RootLayout->addWidget(Container);

        // --- Connections ---
        auto SpinSig = static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged);
        connect(AzimuthSpin,      SpinSig, this, &SkyCloudPanel::OnSunChanged);
        connect(ElevationSpin,    SpinSig, this, &SkyCloudPanel::OnSunChanged);
        connect(SunIntensitySpin, SpinSig, this, &SkyCloudPanel::OnSunIntensityChanged);
        connect(AtmosphereCheck, &QCheckBox::toggled, this, &SkyCloudPanel::OnAtmosphereToggled);
        connect(SunDiskSpin,      SpinSig, this, &SkyCloudPanel::OnSunDiskChanged);
        connect(GlareSpin,        SpinSig, this, &SkyCloudPanel::OnGlareChanged);
        connect(AtmoAmbientCheck, &QCheckBox::toggled, this, &SkyCloudPanel::OnAtmoAmbientToggled);
        connect(AtmoAmbientSpin,  SpinSig, this, &SkyCloudPanel::OnAtmoAmbientChanged);
        connect(CloudsCheck, &QCheckBox::toggled, this, &SkyCloudPanel::OnCloudsToggled);
        connect(CoverageSpin,  SpinSig, this, &SkyCloudPanel::OnCoverageChanged);
        connect(DensitySpin,   SpinSig, this, &SkyCloudPanel::OnDensityChanged);
        connect(AltBottomSpin, SpinSig, this, &SkyCloudPanel::OnCloudAltitudeChanged);
        connect(ThicknessSpin, SpinSig, this, &SkyCloudPanel::OnCloudAltitudeChanged);
        connect(WindSpin,    SpinSig, this, &SkyCloudPanel::OnWindChanged);
        connect(PhaseSpin,   SpinSig, this, &SkyCloudPanel::OnPhaseChanged);
        connect(PowderSpin,  SpinSig, this, &SkyCloudPanel::OnPowderChanged);
        connect(ErosionSpin, SpinSig, this, &SkyCloudPanel::OnErosionChanged);
    }

    void SkyCloudPanel::InitializeWithRenderer(Smile::Renderer* _Renderer) {
        RendererPtr = _Renderer;
        if (!RendererPtr) return;

        // Sync sun azimuth/elevation from the renderer's current direction.
        Smile::Vec3 d = RendererPtr->GetSunDirection();
        const double ElDeg = std::asin(std::max(-1.0f, std::min(1.0f, d.Y))) / Smile::ToRad;
        const double AzDeg = std::atan2(d.X, d.Z) / Smile::ToRad;

        const QSignalBlocker b1(AzimuthSpin), b2(ElevationSpin), b3(SunIntensitySpin),
                             b4(AtmosphereCheck), b5(CloudsCheck);
        AzimuthSpin->setValue(AzDeg);
        ElevationSpin->setValue(ElDeg);
        SunIntensitySpin->setValue(RendererPtr->GetSunIntensity());
        AtmosphereCheck->setChecked(RendererPtr->GetUseAtmosphereSky());
        CloudsCheck->setChecked(RendererPtr->GetUseClouds());
        // Remaining controls keep their BuildUI defaults, which match the engine
        // defaults — no apply needed.
    }

    void SkyCloudPanel::OnSunChanged() {
        if (RendererPtr)
            RendererPtr->SetSunAzimuthElevation(static_cast<float>(AzimuthSpin->value()),
                                                static_cast<float>(ElevationSpin->value()));
    }
    void SkyCloudPanel::OnSunIntensityChanged(double V) {
        if (RendererPtr) RendererPtr->SetSunIntensity(static_cast<float>(V));
    }
    void SkyCloudPanel::OnAtmosphereToggled(bool C) {
        if (RendererPtr) RendererPtr->SetUseAtmosphereSky(C);
    }
    void SkyCloudPanel::OnSunDiskChanged(double V) {
        if (RendererPtr) RendererPtr->SetSunDiskSize(static_cast<float>(V));
    }
    void SkyCloudPanel::OnGlareChanged(double V) {
        if (RendererPtr) RendererPtr->SetSunGlare(static_cast<float>(V));
    }
    void SkyCloudPanel::OnAtmoAmbientToggled(bool C) {
        if (RendererPtr) RendererPtr->SetUseAtmosphereAmbient(C);
    }
    void SkyCloudPanel::OnAtmoAmbientChanged(double V) {
        if (RendererPtr) RendererPtr->SetAtmosphereAmbientIntensity(static_cast<float>(V));
    }
    void SkyCloudPanel::OnCloudsToggled(bool C) {
        if (RendererPtr) RendererPtr->SetUseClouds(C);
    }
    void SkyCloudPanel::OnCoverageChanged(double V) {
        if (RendererPtr) RendererPtr->SetCloudCoverage(static_cast<float>(V));
    }
    void SkyCloudPanel::OnDensityChanged(double V) {
        if (RendererPtr) RendererPtr->SetCloudDensity(static_cast<float>(V));
    }
    void SkyCloudPanel::OnCloudAltitudeChanged() {
        if (RendererPtr)
            RendererPtr->SetCloudAltitude(static_cast<float>(AltBottomSpin->value()),
                                          static_cast<float>(ThicknessSpin->value()));
    }
    void SkyCloudPanel::OnWindChanged(double V) {
        if (RendererPtr) RendererPtr->SetCloudWind(static_cast<float>(V));
    }
    void SkyCloudPanel::OnPhaseChanged(double V) {
        if (RendererPtr) RendererPtr->SetCloudPhaseG(static_cast<float>(V));
    }
    void SkyCloudPanel::OnPowderChanged(double V) {
        if (RendererPtr) RendererPtr->SetCloudPowder(static_cast<float>(V));
    }
    void SkyCloudPanel::OnErosionChanged(double V) {
        if (RendererPtr) RendererPtr->SetCloudErosion(static_cast<float>(V));
    }
}
