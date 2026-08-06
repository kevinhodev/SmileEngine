#include "SmileEditor/DarkTheme.h"
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
                    color: #ddd8ca;
                    background-color: #151512;
                    border: 1px solid #3a3321;
                }
                QMainWindow::separator {
                    background: #20211f;
                    width: 1px;
                    height: 1px;
                }
                QDockWidget::title {
                    background: #171816;
                    padding: 4px;
                    border-bottom: 1px solid #292820;
                }
            )");
        }
    }

    void ApplyDarkTheme(QApplication& _App) {
        _App.setStyle(QStyleFactory::create("Fusion"));

        QPalette Palette;

        const QColor WindowBg(8, 9, 9);
        const QColor BaseBg(16, 17, 15);
        const QColor AltBaseBg(24, 25, 22);
        const QColor Text(221, 216, 202);
        const QColor DisabledText(102, 99, 91);
        const QColor Button(25, 26, 23);
        const QColor Highlight(212, 154, 17);
        const QColor HighlightText(10, 10, 9);
        const QColor Link(255, 211, 93);

        Palette.setColor(QPalette::Window,          WindowBg);
        Palette.setColor(QPalette::WindowText,      Text);
        Palette.setColor(QPalette::Base,            BaseBg);
        Palette.setColor(QPalette::AlternateBase,   AltBaseBg);
        Palette.setColor(QPalette::ToolTipBase,     BaseBg);
        Palette.setColor(QPalette::ToolTipText,     Text);
        Palette.setColor(QPalette::Text,            Text);
        Palette.setColor(QPalette::Button,          Button);
        Palette.setColor(QPalette::ButtonText,      Text);
        Palette.setColor(QPalette::BrightText,      QColor(255, 95, 87));
        Palette.setColor(QPalette::Link,            Link);
        Palette.setColor(QPalette::Highlight,       Highlight);
        Palette.setColor(QPalette::HighlightedText, HighlightText);
        Palette.setColor(QPalette::PlaceholderText, DisabledText);

        Palette.setColor(QPalette::Disabled, QPalette::WindowText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Text,            DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::ButtonText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(58, 52, 35));
        Palette.setColor(QPalette::Disabled, QPalette::HighlightedText, DisabledText);

        _App.setPalette(Palette);

        LoadAndApplyStylesheets(_App);
    }
}
 
