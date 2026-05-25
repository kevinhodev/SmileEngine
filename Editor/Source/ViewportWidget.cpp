#include "SmileEditor/ViewportWidget.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Input/CameraInput.h"
#include <QShowEvent>
#include <QResizeEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QCursor>

namespace SmileEditor {

static constexpr float kMouseSensitivity = 0.15f;  // graus por pixel

ViewportWidget::ViewportWidget(QWidget* _Parent)
    : QWidget(_Parent),
      Renderer(std::make_unique<Smile::Renderer>())
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);

    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 200);

    RedrawTimer = new QTimer(this);
    RedrawTimer->setInterval(16);
    connect(RedrawTimer, &QTimer::timeout, this, &ViewportWidget::OnRenderTimer);

    FrameTimer.start();
}

ViewportWidget::~ViewportWidget() {
    if (RedrawTimer) RedrawTimer->stop();
    if (Renderer)    Renderer->Shutdown();
}

QPaintEngine* ViewportWidget::paintEngine() const {
    return nullptr;
}

void ViewportWidget::EnsureRendererIsInitialized() {
    if (Initialized) return;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    Renderer->Initialize(hwnd,
                         static_cast<unsigned int>(width()),
                         static_cast<unsigned int>(height()));
    Initialized = true;
    emit RendererInitialized();
}

// ---------- Ciclo de vida ----------

void ViewportWidget::showEvent(QShowEvent* _Event) {
    QWidget::showEvent(_Event);
    EnsureRendererIsInitialized();
    FrameTimer.restart();
    RedrawTimer->start();
}

void ViewportWidget::hideEvent(QHideEvent* _Event) {
    RedrawTimer->stop();
    QWidget::hideEvent(_Event);
}

void ViewportWidget::resizeEvent(QResizeEvent* _Event) {
    QWidget::resizeEvent(_Event);
    if (Initialized) {
        Renderer->Resize(static_cast<unsigned int>(_Event->size().width()),
                         static_cast<unsigned int>(_Event->size().height()));
    }
}

void ViewportWidget::paintEvent(QPaintEvent* _Event) {
    Q_UNUSED(_Event);
    // D3D12 controla o HWND; Qt nao precisa pintar nada aqui.
    // O timer chama RenderFrame() diretamente via OnRenderTimer().
}

// ---------- Timer principal ----------

void ViewportWidget::OnRenderTimer() {
    EnsureRendererIsInitialized();
    if (!Renderer->IsInitialized()) return;

    float dt = static_cast<float>(FrameTimer.elapsed()) / 1000.0f;
    FrameTimer.restart();
    dt = Smile::Clamp(dt, 0.001f, 0.1f);  // evita spike no primeiro frame

    // Monta CameraInput a partir do estado de input acumulado
    Smile::CameraInput camInput;
    camInput.Look  = MouseLookActive
        ? Smile::Vec2{ MouseDelta.X * kMouseSensitivity,
                      -MouseDelta.Y * kMouseSensitivity }   // Y invertido: mouse cima = pitch cima
        : Smile::Vec2::Zero();
    camInput.Move  = Smile::Vec3{
        static_cast<float>(IsHeld(Qt::Key_D) - IsHeld(Qt::Key_A)),   // right/left
        static_cast<float>(IsHeld(Qt::Key_E) - IsHeld(Qt::Key_Q)),   // up/down
        static_cast<float>(IsHeld(Qt::Key_W) - IsHeld(Qt::Key_S)),   // forward/back
    };
    camInput.Speed = IsHeld(Qt::Key_Shift) ? 4.0f : 1.0f;

    Renderer->UpdateCamera(camInput, dt);
    Renderer->RenderFrame();

    LastFPS    = dt > 0.0f ? 1.0f / dt : 0.0f;
    MouseDelta = Smile::Vec2::Zero();
    emit FrameReady();
}

// ---------- Teclado ----------

void ViewportWidget::keyPressEvent(QKeyEvent* _Event) {
    if (!_Event->isAutoRepeat())
        HeldKeys.insert(_Event->key());
    QWidget::keyPressEvent(_Event);
}

void ViewportWidget::keyReleaseEvent(QKeyEvent* _Event) {
    if (!_Event->isAutoRepeat())
        HeldKeys.remove(_Event->key());
    QWidget::keyReleaseEvent(_Event);
}

// ---------- Mouse ----------

void ViewportWidget::mousePressEvent(QMouseEvent* _Event) {
    if (_Event->button() == Qt::RightButton) {
        MouseLookActive = true;
        IgnoreNextMove  = false;
        setCursor(Qt::BlankCursor);
        // Ancora o cursor no centro do widget imediatamente
        QCursor::setPos(mapToGlobal(rect().center()));
        IgnoreNextMove = true;
        setFocus();
    }
    QWidget::mousePressEvent(_Event);
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* _Event) {
    if (_Event->button() == Qt::RightButton) {
        MouseLookActive = false;
        MouseDelta      = Smile::Vec2::Zero();
        unsetCursor();
    }
    QWidget::mouseReleaseEvent(_Event);
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* _Event) {
    if (!MouseLookActive) {
        QWidget::mouseMoveEvent(_Event);
        return;
    }

    // Ignora o evento sintético gerado pelo próprio QCursor::setPos abaixo
    if (IgnoreNextMove) {
        IgnoreNextMove = false;
        QWidget::mouseMoveEvent(_Event);
        return;
    }

    QPoint center = mapToGlobal(rect().center());
    QPoint pos    = _Event->globalPosition().toPoint();

    MouseDelta.X += static_cast<float>(pos.x() - center.x());
    MouseDelta.Y += static_cast<float>(pos.y() - center.y());

    // Ancora de volta ao centro para o próximo evento
    IgnoreNextMove = true;
    QCursor::setPos(center);

    QWidget::mouseMoveEvent(_Event);
}

} // namespace SmileEditor
