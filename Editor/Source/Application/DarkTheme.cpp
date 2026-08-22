#include "SmileEditor/Application/DarkTheme.h"
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>

namespace SmileEditor {
    QString GetStylesDirectoryPath() {
#ifdef SMILE_EDITOR_SOURCE_DIR
        QString SourceDir = QStringLiteral(SMILE_EDITOR_SOURCE_DIR) + "/Styles";
        if (QDir(SourceDir).exists()) {
            return SourceDir;
        }
#endif
        return QDir(QCoreApplication::applicationDirPath()).filePath("Editor/Styles");
    }

    QStringList GetStylesheetFiles() {
        const QString StylesDir = GetStylesDirectoryPath();
        return QStringList {
            StylesDir + "/global.qss",
            StylesDir + "/MainWindow.qss"
        };
    }

    void LoadAndApplyStylesheets(QApplication& _App) {
        QString CombinedStyles;
        const QStringList QSSFiles = GetStylesheetFiles();
        bool AnyLoaded = false;

        for (const QString& Path : QSSFiles) {
            QFile File(Path);
            if (File.open(QFile::ReadOnly | QFile::Text)) {
                CombinedStyles += "\n/* --- " + QFileInfo(Path).fileName() + " --- */\n";
                CombinedStyles += File.readAll();
                AnyLoaded = true;
            }
        }

        if (AnyLoaded) {
            _App.setStyleSheet(CombinedStyles);
        } else {
            _App.setStyleSheet(R"(
                QToolTip {
                    color: #F2F2F0;
                    background-color: #18181B;
                    border: 1px solid #333338;
                }
                QMainWindow::separator {
                    background: #26262B;
                    width: 4px;
                    height: 4px;
                }
            )");
        }
    }

    void ApplyDarkTheme(QApplication& _App) {
        _App.setStyle(QStyleFactory::create("Fusion"));

        QPalette Palette;

        // Paleta base do Fusion no conceito "Signal Graphite". Vale para os dialogos nativos
        // (abrir cena, mensagens) e para tudo que o QSS nao alcanca; os valores repetem os
        // tokens de Qml/components/Theme.qml, que o C++ nao consegue ler.
        const QColor WindowBg(0x09, 0x09, 0x0A);
        const QColor BaseBg(0x11, 0x11, 0x13);
        const QColor AltBaseBg(0x18, 0x18, 0x1B);
        const QColor Text(0xC9, 0xC9, 0xCF);
        const QColor DisabledText(0x5F, 0x5F, 0x66);
        const QColor Button(0x18, 0x18, 0x1B);
        const QColor Highlight(0xFF, 0x6B, 0x2C);
        const QColor HighlightText(0x09, 0x09, 0x0A);
        const QColor Link(0x6F, 0xA8, 0xFF);

        Palette.setColor(QPalette::Window,          WindowBg);
        Palette.setColor(QPalette::WindowText,      Text);
        Palette.setColor(QPalette::Base,            BaseBg);
        Palette.setColor(QPalette::AlternateBase,   AltBaseBg);
        Palette.setColor(QPalette::ToolTipBase,     BaseBg);
        Palette.setColor(QPalette::ToolTipText,     Text);
        Palette.setColor(QPalette::Text,            Text);
        Palette.setColor(QPalette::Button,          Button);
        Palette.setColor(QPalette::ButtonText,      Text);
        Palette.setColor(QPalette::BrightText,      QColor(0xFF, 0x5C, 0x5C));
        Palette.setColor(QPalette::Link,            Link);
        Palette.setColor(QPalette::Highlight,       Highlight);
        Palette.setColor(QPalette::HighlightedText, HighlightText);
        Palette.setColor(QPalette::PlaceholderText, DisabledText);

        Palette.setColor(QPalette::Disabled, QPalette::WindowText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Text,            DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::ButtonText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(0x1F, 0x1F, 0x23));
        Palette.setColor(QPalette::Disabled, QPalette::HighlightedText, DisabledText);

        _App.setPalette(Palette);

        LoadAndApplyStylesheets(_App);
    }
}
 
