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
                    color: #EDEDED;
                    background-color: #2E2E2E;
                    border: 1px solid #0F0F0F;
                }
                QMainWindow::separator {
                    background: #0F0F0F;
                    width: 2px;
                    height: 2px;
                }
            )");
        }
    }

    void ApplyDarkTheme(QApplication& _App) {
        _App.setStyle(QStyleFactory::create("Fusion"));

        QPalette Palette;

        // Paleta base do Fusion no conceito "Cartao 18%". Vale para os dialogos nativos
        // (abrir cena, mensagens) e para tudo que o QSS nao alcanca; os valores repetem os
        // tokens de Qml/components/Theme.qml, que o C++ nao consegue ler.
        const QColor WindowBg(0x21, 0x21, 0x21);
        const QColor BaseBg(0x17, 0x17, 0x17);
        const QColor AltBaseBg(0x1D, 0x1D, 0x1D);
        const QColor Text(0xD4, 0xD4, 0xD4);
        const QColor DisabledText(0x59, 0x59, 0x59);
        const QColor Button(0x2E, 0x2E, 0x2E);
        const QColor Highlight(0xF4, 0xC4, 0x30);
        const QColor HighlightText(0x1E, 0x1E, 0x1E);
        const QColor Link(0x7F, 0xB2, 0xEC);

        Palette.setColor(QPalette::Window,          WindowBg);
        Palette.setColor(QPalette::WindowText,      Text);
        Palette.setColor(QPalette::Base,            BaseBg);
        Palette.setColor(QPalette::AlternateBase,   AltBaseBg);
        Palette.setColor(QPalette::ToolTipBase,     BaseBg);
        Palette.setColor(QPalette::ToolTipText,     Text);
        Palette.setColor(QPalette::Text,            Text);
        Palette.setColor(QPalette::Button,          Button);
        Palette.setColor(QPalette::ButtonText,      Text);
        Palette.setColor(QPalette::BrightText,      QColor(0xF0, 0x52, 0x4F));
        Palette.setColor(QPalette::Link,            Link);
        Palette.setColor(QPalette::Highlight,       Highlight);
        Palette.setColor(QPalette::HighlightedText, HighlightText);
        Palette.setColor(QPalette::PlaceholderText, DisabledText);

        Palette.setColor(QPalette::Disabled, QPalette::WindowText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Text,            DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::ButtonText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(0x3C, 0x3C, 0x3C));
        Palette.setColor(QPalette::Disabled, QPalette::HighlightedText, DisabledText);

        _App.setPalette(Palette);

        LoadAndApplyStylesheets(_App);
    }
}
 
