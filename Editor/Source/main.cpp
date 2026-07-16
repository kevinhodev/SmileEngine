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
    // Dev/smoke: caminho de .sscene como 1o argumento carrega a cena no boot.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && args.at(1).endsWith(QStringLiteral(".sscene"), Qt::CaseInsensitive))
        window.SetStartupScene(args.at(1));
    window.show();

    return app.exec();
}
