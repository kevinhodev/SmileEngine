#pragma once

#include <QWidget>
#include <QSet>
#include <QPoint>
#include <QElapsedTimer>
#include <memory>
#include "Smile/Math/Math.h"

class QTimer;
class QPaintEngine;
class QKeyEvent;
class QMouseEvent;

namespace Smile { class Renderer; }

namespace SmileEditor {
    class ViewportWidget : public QWidget {
        Q_OBJECT

    public:
        explicit ViewportWidget(QWidget* parent = nullptr);
        ~ViewportWidget() override;

        Smile::Renderer* GetRenderer() const { return Renderer.get(); }
        float            GetFPS()      const { return LastFPS; }

        QPaintEngine* paintEngine() const override;

    protected:
        void showEvent(QShowEvent* event)   override;
        void resizeEvent(QResizeEvent* event) override;
        void paintEvent(QPaintEvent* event) override;
        void hideEvent(QHideEvent* event)   override;

        void keyPressEvent(QKeyEvent* event)     override;
        void keyReleaseEvent(QKeyEvent* event)   override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event)  override;

    signals:
        void FrameReady();

    private slots:
        void OnRenderTimer();

    private:
        void EnsureRendererIsInitialized();

        bool IsHeld(int key) const { return HeldKeys.contains(key); }

        std::unique_ptr<Smile::Renderer> Renderer;
        QTimer*       RedrawTimer       = nullptr;
        bool          Initialized       = false;

        QSet<int>     HeldKeys;
        Smile::Vec2   MouseDelta       = Smile::Vec2::Zero();
        bool          MouseLookActive  = false;
        bool          IgnoreNextMove   = false;
        float         LastFPS          = 0.0f;
        QElapsedTimer FrameTimer;
    };
} 
