#include "SmileEditor/DarkTheme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QColor>

namespace SmileEditor {

void ApplyDarkTheme(QApplication& app) {
    // Estilo Fusion respeita a paleta de forma consistente em todas as plataformas.
    // O estilo nativo do Windows ignora cores customizadas em muitos widgets.
    app.setStyle(QStyleFactory::create("Fusion"));

    // Paleta inspirada em editores modernos (VSCode/Material dark)
    QPalette p;

    const QColor windowBg(30, 30, 30);          // #1E1E1E
    const QColor baseBg(37, 37, 38);            // #252526
    const QColor altBaseBg(45, 45, 48);         // #2D2D30
    const QColor text(220, 220, 220);           // #DCDCDC
    const QColor disabledText(120, 120, 120);   // #787878
    const QColor button(60, 60, 60);            // #3C3C3C
    const QColor highlight(14, 99, 156);        // #0E639C (azul VSCode)
    const QColor highlightText(255, 255, 255);
    const QColor link(64, 156, 255);

    p.setColor(QPalette::Window,          windowBg);
    p.setColor(QPalette::WindowText,      text);
    p.setColor(QPalette::Base,            baseBg);
    p.setColor(QPalette::AlternateBase,   altBaseBg);
    p.setColor(QPalette::ToolTipBase,     baseBg);
    p.setColor(QPalette::ToolTipText,     text);
    p.setColor(QPalette::Text,            text);
    p.setColor(QPalette::Button,          button);
    p.setColor(QPalette::ButtonText,      text);
    p.setColor(QPalette::BrightText,      Qt::red);
    p.setColor(QPalette::Link,            link);
    p.setColor(QPalette::Highlight,       highlight);
    p.setColor(QPalette::HighlightedText, highlightText);
    p.setColor(QPalette::PlaceholderText, disabledText);

    // Estados desabilitados precisam ser definidos separadamente
    p.setColor(QPalette::Disabled, QPalette::WindowText,      disabledText);
    p.setColor(QPalette::Disabled, QPalette::Text,            disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText,      disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(80, 80, 80));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);

    app.setPalette(p);

    // Stylesheet minimo: tooltip e separadores ignoram a paleta em alguns estilos
    app.setStyleSheet(R"(
        QToolTip {
            color: #DCDCDC;
            background-color: #252526;
            border: 1px solid #3C3C3C;
        }
        QMainWindow::separator {
            background: #3C3C3C;
            width: 1px;
            height: 1px;
        }
        QDockWidget::title {
            background: #2D2D30;
            padding: 4px;
            border-bottom: 1px solid #3C3C3C;
        }
    )");
}

} // namespace SmileEditor
