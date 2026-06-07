#pragma once

#include <QWidget>
#include <QColor>
#include <optional>
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/Texture.h"
#include "SmileEditor/TextureLoadWorker.h"

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

    signals:
        // Dispara o decode no worker thread (conectado a TextureLoadWorker::Process).
        void RequestLoad(quint64 ReqId, int Slot, QString Path, bool IsNormalMap);

    private slots:
        // Recebe os dados CPU do worker (no thread principal) e faz o upload D3D12.
        void OnTextureLoaded(quint64 ReqId, int Slot, SmileEditor::TexCPUPtr Data);
        void OnBrowseSlot(int Slot);
        void OnClearSlot(int Slot);
        void OnPickBaseColor();
        void OnPickEmissiveColor();
        void OnNormalFlipYChanged(bool Checked);
        void OnMetallicChanged(double Value);
        void OnRoughnessChanged(double Value);
        void OnAOStrengthChanged(double Value);
        void OnNormalStrengthChanged(double Value);
        void OnEmissiveStrengthChanged(double Value);
        void OnHeightScaleChanged(double Value);
        void OnParallaxMinStepsChanged(double Value);
        void OnParallaxMaxStepsChanged(double Value);
        void OnParallaxSelfShadowChanged(bool Checked);
        void OnParallaxShadowStepsChanged(double Value);
        void OnParallaxFadeStartChanged(double Value);
        void OnParallaxFadeRangeChanged(double Value);
        void OnParallaxRefineChanged(bool Checked);
        void OnParallaxRefineStepsChanged(double Value);

    private:
        void BuildUI();
        QWidget* BuildTexturesSection();
        QWidget* BuildParametersSection();
        QWidget* BuildParallaxSection();

        void ApplyTextureToSlot(int Slot, const QString& Path);
        void ClearSlot(int Slot);
        void SetSlotPointer(int Slot, Smile::FTexture* Texture);
        void UpdateHasFlag(int Slot, bool Has);
        void UpdateColorButton(QPushButton* Button, const QColor& Color);

        static Smile::EDefaultTexture FallbackTypeForSlot(int Slot);

        Smile::Renderer* RendererPtr = nullptr;

        // Carregamento assincrono de textura: o worker encapsula seu proprio thread e
        // faz o decode (CPU); o upload D3D12 acontece em OnTextureLoaded (thread principal).
        std::unique_ptr<TextureLoadWorker> LoadWorker;
        quint64            RequestCounter = 0;
        // Ultimo ReqId emitido por slot — resultados mais antigos sao descartados (supersede).
        quint64            SlotRequestId[Smile::kMaterialTextureSlots] = {};
        // Path do pedido mais recente por slot (para thumbnail/label ao concluir).
        QString            SlotPendingPath[Smile::kMaterialTextureSlots];

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
        QDoubleSpinBox* EmissiveStrSpin     = nullptr;
        QCheckBox*      NormalFlipYCheck    = nullptr;

        QDoubleSpinBox* HeightScaleSpin       = nullptr;
        QDoubleSpinBox* ParallaxMinSpin       = nullptr;
        QDoubleSpinBox* ParallaxMaxSpin       = nullptr;
        QCheckBox*      ParallaxSelfShadowChk  = nullptr;
        QDoubleSpinBox* ParallaxShadowStepsSpin = nullptr;
        QDoubleSpinBox* ParallaxFadeStartSpin   = nullptr;
        QDoubleSpinBox* ParallaxFadeRangeSpin   = nullptr;
        QCheckBox*      ParallaxRefineChk       = nullptr;
        QDoubleSpinBox* ParallaxRefineStepsSpin = nullptr;
    };
} 
