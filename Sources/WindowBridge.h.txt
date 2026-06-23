#pragma once

#include <QObject>
#include <QRect>
#include <QVector>
#include <QVariantList>

class QWidget;

namespace SmileEditor {
    // Ponte C++<->QML da barra de titulo (MainBar.qml). Executa min/max/fechar na janela e
    // fornece ao NativeWindowFilter as "zonas" da barra (altura + retangulos interativos),
    // equivalente ao GetWindowZoneForPoint do Slate: a QML reporta onde estao os controles
    // (menus/botoes) pra esses pontos virarem HTCLIENT, e o resto da faixa vira HTCAPTION.
    class WindowBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool maximized READ IsMaximized NOTIFY MaximizedChanged)

    public:
        explicit WindowBridge(QWidget* window, QObject* parent = nullptr);

        bool IsMaximized() const;

        // Lidos pelo NativeWindowFilter no WM_NCHITTEST (coords relativas a janela).
        int   TitleBarHeight() const { return TitleBarH; }
        bool  PointIsInteractive(int x, int y) const;
        qreal Dpr() const; // devicePixelRatio: converte fisico (hit-test) <-> logico (QML)

        // MainWindow chama em changeEvent quando o estado da janela muda (max/restore/snap).
        void NotifyWindowStateChanged();

    public slots:
        void minimize();
        void toggleMaximize();
        void close();
        void setTitleBarHeight(int h);
        void setInteractiveRects(const QVariantList& rects);

        // Arrasto/redimensiona via API nativa do Qt — funciona mesmo com janelas filhas nativas
        // (o WM_NCHITTEST nao alcanca o top-level por cima delas). startSystemMove da Aero Snap.
        void startSystemMove();
        void startSystemResize(int edges); // Qt::Edges

    signals:
        void MaximizedChanged();

    private:
        QWidget*       Window;
        int            TitleBarH = 0;
        QVector<QRect> InteractiveRects;
    };
}
