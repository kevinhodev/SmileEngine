// Ponto de entrada do SmileEditor.

#include "SmileEditor/DarkTheme.h"
#include "SmileEditor/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("SmileEditor");
    QApplication::setOrganizationName("SmileEngine");

    SmileEditor::ApplyDarkTheme(app);

    SmileEditor::MainWindow window;
    window.show();

    return app.exec();
}
