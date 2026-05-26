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
    static constexpr float kMouseSensitivity = 0.15f;  

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
        const HWND hWnd = reinterpret_cast<HWND>(winId());
        Renderer->Initialize(hWnd,
                             static_cast<unsigned int>(width()),
                             static_cast<unsigned int>(height()));
        Initialized = true;
        emit RendererInitialized();
    }

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
    }

    void ViewportWidget::OnRenderTimer() {
        EnsureRendererIsInitialized();
        if (!Renderer->IsInitialized()) return;

        float DeltaTime = static_cast<float>(FrameTimer.elapsed()) / 1000.0f;
        FrameTimer.restart();
        DeltaTime = Smile::Clamp(DeltaTime, 0.001f, 0.1f);  

        Smile::CameraInput CameraInput;
        CameraInput.Look  = MouseLookActive
            ? Smile::Vec2{ MouseDelta.X * kMouseSensitivity,
                          -MouseDelta.Y * kMouseSensitivity }   
            : Smile::Vec2::Zero();
        CameraInput.Move  = Smile::Vec3{
            static_cast<float>(IsHeld(Qt::Key_D) - IsHeld(Qt::Key_A)),   
            static_cast<float>(IsHeld(Qt::Key_E) - IsHeld(Qt::Key_Q)),   
            static_cast<float>(IsHeld(Qt::Key_W) - IsHeld(Qt::Key_S)),   
        };
        CameraInput.Speed = IsHeld(Qt::Key_Shift) ? 4.0f : 1.0f;

        Renderer->UpdateCamera(CameraInput, DeltaTime);
        Renderer->RenderFrame();

        LastFPS    = DeltaTime > 0.0f ? 1.0f / DeltaTime : 0.0f;
        MouseDelta = Smile::Vec2::Zero();
        emit FrameReady();
    }

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

    void ViewportWidget::mousePressEvent(QMouseEvent* _Event) {
        if (_Event->button() == Qt::RightButton) {
            MouseLookActive = true;
            IgnoreNextMove  = false;
            setCursor(Qt::BlankCursor);
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

        if (IgnoreNextMove) {
            IgnoreNextMove = false;
            QWidget::mouseMoveEvent(_Event);
            return;
        }

        QPoint Center = mapToGlobal(rect().center());
        QPoint Position    = _Event->globalPosition().toPoint();

        MouseDelta.X += static_cast<float>(Position.x() - Center.x());
        MouseDelta.Y += static_cast<float>(Position.y() - Center.y());

        IgnoreNextMove = true;
        QCursor::setPos(Center);

        QWidget::mouseMoveEvent(_Event);
    }
} 
