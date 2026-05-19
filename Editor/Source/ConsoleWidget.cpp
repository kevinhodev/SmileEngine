#include "SmileEditor/ConsoleWidget.h"

#include <QPainter>
#include <QWheelEvent>
#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>

namespace SmileEditor {

// ─── Paleta estilo UE5 Output Log ────────────────────────────────────────────

static constexpr QColor kBgMain  { 25,  25,  25  };  // fundo geral
static constexpr QColor kBgStats { 30,  30,  30  };  // barra de stats
static constexpr QColor kBgRowB  { 28,  28,  28  };  // linhas alternadas (par)
static constexpr QColor kSep     { 45,  45,  45  };  // separador
static constexpr QColor kAccent  { 38, 139, 210  };  // barra azul esquerda

static constexpr QColor kTagInfo { 110, 110, 110 };  // [INFO] cinza muted
static constexpr QColor kTagWarn { 215, 186, 125 };  // [WARN] amarelo quente
static constexpr QColor kTagErr  { 244,  71,  71 };  // [ERR]  vermelho

static constexpr QColor kTxtInfo { 212, 212, 212 };
static constexpr QColor kTxtWarn { 229, 192, 123 };
static constexpr QColor kTxtErr  { 244,  71,  71 };
static constexpr QColor kStats   { 156, 220, 254 };  // azul claro para stats

// ─── Layout (pixels físicos, DPI tratado pelo QRhiWidget) ────────────────────

static constexpr int kPadX    =  10;
static constexpr int kPadY    =   3;
static constexpr int kLineH   =  18;
static constexpr int kStatsH  =  42;  // 2 linhas de stats
static constexpr int kSepH    =   1;
static constexpr int kAccentW =   3;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static QShader LoadShader(const char* path) {
    QFile f(path);
    if (!f.open(QFile::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

// ─── Ctor ─────────────────────────────────────────────────────────────────────

ConsoleWidget::ConsoleWidget(QWidget* parent)
    : QRhiWidget(parent)
{
    setMinimumSize(100, 60);
}

// ─── API pública ──────────────────────────────────────────────────────────────

void ConsoleWidget::AppendLine(Smile::LogLevel level, const QString& text) {
    if (static_cast<int>(LogEntries.size()) >= kMaxEntries)
        LogEntries.pop_front();
    LogEntries.push_back({ level, text });
    Dirty = true;
    update();
}

void ConsoleWidget::SetStats(const QString& text) {
    if (StatsText == text) return;
    StatsText = text;
    Dirty = true;
    update();
}

void ConsoleWidget::wheelEvent(QWheelEvent* e) {
    const int total  = static_cast<int>(LogEntries.size());
    // FrameImage.height() tem o tamanho físico atual (set em initialize)
    const int physH  = FrameImage.isNull() ? height() : FrameImage.height();
    const int maxVis = std::max(1, (physH - kStatsH - kSepH) / kLineH);
    ScrollOffset = std::clamp(ScrollOffset - e->angleDelta().y() / 40,
                              0, std::max(0, total - maxVis));
    Dirty = true;
    update();
    e->accept();
}

// ─── QRhiWidget ───────────────────────────────────────────────────────────────

void ConsoleWidget::initialize(QRhiCommandBuffer*) {
    QRhi* rhi = this->rhi();

    // Se o device mudou, libera tudo e recria
    if (CurrentRhi != rhi) {
        releaseResources();
        CurrentRhi = rhi;
    }

    const QSize sz = renderTarget()->pixelSize();

    // Recria texture e SRB quando o tamanho muda (ou na primeira vez)
    if (!Texture || Texture->pixelSize() != sz) {
        delete Texture; Texture = nullptr;
        delete SRB;     SRB     = nullptr;

        FrameImage = QImage(sz, QImage::Format_RGBA8888);
        FrameImage.fill(kBgMain);

        Texture = rhi->newTexture(QRhiTexture::RGBA8, sz, 1, {});
        Texture->create();

        // SRB referencia UBO + Texture: UBO deve existir antes (criado abaixo se ainda não existe)
        Dirty = true;
    }

    // Recursos independentes de tamanho: criados apenas uma vez
    if (!Pipeline) {
        Sampler = rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                  QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        Sampler->create();

        // Quad fullscreen em NDC: pos (xy) + uv (xy) por vértice
        //  NDC        UV          topo-esquerda = (0,0) em QImage e em D3D11/D3D12
        // (-1, 1)  (0, 0)
        // ( 1, 1)  (1, 0)
        // (-1,-1)  (0, 1)
        // ( 1,-1)  (1, 1)
        static const float kVerts[] = {
            -1.f,  1.f,  0.f, 0.f,
             1.f,  1.f,  1.f, 0.f,
            -1.f, -1.f,  0.f, 1.f,
             1.f, -1.f,  1.f, 1.f,
        };
        static const quint16 kIdx[] = { 0, 2, 1,  1, 2, 3 };

        VBO = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kVerts));
        VBO->create();
        IBO = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, sizeof(kIdx));
        IBO->create();

        // UBO: mat4 MVP (64 bytes, std140)
        UBO = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64);
        UBO->create();

        // Pipeline: textured quad simples
        Pipeline = rhi->newGraphicsPipeline();
        Pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,
              LoadShader(":/SmileEditor/Shaders/console.vert.qsb") },
            { QRhiShaderStage::Fragment,
              LoadShader(":/SmileEditor/Shaders/console.frag.qsb") },
        });

        QRhiVertexInputLayout vtxLayout;
        vtxLayout.setBindings({ { 4 * sizeof(float) } });
        vtxLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float2, quint32(2 * sizeof(float)) },
        });
        Pipeline->setVertexInputLayout(vtxLayout);
        Pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

        // SRB placeholder apenas para criar o pipeline (Texture ainda pode não existir)
        // O SRB real com a Texture correta é criado logo abaixo
        NeedsGeomUpload = true;
    }

    // (Re)cria SRB sempre que Texture foi recriada e UBO já existe
    if (!SRB && UBO) {
        SRB = rhi->newShaderResourceBindings();
        SRB->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                UBO),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                Texture, Sampler),
        });
        SRB->create();

        // Pipeline precisa do SRB para saber o layout de bindings
        Pipeline->setShaderResourceBindings(SRB);
        Pipeline->create();
    }
}

void ConsoleWidget::render(QRhiCommandBuffer* cb) {
    QRhi* rhi = this->rhi();
    QRhiRenderTarget* rt = renderTarget();

    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();

    // Upload de geometria e MVP na primeira vez (ou após recreate)
    if (NeedsGeomUpload) {
        static const float kVerts[] = {
            -1.f,  1.f,  0.f, 0.f,
             1.f,  1.f,  1.f, 0.f,
            -1.f, -1.f,  0.f, 1.f,
             1.f, -1.f,  1.f, 1.f,
        };
        static const quint16 kIdx[] = { 0, 2, 1,  1, 2, 3 };

        batch->uploadStaticBuffer(VBO, kVerts);
        batch->uploadStaticBuffer(IBO, kIdx);

        // clipSpaceCorrMatrix lida com diferenças de Y entre backends (OpenGL vs D3D/Vulkan)
        QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix();
        batch->updateDynamicBuffer(UBO, 0, 64, mvp.constData());

        NeedsGeomUpload = false;
    }

    // Re-renderiza frame se o conteúdo mudou
    if (Dirty) {
        RedrawTexture();
        batch->uploadTexture(Texture, FrameImage);
        Dirty = false;
    }

    cb->beginPass(rt, QColor(25, 25, 25), { 1.0f, 0 }, batch);
    cb->setGraphicsPipeline(Pipeline);
    cb->setShaderResources(SRB);
    cb->setViewport({ 0, 0,
        float(rt->pixelSize().width()),
        float(rt->pixelSize().height()) });

    const QRhiCommandBuffer::VertexInput vb(VBO, 0);
    cb->setVertexInput(0, 1, &vb, IBO, 0, QRhiCommandBuffer::IndexUInt16);
    cb->drawIndexed(6);
    cb->endPass();
}

void ConsoleWidget::releaseResources() {
    // Destrói em ordem inversa de dependência
    delete Pipeline;  Pipeline = nullptr;
    delete SRB;       SRB      = nullptr;
    delete UBO;       UBO      = nullptr;
    delete IBO;       IBO      = nullptr;
    delete VBO;       VBO      = nullptr;
    delete Sampler;   Sampler  = nullptr;
    delete Texture;   Texture  = nullptr;
    NeedsGeomUpload = true;
}

// ─── Renderização CPU do frame (QPainter → QImage) ───────────────────────────

void ConsoleWidget::RedrawTexture() {
    const int W = FrameImage.width();
    const int H = FrameImage.height();

    QPainter p(&FrameImage);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Fundo geral
    p.fillRect(0, 0, W, H, kBgMain);

    // ── Barra de stats (topo, pinada) ─────────────────────────────────────
    const int statsH = StatsText.isEmpty() ? 0 : kStatsH;
    if (!StatsText.isEmpty()) {
        p.fillRect(0, 0, W, statsH, kBgStats);

        // Barra de acento azul (estilo UE5 Output Log)
        p.fillRect(0, 0, kAccentW, statsH, kAccent);

        p.setFont(QFont(QStringLiteral("Consolas"), 9));
        p.setPen(kStats);

        const int nl = StatsText.indexOf(u'\n');
        const QString line1 = StatsText.left(nl < 0 ? StatsText.size() : nl);
        const QString line2 = nl >= 0 ? StatsText.mid(nl + 1) : QString{};

        const int tx     = kAccentW + kPadX;
        const int ascent = p.fontMetrics().ascent();
        p.drawText(tx, kPadY + ascent,         line1);
        if (!line2.isEmpty())
            p.drawText(tx, kPadY + kLineH + ascent, line2);

        // Separador
        p.fillRect(0, statsH, W, kSepH, kSep);
    }

    // ── Entradas de log ───────────────────────────────────────────────────
    const int logTop = statsH + (statsH > 0 ? kSepH : 0);
    const int logH   = H - logTop;
    const int maxVis = std::max(1, logH / kLineH);
    const int total  = static_cast<int>(LogEntries.size());

    ScrollOffset = std::clamp(ScrollOffset, 0, std::max(0, total - maxVis));

    const int start = std::max(0, total - maxVis - ScrollOffset);
    const int end   = std::max(0, total - ScrollOffset);

    p.setFont(QFont(QStringLiteral("Consolas"), 9));
    const int ascent = p.fontMetrics().ascent();

    for (int i = start; i < end; ++i) {
        const auto& entry = LogEntries[i];
        const int   row   = i - start;
        const int   y     = logTop + row * kLineH;

        // Listras alternadas sutis (par = kBgMain já pintado, ímpar = levemente diferente)
        if (row % 2 == 1)
            p.fillRect(0, y, W, kLineH, kBgRowB);

        const QString tag =
            entry.Level == Smile::LogLevel::Error   ? QStringLiteral("[ERR]  ") :
            entry.Level == Smile::LogLevel::Warning ? QStringLiteral("[WARN] ") :
                                                      QStringLiteral("[INFO] ");

        const QColor tagColor =
            entry.Level == Smile::LogLevel::Error   ? kTagErr  :
            entry.Level == Smile::LogLevel::Warning ? kTagWarn :
                                                      kTagInfo ;

        const QColor txtColor =
            entry.Level == Smile::LogLevel::Error   ? kTxtErr  :
            entry.Level == Smile::LogLevel::Warning ? kTxtWarn :
                                                      kTxtInfo ;

        // Tag colorido + texto da mensagem
        p.setPen(tagColor);
        p.drawText(kPadX, y + kPadY + ascent, tag);
        const int tagW = p.fontMetrics().horizontalAdvance(tag);

        p.setPen(txtColor);
        p.drawText(kPadX + tagW, y + kPadY + ascent, entry.Text);
    }

    // ── Indicador de scroll (barra fina direita) ──────────────────────────
    if (total > maxVis) {
        const int sbW = 3;
        const int sbH = std::max(20, logH * maxVis / total);
        const int range = logH - sbH;
        const int sbY = logTop + (ScrollOffset > 0
            ? range - range * ScrollOffset / std::max(1, total - maxVis)
            : range);
        p.fillRect(W - sbW - 2, sbY, sbW, sbH, QColor(80, 80, 80, 180));
    }
}

} // namespace SmileEditor
