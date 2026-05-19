#pragma once

#include <QRhiWidget>
#include <deque>

// Forward declarations — rhi/qrhi.h está no caminho privado do Qt;
// o .cpp faz o include completo via Qt6::GuiPrivate.
class QRhi;
class QRhiTexture;
class QRhiSampler;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
class QRhiCommandBuffer;

#include "Smile/Core/Logger.h"

namespace SmileEditor {

// Console renderizado via QRhi (GPU): QPainter desenha o frame em QImage,
// que é uploadado como textura e exibido num quad fullscreen via pipeline QRhi.
// Resize e composição com o resto dos widgets é gerenciado pelo Qt.
class ConsoleWidget : public QRhiWidget {
    Q_OBJECT

public:
    explicit ConsoleWidget(QWidget* parent = nullptr);

    void AppendLine(Smile::LogLevel level, const QString& text);
    void SetStats(const QString& text);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb)     override;
    void releaseResources()                override;
    void wheelEvent(QWheelEvent* e)        override;

private:
    struct LogEntry {
        Smile::LogLevel Level;
        QString         Text;
    };

    void RedrawTexture();

    // ── QRhi resources ───────────────────────────────────────────────────────
    QRhi*                       CurrentRhi = nullptr;

    QRhiTexture*                Texture    = nullptr;  // frame renderizado pelo QPainter
    QRhiSampler*                Sampler    = nullptr;
    QRhiBuffer*                 VBO        = nullptr;  // quad fullscreen
    QRhiBuffer*                 IBO        = nullptr;
    QRhiBuffer*                 UBO        = nullptr;  // mat4 MVP
    QRhiShaderResourceBindings* SRB        = nullptr;
    QRhiGraphicsPipeline*       Pipeline   = nullptr;

    bool NeedsGeomUpload = true;  // sinaliza primeiro upload de VBO/IBO/UBO

    // ── Frame ────────────────────────────────────────────────────────────────
    QImage FrameImage;  // buffer CPU onde QPainter desenha

    // ── Estado do console ────────────────────────────────────────────────────
    std::deque<LogEntry> LogEntries;
    static constexpr int kMaxEntries = 500;

    QString StatsText;
    int     ScrollOffset = 0;
    bool    Dirty        = true;
};

} // namespace SmileEditor
