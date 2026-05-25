#pragma once

#include <QWidget>
#include <QColor>
#include <optional>

#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/Texture.h"

class QPushButton;
class QDoubleSpinBox;
class QCheckBox;

namespace Smile { class Renderer; }

namespace SmileEditor {

class TextureSlotWidget;

// Right-side material editor panel — inspired by CryEngine Sandbox's material panel
// and Unreal's Material Instance editor.
//
// Owns a FMaterial and per-slot FTexture objects. Changes are applied to the renderer
// in real-time (no Apply button), matching Unreal's live preview behavior.
class MaterialEditorPanel : public QWidget {
    Q_OBJECT

public:
    explicit MaterialEditorPanel(QWidget* parent = nullptr);
    ~MaterialEditorPanel() override = default;

    // Called once after the D3D12 renderer becomes ready.
    // Creates fallback textures, finalizes material, sets it as active.
    void InitializeWithRenderer(Smile::Renderer* renderer);

    bool IsInitialized() const { return RendererPtr != nullptr; }

private slots:
    void OnBrowseSlot(int slot);
    void OnClearSlot(int slot);
    void OnPickBaseColor();
    void OnPickEmissiveColor();
    void OnNormalFlipYChanged(bool checked);
    void OnMetallicChanged(double v);
    void OnRoughnessChanged(double v);
    void OnAOStrengthChanged(double v);
    void OnNormalStrengthChanged(double v);
    void OnHeightScaleChanged(double v);
    void OnEmissiveStrengthChanged(double v);

private:
    void BuildUI();
    QWidget* BuildTexturesSection();
    QWidget* BuildParametersSection();

    void ApplyTextureToSlot(int slot, const QString& path);
    void ClearSlot(int slot);
    void SetSlotPointer(int slot, Smile::FTexture* tex);
    void UpdateHasFlag(int slot, bool has);
    void UpdateColorButton(QPushButton* btn, const QColor& color);

    // Slot index → EDefaultTexture fallback type
    static Smile::EDefaultTexture FallbackTypeForSlot(int slot);

    // Renderer access (non-owning)
    Smile::Renderer* RendererPtr = nullptr;

    // Material owned by this panel
    Smile::FMaterial              Mat;
    std::optional<Smile::FTexture> SlotTex[Smile::kMaterialTextureSlots];  // user-loaded
    Smile::FTexture               FallbackTex[Smile::kMaterialTextureSlots]; // 1×1 defaults

    TextureSlotWidget* SlotWidgets[Smile::kMaterialTextureSlots] = {};

    // Parameter UI
    QColor       BaseColorValue    = QColor(204, 204, 204);
    QColor       EmissiveColorValue = QColor(0, 0, 0);
    QPushButton* BaseColorBtn       = nullptr;
    QPushButton* EmissiveColorBtn   = nullptr;
    QDoubleSpinBox* MetallicSpin    = nullptr;
    QDoubleSpinBox* RoughnessSpin   = nullptr;
    QDoubleSpinBox* AOStrengthSpin  = nullptr;
    QDoubleSpinBox* NormalStrSpin   = nullptr;
    QDoubleSpinBox* HeightScaleSpin = nullptr;
    QDoubleSpinBox* EmissiveStrSpin = nullptr;
    QCheckBox*      NormalFlipYCheck = nullptr;
};

} // namespace SmileEditor
