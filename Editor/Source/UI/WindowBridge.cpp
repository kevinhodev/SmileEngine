#include "SmileEditor/UI/WindowBridge.h"
#include <QWidget>
#include <QWindow>
#include <QRect>

namespace SmileEditor {
    WindowBridge::WindowBridge(QWidget* _Window, QObject* _Parent)
        : QObject(_Parent), Window(_Window)
    {
    }

    bool WindowBridge::IsMaximized() const {
        return Window && (Window->isMaximized() || Window->isFullScreen());
    }

    bool WindowBridge::PointIsInteractive(int _X, int _Y) const {
        const QPoint P(_X, _Y);
        for (const QRect& R : InteractiveRects)
            if (R.contains(P)) return true;
        return false;
    }

    qreal WindowBridge::Dpr() const {
        return Window ? Window->devicePixelRatioF() : 1.0;
    }

    void WindowBridge::NotifyWindowStateChanged() {
        emit MaximizedChanged();
    }

    void WindowBridge::minimize() {
        if (Window) Window->showMinimized();
    }

    void WindowBridge::toggleMaximize() {
        if (!Window) return;
        if (Window->isMaximized()) Window->showNormal();
        else                       Window->showMaximized();
        emit MaximizedChanged();
    }

    void WindowBridge::close() {
        if (Window) Window->close();
    }

    void WindowBridge::setTitleBarHeight(int _H) {
        TitleBarH = _H;
    }

    void WindowBridge::setInteractiveRects(const QVariantList& _Rects) {
        InteractiveRects.clear();
        InteractiveRects.reserve(_Rects.size());
        for (const QVariant& V : _Rects) {
            // QML manda Qt.rect(x,y,w,h) -> QRectF; toRect() arredonda pro hit-test em pixels.
            const QRectF R = V.toRectF();
            if (!R.isNull()) InteractiveRects.push_back(R.toRect());
        }
    }

    void WindowBridge::startSystemMove() {
        if (Window && Window->windowHandle())
            Window->windowHandle()->startSystemMove();
    }

    void WindowBridge::startSystemResize(int _Edges) {
        if (Window && Window->windowHandle())
            Window->windowHandle()->startSystemResize(static_cast<Qt::Edges>(_Edges));
    }
}
