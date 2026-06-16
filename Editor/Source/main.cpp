#include "SmileEditor/DarkTheme.h"
#include "SmileEditor/MainWindow.h"
#include <QApplication>
#include <QQuickStyle>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("SmileEditor");
    QApplication::setOrganizationName("SmileEngine");

    // Estilo Basic dos QtQuick.Controls: 100% customizavel, sem decoracoes nativas (setas,
    // groove, focus frame) que vazavam por baixo da ThinScrollBar. Deve vir antes de carregar QML.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    SmileEditor::ApplyDarkTheme(app);

    SmileEditor::MainWindow window;
    window.show();

    return app.exec();
}
