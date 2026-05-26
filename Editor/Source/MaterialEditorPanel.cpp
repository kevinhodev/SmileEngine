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
        { "Height/Deslocamento" },
        { "Emissivo"            },
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
        Header->setStyleSheet(
            "background: #252526; color: #dcdcdc; font-weight: bold; font-size: 12px;"
            "padding: 6px 10px; border-bottom: 1px solid #3c3c42;");
        RootLayout->addWidget(Header);

        auto* ScrollArea = new QScrollArea(this);
        ScrollArea->setWidgetResizable(true);
        ScrollArea->setFrameShape(QFrame::NoFrame);
        ScrollArea->setStyleSheet("QScrollArea { background: #1e1e1e; border: none; }");

        auto* Inner  = new QWidget(ScrollArea);
        auto* Layout = new QVBoxLayout(Inner);
        Layout->setContentsMargins(6, 6, 6, 6);
        Layout->setSpacing(6);

        Layout->addWidget(BuildTexturesSection());
        Layout->addWidget(BuildParametersSection());
        Layout->addStretch();

        ScrollArea->setWidget(Inner);
        RootLayout->addWidget(ScrollArea);
    }

    QWidget* MaterialEditorPanel::BuildTexturesSection() {
        auto* TexturesGroup = new QGroupBox(tr("Texturas"), this);
        TexturesGroup->setStyleSheet(
            "QGroupBox { color: #a0c8ff; font-size: 11px; font-weight: bold;"
            "  border: 1px solid #3c3c42; border-radius: 4px; margin-top: 8px; padding-top: 4px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 8px; }");

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
        auto* group = new QGroupBox(tr("Parâmetros PBR"), this);
        group->setStyleSheet(
            "QGroupBox { color: #a0c8ff; font-size: 11px; font-weight: bold;"
            "  border: 1px solid #3c3c42; border-radius: 4px; margin-top: 8px; padding-top: 4px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 8px; }");

        auto* form = new QFormLayout(group);
        form->setContentsMargins(4, 8, 4, 4);
        form->setSpacing(4);
        form->setLabelAlignment(Qt::AlignRight);

        const QString labelStyle = "color: #aaa; font-size: 10px;";
        const QString spinStyle  =
            "QDoubleSpinBox { background: #2d2d30; color: #dcdcdc; border: 1px solid #555;"
            "  border-radius: 3px; padding: 1px 4px; font-size: 10px; }"
            "QDoubleSpinBox:focus { border-color: #0e639c; }";
        const QString btnStyle =
            "QPushButton { border: 1px solid #555; border-radius: 3px; min-height: 20px; }"
            "QPushButton:hover { border-color: #0e639c; }";

        auto makeLabel = [&](const char* text) -> QLabel* {
            auto* l = new QLabel(tr(text), group);
            l->setStyleSheet(labelStyle);
            return l;
        };

        auto makeSpinbox = [&](double lo, double hi, double step, double val) -> QDoubleSpinBox* {
            auto* s = new QDoubleSpinBox(group);
            s->setRange(lo, hi);
            s->setSingleStep(step);
            s->setValue(val);
            s->setDecimals(3);
            s->setFixedWidth(72);
            s->setStyleSheet(spinStyle);
            return s;
        };

        // --- Cor Base ---
        BaseColorBtn = new QPushButton(group);
        BaseColorBtn->setFixedHeight(22);
        BaseColorBtn->setStyleSheet(btnStyle);
        UpdateColorButton(BaseColorBtn, BaseColorValue);
        form->addRow(makeLabel("Cor Base"), BaseColorBtn);
        connect(BaseColorBtn, &QPushButton::clicked, this, &MaterialEditorPanel::OnPickBaseColor);

        // --- Metálico ---
        MetallicSpin = makeSpinbox(0.0, 1.0, 0.01, 0.0);
        form->addRow(makeLabel("Metálico"), MetallicSpin);
        connect(MetallicSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnMetallicChanged);

        // --- Rugosidade ---
        RoughnessSpin = makeSpinbox(0.0, 1.0, 0.01, 0.5);
        form->addRow(makeLabel("Rugosidade"), RoughnessSpin);
        connect(RoughnessSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnRoughnessChanged);

        // --- Força AO ---
        AOStrengthSpin = makeSpinbox(0.0, 1.0, 0.01, 1.0);
        form->addRow(makeLabel("Força AO"), AOStrengthSpin);
        connect(AOStrengthSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnAOStrengthChanged);

        // --- Força Normal ---
        NormalStrSpin = makeSpinbox(0.0, 3.0, 0.05, 1.0);
        form->addRow(makeLabel("Força Normal"), NormalStrSpin);
        connect(NormalStrSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnNormalStrengthChanged);

        // --- Normal FlipY (DirectX convention) ---
        NormalFlipYCheck = new QCheckBox(tr("Normal DirectX (inverter Y)"), group);
        NormalFlipYCheck->setStyleSheet("color: #aaa; font-size: 10px;");
        form->addRow(NormalFlipYCheck);
        connect(NormalFlipYCheck, &QCheckBox::toggled, this, &MaterialEditorPanel::OnNormalFlipYChanged);

        // --- Escala Height ---
        HeightScaleSpin = makeSpinbox(0.0, 0.2, 0.001, 0.05);
        form->addRow(makeLabel("Escala Height"), HeightScaleSpin);
        connect(HeightScaleSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnHeightScaleChanged);

        // --- Separador visual ---
        auto* sep = new QFrame(group);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: #3c3c42;");
        form->addRow(sep);

        // --- Cor Emissiva ---
        EmissiveColorBtn = new QPushButton(group);
        EmissiveColorBtn->setFixedHeight(22);
        EmissiveColorBtn->setStyleSheet(btnStyle);
        UpdateColorButton(EmissiveColorBtn, EmissiveColorValue);
        form->addRow(makeLabel("Emissivo"), EmissiveColorBtn);
        connect(EmissiveColorBtn, &QPushButton::clicked, this, &MaterialEditorPanel::OnPickEmissiveColor);

        // --- Intensidade Emissiva ---
        EmissiveStrSpin = makeSpinbox(0.0, 20.0, 0.1, 1.0);
        form->addRow(makeLabel("Intensidade"), EmissiveStrSpin);
        connect(EmissiveStrSpin, &QDoubleSpinBox::valueChanged, this, &MaterialEditorPanel::OnEmissiveStrengthChanged);

        return group;
    }

    Smile::EDefaultTexture MaterialEditorPanel::FallbackTypeForSlot(int slot) {
        using E = Smile::EDefaultTexture;
        switch (slot) {
            case 0: return E::White;       // Albedo
            case 1: return E::FlatNormal;  // Normal
            case 2: return E::ORM;         // MetallicRoughness
            case 3: return E::White;       // AO
            case 4: return E::MidGrey;     // Height
            case 5: return E::Black;       // Emissive
            default: return E::White;
        }
    }

    void MaterialEditorPanel::InitializeWithRenderer(Smile::Renderer* renderer) {
        if (RendererPtr) return; // already done
        RendererPtr = renderer;

        auto* Dev = renderer->GetDevice().Native();
        auto& CQ  = renderer->GetCmdQueue();
        auto& SRV = renderer->GetSRVHeap();

        // Create 1×1 fallback textures for all 6 slots
        for (int i = 0; i < static_cast<int>(Smile::kMaterialTextureSlots); ++i)
            FallbackTexture[i] = Smile::FTexture::CreateDefault(Dev, CQ, SRV, FallbackTypeForSlot(i));

        // Wire fallbacks into material
        Material.Albedo            = &FallbackTexture[0];
        Material.Normal            = &FallbackTexture[1];
        Material.MetallicRoughness = &FallbackTexture[2];
        Material.AO                = &FallbackTexture[3];
        Material.Height            = &FallbackTexture[4];
        Material.Emissive          = &FallbackTexture[5];

        // Apply initial parameter values from UI state
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
        Material.Constants.HeightScale      = static_cast<float>(HeightScaleSpin->value());
        Material.Constants.EmissiveFactor   = {
            EmissiveColorValue.redF(),
            EmissiveColorValue.greenF(),
            EmissiveColorValue.blueF(),
            1.0f
        };
        Material.Constants.EmissiveStrength = static_cast<float>(EmissiveStrSpin->value());

        Material.Finalize(Dev, SRV);
        renderer->SetMaterial(&Material);
    }

    void MaterialEditorPanel::SetSlotPointer(int slot, Smile::FTexture* tex) {
        switch (slot) {
            case 0: Material.Albedo            = tex; break;
            case 1: Material.Normal            = tex; break;
            case 2: Material.MetallicRoughness = tex; break;
            case 3: Material.AO                = tex; break;
            case 4: Material.Height            = tex; break;
            case 5: Material.Emissive          = tex; break;
        }
    }

    void MaterialEditorPanel::UpdateHasFlag(int slot, bool has) {
        const Smile::u32 v = has ? 1u : 0u;
        switch (slot) {
            case 0: Material.Constants.HasAlbedoMap            = v; break;
            case 1: Material.Constants.HasNormalMap            = v; break;
            case 2: Material.Constants.HasMetallicRoughnessMap = v; break;
            case 3: Material.Constants.HasAOMap                = v; break;
            case 4: Material.Constants.HasHeightMap            = v; break;
            case 5: Material.Constants.HasEmissiveMap          = v; break;
        }
    }

    void MaterialEditorPanel::ApplyTextureToSlot(int slot, const QString& path) {
        if (!RendererPtr) return;

        auto* Dev = RendererPtr->GetDevice().Native();
        auto& CQ  = RendererPtr->GetCmdQueue();
        auto& SRV = RendererPtr->GetSRVHeap();

        // Load texture via WIC — runs synchronously on the Qt main thread
        const std::wstring wpath = path.toStdWString();
        SlotTexture[slot] = Smile::FTexture::LoadFromFile(Dev, CQ, SRV, wpath);

        SetSlotPointer(slot, &SlotTexture[slot].value());
        Material.UpdateTextureSlot(Dev, SRV, static_cast<Smile::u32>(slot), &SlotTexture[slot].value());
        UpdateHasFlag(slot, true);
        Material.UpdateConstants();

        // Update thumbnail using Qt's own image loading (independent of WIC)
        if (SlotWidgets[slot]) {
            SlotWidgets[slot]->SetThumbnail(QPixmap(path));
            SlotWidgets[slot]->SetPath(path);
        }
    }

    void MaterialEditorPanel::ClearSlot(int slot) {
        if (!RendererPtr) return;

        auto* Dev = RendererPtr->GetDevice().Native();
        auto& SRV = RendererPtr->GetSRVHeap();

        SlotTexture[slot] = std::nullopt;
        SetSlotPointer(slot, &FallbackTexture[slot]);
        Material.UpdateTextureSlot(Dev, SRV, static_cast<Smile::u32>(slot), &FallbackTexture[slot]);
        UpdateHasFlag(slot, false);
        Material.UpdateConstants();

        if (SlotWidgets[slot]) {
            SlotWidgets[slot]->ClearThumbnail();
            SlotWidgets[slot]->SetPath(QString());
        }
    }

    void MaterialEditorPanel::OnBrowseSlot(int slot) {
        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Selecionar Textura — %1").arg(tr(kSlots[slot].Name)),
            QString(),
            tr("Imagens (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;Todos os arquivos (*)"));
        if (!path.isEmpty())
            ApplyTextureToSlot(slot, path);
    }

    void MaterialEditorPanel::OnClearSlot(int slot) {
        ClearSlot(slot);
    }

    void MaterialEditorPanel::UpdateColorButton(QPushButton* _Button, const QColor& color) {
        _Button->setStyleSheet(QString(
            "QPushButton { background: %1; border: 1px solid #555; border-radius: 3px; min-height: 20px; }"
            "QPushButton:hover { border-color: #0e639c; }").arg(color.name()));
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
        const QColor c = QColorDialog::getColor(EmissiveColorValue, this, tr("Cor Emissiva"));
        if (!c.isValid()) return;
        EmissiveColorValue = c;
        UpdateColorButton(EmissiveColorBtn, c);
        if (!RendererPtr) return;
        Material.Constants.EmissiveFactor = {
            static_cast<Smile::f32>(c.redF()),
            static_cast<Smile::f32>(c.greenF()),
            static_cast<Smile::f32>(c.blueF()),
            1.0f
        };
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnMetallicChanged(double v) {
        if (!RendererPtr) return;
        Material.Constants.MetallicFactor = static_cast<Smile::f32>(v);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnRoughnessChanged(double v) {
        if (!RendererPtr) return;
        Material.Constants.RoughnessFactor = static_cast<Smile::f32>(v);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnAOStrengthChanged(double v) {
        if (!RendererPtr) return;
        Material.Constants.AOStrength = static_cast<Smile::f32>(v);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnNormalStrengthChanged(double v) {
        if (!RendererPtr) return;
        Material.Constants.NormalStrength = static_cast<Smile::f32>(v);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnHeightScaleChanged(double v) {
        if (!RendererPtr) return;
        Material.Constants.HeightScale = static_cast<Smile::f32>(v);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnEmissiveStrengthChanged(double v) {
        if (!RendererPtr) return;
        Material.Constants.EmissiveStrength = static_cast<Smile::f32>(v);
        Material.UpdateConstants();
    }

    void MaterialEditorPanel::OnNormalFlipYChanged(bool checked) {
        if (!RendererPtr) return;
        Material.Constants.NormalFlipY = checked ? 1u : 0u;
        Material.UpdateConstants();
    }
} 
