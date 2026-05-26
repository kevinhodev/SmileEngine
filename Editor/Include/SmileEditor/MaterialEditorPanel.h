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

    class MaterialEditorPanel : public QWidget {
        Q_OBJECT

    public:
        explicit MaterialEditorPanel(QWidget* Parent = nullptr);
        ~MaterialEditorPanel() override = default;

        void InitializeWithRenderer(Smile::Renderer* Renderer);

        bool IsInitialized() const { return RendererPtr != nullptr; }

    private slots:
        void OnBrowseSlot(int Slot);
        void OnClearSlot(int Slot);
        void OnPickBaseColor();
        void OnPickEmissiveColor();
        void OnNormalFlipYChanged(bool Checked);
        void OnMetallicChanged(double Value);
        void OnRoughnessChanged(double Value);
        void OnAOStrengthChanged(double Value);
        void OnNormalStrengthChanged(double Value);
        void OnHeightScaleChanged(double Value);
        void OnEmissiveStrengthChanged(double Value);

    private:
        void BuildUI();
        QWidget* BuildTexturesSection();
        QWidget* BuildParametersSection();

        void ApplyTextureToSlot(int Slot, const QString& Path);
        void ClearSlot(int Slot);
        void SetSlotPointer(int Slot, Smile::FTexture* Texture);
        void UpdateHasFlag(int Slot, bool Has);
        void UpdateColorButton(QPushButton* Button, const QColor& Color);

        static Smile::EDefaultTexture FallbackTypeForSlot(int Slot);

        Smile::Renderer* RendererPtr = nullptr;

        Smile::FMaterial               Material;
        std::optional<Smile::FTexture> SlotTexture[Smile::kMaterialTextureSlots]; 
        Smile::FTexture                FallbackTexture[Smile::kMaterialTextureSlots]; 

        TextureSlotWidget* SlotWidgets[Smile::kMaterialTextureSlots] = {};

        QColor          BaseColorValue      = QColor(204, 204, 204);
        QColor          EmissiveColorValue  = QColor(0, 0, 0);
        QPushButton*    BaseColorBtn        = nullptr;
        QPushButton*    EmissiveColorBtn    = nullptr;
        QDoubleSpinBox* MetallicSpin        = nullptr;
        QDoubleSpinBox* RoughnessSpin       = nullptr;
        QDoubleSpinBox* AOStrengthSpin      = nullptr;
        QDoubleSpinBox* NormalStrSpin       = nullptr;
        QDoubleSpinBox* HeightScaleSpin     = nullptr;
        QDoubleSpinBox* EmissiveStrSpin     = nullptr;
        QCheckBox*      NormalFlipYCheck    = nullptr;
    };
} 
