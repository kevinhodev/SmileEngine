#include "SmileEditor/MaterialEditorPanel.h"
#include "SmileEditor/TextureSlotWidget.h"
#include "Smile/Graphics/Renderer.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QColorDialog>
#include <QFileInfo>

namespace SmileEditor {
    static const struct { const char* Name; } kSlots[Smile::kMaterialTextureSlots] = {
        { "Albedo"              },
        { "Normal Map"          },
        { "Metallic/Roughness"  },
        { "Oclusão Ambiente"    },
        { "Emissivo"            },
        { "Mapa de Altura"      },
        { "Metalness"           },
        { "Roughness"           },
    };

    MaterialEditorPanel::MaterialEditorPanel(QWidget* _Parent)
        : QWidget(_Parent)
    {
        setMinimumWidth(240);
        setMaximumWidth(320);
        BuildUI();
    }

    void MaterialEditorPanel::BuildUI() {
        auto* RootLayout = new QVBoxLayout(this);
        RootLayout->setContentsMargins(0, 0, 0, 0);
        RootLayout->setSpacing(0);

        auto* Header = new QLabel(tr("Material"), this);
        Header->setObjectName("MaterialHeader");
        RootLayout->addWidget(Header);

        auto* ScrollArea = new QScrollArea(this);
        ScrollArea->setObjectName("MaterialScrollArea");
        ScrollArea->setWidgetResizable(true);
        ScrollArea->setFrameShape(QFrame::NoFrame);

        auto* Inner  = new QWidget(ScrollArea);
        auto* Layout = new QVBoxLayout(Inner);
        Layout->setContentsMargins(6, 6, 6, 6);
        Layout->setSpacing(6);

        Layout->addWidget(BuildTexturesSection());
        Layout->addWidget(BuildParametersSection());
        Layout->addWidget(BuildParallaxSection());
        Layout->addStretch();

        ScrollArea->setWidget(Inner);
        RootLayout->addWidget(ScrollArea);
    }

    QWidget* MaterialEditorPanel::BuildTexturesSection() {
        auto* TexturesGroup = new QGroupBox(tr("Texturas"), this);
        TexturesGroup->setObjectName("MaterialTexturesGroup");

        auto* Layout = new QVBoxLayout(TexturesGroup);
        Layout->setContentsMargins(4, 8, 4, 4);
        Layout->setSpacing(4);

        for (int i = 0; i < static_cast<int>(Smile::kMaterialTextureSlots); ++i) {
            auto* TextureSlot = new TextureSlotWidget(tr(kSlots[i].Name), TexturesGroup);
            SlotWidgets[i] = TextureSlot;
            Layout->addWidget(TextureSlot);

            connect(TextureSlot, &TextureSlotWidget::BrowseRequested, this, [this, i]{ OnBrowseSlot(i); });
            connect(TextureSlot, &TextureSlotWidget::ClearRequested,  this, [this, i]{ OnClearSlot(i); });
        }

        return TexturesGroup;
    }

    QWidget* MaterialEditorPanel::BuildParametersSection() {
        auto* Group = new QGroupBox(tr("Parâmetros PBR"), this);
        Group->setObjectName("MaterialParametersGroup");

        auto* FormLayout = new QFormLayout(Group);
        FormLayout->setContentsMargins(4, 8, 4, 4);
        FormLayout->setSpacing(4);
        FormLayout->setLabelAlignment(Qt::AlignRight);

        auto MakeLabel = [&](const char* _Text) -> QLabel* {
            auto* Label = new QLabel(tr(_Text), Group);
            Label->setObjectName("MaterialParamLabel");
            return Label;
        };

        auto MakeSpinbox = [&](double _Min, double _Max, double _Step, double _Value) -> QDoubleSpinBox* {
            auto* s = new QDoubleSpinBox(Group);
            s->setObjectName("MaterialSpinBox");
            s->setRange(_Min, _Max);
            s->setSingleStep(_Step);
            s->setValue(_Value);
            s->setDecimals(3);
            s->setFixedWidth(72);
            return s;
        };

        BaseColorBtn = new QPushButton(Group);
        BaseColorBtn->setObjectName("MaterialColorBtn");
        BaseColorBtn->setFixedHeight(22);
        UpdateColorButton(BaseColorBtn, BaseColorValue);
        FormLayout->addRow(MakeLabel("Cor Base"), BaseColorBtn);
        connect(BaseColorBtn, &QPushButton::clicked, this, &MaterialEditorPanel::OnPickBaseColor);

        MetallicSpin = MakeSpinbox(0.0, 1.0, 0.01, 0.0);
        FormLayout->addRow(MakeLabel("Metálico"), MetallicSpin);
        connect(MetallicSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnMetallicChanged);

        RoughnessSpin = MakeSpinbox(0.0, 1.0, 0.01, 0.5);
        FormLayout->addRow(MakeLabel("Rugosidade"), RoughnessSpin);
        connect(RoughnessSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnRoughnessChanged);

        AOStrengthSpin = MakeSpinbox(0.0, 1.0, 0.01, 1.0);
        FormLayout->addRow(MakeLabel("Força AO"), AOStrengthSpin);
        connect(AOStrengthSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnAOStrengthChanged);

        NormalStrSpin = MakeSpinbox(0.0, 3.0, 0.05, 1.0);
        FormLayout->addRow(MakeLabel("Força Normal"), NormalStrSpin);
        connect(NormalStrSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnNormalStrengthChanged);

        // Padrão = desmarcado = normal map OpenGL/GL (verde invertido p/ D3D).
        // Marque apenas se o map for DirectX (verde p/ baixo), que dispensa o flip.
        NormalFlipYCheck = new QCheckBox(tr("Normal map é DirectX"), Group);
        NormalFlipYCheck->setObjectName("MaterialDirectXCheck");
        FormLayout->addRow(NormalFlipYCheck);
        connect(NormalFlipYCheck, &QCheckBox::toggled, this, &MaterialEditorPanel::OnNormalFlipYChanged);



        auto* Separator = new QFrame(Group);
        Separator->setObjectName("MaterialSeparator");
        Separator->setFrameShape(QFrame::HLine);
        FormLayout->addRow(Separator);

        EmissiveColorBtn = new QPushButton(Group);
        EmissiveColorBtn->setObjectName("MaterialColorBtn");
        EmissiveColorBtn->setFixedHeight(22);
        UpdateColorButton(EmissiveColorBtn, EmissiveColorValue);
        FormLayout->addRow(MakeLabel("Emissivo"), EmissiveColorBtn);
        connect(EmissiveColorBtn, &QPushButton::clicked, this, &MaterialEditorPanel::OnPickEmissiveColor);

        EmissiveStrSpin = MakeSpinbox(0.0, 20.0, 0.1, 1.0);
        FormLayout->addRow(MakeLabel("Intensidade"), EmissiveStrSpin);
        connect(EmissiveStrSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnEmissiveStrengthChanged);

        return Group;
    }

    QWidget* MaterialEditorPanel::BuildParallaxSection() {
        auto* Group = new QGroupBox(tr("Parallax (POM)"), this);
        Group->setObjectName("MaterialParametersGroup");

        auto* FormLayout = new QFormLayout(Group);
        FormLayout->setContentsMargins(4, 8, 4, 4);
        FormLayout->setSpacing(4);
        FormLayout->setLabelAlignment(Qt::AlignRight);

        auto MakeLabel = [&](const char* _Text) -> QLabel* {
            auto* Label = new QLabel(tr(_Text), Group);
            Label->setObjectName("MaterialParamLabel");
            return Label;
        };
        auto MakeSpinbox = [&](double _Min, double _Max, double _Step, double _Value, int _Decimals) -> QDoubleSpinBox* {
            auto* s = new QDoubleSpinBox(Group);
            s->setObjectName("MaterialSpinBox");
            s->setRange(_Min, _Max);
            s->setSingleStep(_Step);
            s->setValue(_Value);
            s->setDecimals(_Decimals);
            s->setFixedWidth(72);
            return s;
        };

        HeightScaleSpin = MakeSpinbox(0.0, 0.5, 0.005, Material.Constants.HeightScale, 3);
        FormLayout->addRow(MakeLabel("Escala"), HeightScaleSpin);
        connect(HeightScaleSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnHeightScaleChanged);

        ParallaxMinSpin = MakeSpinbox(1.0, 256.0, 1.0, Material.Constants.ParallaxMinSteps, 0);
        FormLayout->addRow(MakeLabel("Passos mín."), ParallaxMinSpin);
        connect(ParallaxMinSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnParallaxMinStepsChanged);

        ParallaxMaxSpin = MakeSpinbox(1.0, 256.0, 1.0, Material.Constants.ParallaxMaxSteps, 0);
        FormLayout->addRow(MakeLabel("Passos máx."), ParallaxMaxSpin);
        connect(ParallaxMaxSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnParallaxMaxStepsChanged);

        ParallaxSelfShadowChk = new QCheckBox(tr("Auto-sombra"), Group);
        ParallaxSelfShadowChk->setObjectName("MaterialDirectXCheck");
        FormLayout->addRow(ParallaxSelfShadowChk);
        connect(ParallaxSelfShadowChk, &QCheckBox::toggled, this, &MaterialEditorPanel::OnParallaxSelfShadowChanged);

        ParallaxShadowStepsSpin = MakeSpinbox(1.0, 128.0, 1.0, Material.Constants.ParallaxShadowSteps, 0);
        FormLayout->addRow(MakeLabel("Passos sombra"), ParallaxShadowStepsSpin);
        connect(ParallaxShadowStepsSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnParallaxShadowStepsChanged);

        ParallaxFadeStartSpin = MakeSpinbox(0.0, 12.0, 0.5, Material.Constants.ParallaxFadeStart, 1);
        FormLayout->addRow(MakeLabel("Fade início"), ParallaxFadeStartSpin);
        connect(ParallaxFadeStartSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnParallaxFadeStartChanged);

        ParallaxFadeRangeSpin = MakeSpinbox(0.5, 12.0, 0.5, Material.Constants.ParallaxFadeRange, 1);
        FormLayout->addRow(MakeLabel("Fade alcance"), ParallaxFadeRangeSpin);
        connect(ParallaxFadeRangeSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnParallaxFadeRangeChanged);

        ParallaxRefineChk = new QCheckBox(tr("Refino (busca binária)"), Group);
        ParallaxRefineChk->setObjectName("MaterialDirectXCheck");
        FormLayout->addRow(ParallaxRefineChk);
        connect(ParallaxRefineChk, &QCheckBox::toggled, this, &MaterialEditorPanel::OnParallaxRefineChanged);

        ParallaxRefineStepsSpin = MakeSpinbox(1.0, 8.0, 1.0, Material.Constants.ParallaxRefineSteps, 0);
        ParallaxRefineStepsSpin->setEnabled(ParallaxRefineChk->isChecked());
        FormLayout->addRow(MakeLabel("Iterações refino"), ParallaxRefineStepsSpin);
        connect(ParallaxRefineStepsSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnParallaxRefineStepsChanged);

        return Group;
    }

    Smile::EDefaultTexture MaterialEditorPanel::FallbackTypeForSlot(int _Slot) {
        using E = Smile::EDefaultTexture;
        switch (_Slot) {
            case 0: return E::White;       // Albedo
            case 1: return E::FlatNormal;  // Normal
            case 2: return E::ORM;         // MetallicRoughness
            case 3: return E::White;       // AO
            case 4: return E::Black;       // Emissive
            case 5: return E::White;       // Height (1.0 = surface top → zero parallax)
            case 6: return E::White;       // Metalness (1.0 = identidade multiplicativa)
            case 7: return E::White;       // Roughness  (1.0 = identidade multiplicativa)
            default: return E::White;
        }
    }

    void MaterialEditorPanel::InitializeWithRenderer(Smile::Renderer* _Renderer) {
        if (RendererPtr) return; 
        RendererPtr = _Renderer;

        auto* Device = _Renderer->GetDevice().Native();
        auto& CommandQueue  = _Renderer->GetCmdQueue();
        auto& SRVHeap = _Renderer->GetSRVHeap();

        for (int i = 0; i < static_cast<int>(Smile::kMaterialTextureSlots); ++i)
            FallbackTexture[i] = Smile::FTexture::CreateDefault(Device, CommandQueue, SRVHeap, FallbackTypeForSlot(i));

        Material.Albedo            = &FallbackTexture[0];
        Material.Normal            = &FallbackTexture[1];
        Material.MetallicRoughness = &FallbackTexture[2];
        Material.AO                = &FallbackTexture[3];
        Material.Emissive          = &FallbackTexture[4];
        Material.Height            = &FallbackTexture[5];
        Material.Metalness         = &FallbackTexture[6];
        Material.Roughness         = &FallbackTexture[7];

        Material.Constants.BaseColorFactor  = {
            BaseColorValue.redF(),
            BaseColorValue.greenF(),
            BaseColorValue.blueF(),
            1.0f
        };
        Material.Constants.MetallicFactor   = static_cast<float>(MetallicSpin->value());
        Material.Constants.RoughnessFactor  = static_cast<float>(RoughnessSpin->value());
        Material.Constants.AOStrength       = static_cast<float>(AOStrengthSpin->value());
        Material.Constants.NormalStrength   = static_cast<float>(NormalStrSpin->value());

        Material.Constants.EmissiveFactor   = {
            EmissiveColorValue.redF(),
            EmissiveColorValue.greenF(),
            EmissiveColorValue.blueF(),
            1.0f
        };
        Material.Constants.EmissiveStrength = static_cast<float>(EmissiveStrSpin->value());

        Material.Finalize(Device, SRVHeap);
        _Renderer->SetMaterial(&Material);
    }

    void MaterialEditorPanel::SetSlotPointer(int _Slot, Smile::FTexture* _Texture) {
        switch (_Slot) {
            case 0: Material.Albedo            = _Texture; break;
            case 1: Material.Normal            = _Texture; break;
            case 2: Material.MetallicRoughness = _Texture; break;
            case 3: Material.AO                = _Texture; break;
            case 4: Material.Emissive          = _Texture; break;
            case 5: Material.Height            = _Texture; break;
            case 6: Material.Metalness         = _Texture; break;
            case 7: Material.Roughness         = _Texture; break;
        }
    }

    void MaterialEditorPanel::UpdateHasFlag(int _Slot, bool _Has) {
        const Smile::u32 Value = _Has ? 1u : 0u;
        switch (_Slot) {
            case 0: Material.Constants.HasAlbedoMap            = Value; break;
            case 1: Material.Constants.HasNormalMap            = Value; break;
            case 2: Material.Constants.HasMetallicRoughnessMap = Value; break;
            case 3: Material.Constants.HasAOMap                = Value; break;
            case 4: Material.Constants.HasEmissiveMap          = Value; break;
            case 5: Material.Constants.HasHeightMap            = Value; break;
            case 6: Material.Constants.HasMetalnessMap         = Value; break;
            case 7: Material.Constants.HasRoughnessMap         = Value; break;
        }
    }

    void MaterialEditorPanel::ApplyTextureToSlot(int _Slot, const QString& _Path) {
        if (!RendererPtr) return;

        auto* Device = RendererPtr->GetDevice().Native();
        auto& CommandQueue  = RendererPtr->GetCmdQueue();
        auto& SRVHeap = RendererPtr->GetSRVHeap();

        const std::wstring wPath = _Path.toStdWString();
        const bool IsNormalMap = (_Slot == 1); // slot index 1 = Normal Map (see kSlots)
        SlotTexture[_Slot] = Smile::FTexture::LoadFromFile(Device, CommandQueue, SRVHeap, wPath, IsNormalMap);

        SetSlotPointer(_Slot, &SlotTexture[_Slot].value());
        Material.UpdateTextureSlot(Device, SRVHeap, static_cast<Smile::u32>(_Slot), &SlotTexture[_Slot].value());
        UpdateHasFlag(_Slot, true);
        Material.UpdateConstants();

        if (SlotWidgets[_Slot]) {
            SlotWidgets[_Slot]->SetThumbnail(QPixmap(_Path));
            SlotWidgets[_Slot]->SetPath(_Path);
        }
    }

    void MaterialEditorPanel::ClearSlot(int _Slot) {
        if (!RendererPtr) return;

        auto* Device = RendererPtr->GetDevice().Native();
        auto& SRVHeap = RendererPtr->GetSRVHeap();

        SlotTexture[_Slot] = std::nullopt;
        SetSlotPointer(_Slot, &FallbackTexture[_Slot]);
        Material.UpdateTextureSlot(Device, SRVHeap, static_cast<Smile::u32>(_Slot), &FallbackTexture[_Slot]);
        UpdateHasFlag(_Slot, false);
        Material.UpdateConstants();

        if (SlotWidgets[_Slot]) {
            SlotWidgets[_Slot]->ClearThumbnail();
            SlotWidgets[_Slot]->SetPath(QString());
        }
    }

    void MaterialEditorPanel::OnBrowseSlot(int _Slot) {
        const QString Path = QFileDialog::getOpenFileName(
            this,
            tr("Selecionar Textura — %1").arg(tr(kSlots[_Slot].Name)),
            QString(),
            tr("Imagens (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;Todos os arquivos (*)"));
        if (!Path.isEmpty())
            ApplyTextureToSlot(_Slot, Path);
    }

    void MaterialEditorPanel::OnClearSlot(int _Slot) {
        ClearSlot(_Slot);
    }

    void MaterialEditorPanel::UpdateColorButton(QPushButton* _Button, const QColor& _Color) {
        _Button->setStyleSheet(QStringLiteral("background-color: %1;").arg(_Color.name()));
    }

    void MaterialEditorPanel::OnPickBaseColor() {
        const QColor Color = QColorDialog::getColor(BaseColorValue, this, tr("Cor Base"));
        if (!Color.isValid()) return;
        BaseColorValue = Color;
        UpdateColorButton(BaseColorBtn, Color);
        if (!RendererPtr) return;
        Material.Constants.BaseColorFactor = {
            static_cast<Smile::f32>(Color.redF()),
            static_cast<Smile::f32>(Color.greenF()),
            static_cast<Smile::f32>(Color.blueF()),
            1.0f
        };
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnPickEmissiveColor() {
        const QColor Color = QColorDialog::getColor(EmissiveColorValue, this, tr("Cor Emissiva"));
        if (!Color.isValid()) return;
        EmissiveColorValue = Color;
        UpdateColorButton(EmissiveColorBtn, Color);
        if (!RendererPtr) return;
        Material.Constants.EmissiveFactor = {
            static_cast<Smile::f32>(Color.redF()),
            static_cast<Smile::f32>(Color.greenF()),
            static_cast<Smile::f32>(Color.blueF()),
            1.0f
        };
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnMetallicChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.MetallicFactor = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnRoughnessChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.RoughnessFactor = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnAOStrengthChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.AOStrength = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnNormalStrengthChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.NormalStrength = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }



    void MaterialEditorPanel::OnEmissiveStrengthChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.EmissiveStrength = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnNormalFlipYChanged(bool _Checked) {
        if (!RendererPtr) return;
        Material.Constants.NormalFlipY = _Checked ? 1u : 0u;
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnHeightScaleChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.HeightScale = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxMinStepsChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxMinSteps = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxMaxStepsChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxMaxSteps = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxSelfShadowChanged(bool _Checked) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxSelfShadow = _Checked ? 1u : 0u;
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxShadowStepsChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxShadowSteps = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxFadeStartChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxFadeStart = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxFadeRangeChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxFadeRange = static_cast<Smile::f32>(_Value);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxRefineChanged(bool _Checked) {
        if (ParallaxRefineStepsSpin)
            ParallaxRefineStepsSpin->setEnabled(_Checked);
        if (!RendererPtr) return;
        Material.Constants.ParallaxRefine = _Checked ? 1u : 0u;
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnParallaxRefineStepsChanged(double _Value) {
        if (!RendererPtr) return;
        Material.Constants.ParallaxRefineSteps = static_cast<Smile::u32>(_Value);
        Material.UpdateConstants();
    }
}
