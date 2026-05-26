#include "SmileEditor/DarkTheme.h"
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QColor>
#include <QFile>
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
            StylesDir + "/MainWindow.qss",
            StylesDir + "/AboutDialog.qss",
            StylesDir + "/MaterialEditorPanel.qss",
            StylesDir + "/TextureSlotWidget.qss"
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
    }

    void ApplyDarkTheme(QApplication& _App) {
        _App.setStyle(QStyleFactory::create("Fusion"));

        QPalette Palette;

        const QColor WindowBg(30, 30, 30);          
        const QColor BaseBg(37, 37, 38);            
        const QColor AltBaseBg(45, 45, 48);         
        const QColor Text(220, 220, 220);           
        const QColor DisabledText(120, 120, 120);   
        const QColor Button(60, 60, 60);            
        const QColor Highlight(14, 99, 156);        
        const QColor HighlightText(255, 255, 255);
        const QColor Link(64, 156, 255);

        Palette.setColor(QPalette::Window,          WindowBg);
        Palette.setColor(QPalette::WindowText,      Text);
        Palette.setColor(QPalette::Base,            BaseBg);
        Palette.setColor(QPalette::AlternateBase,   AltBaseBg);
        Palette.setColor(QPalette::ToolTipBase,     BaseBg);
        Palette.setColor(QPalette::ToolTipText,     Text);
        Palette.setColor(QPalette::Text,            Text);
        Palette.setColor(QPalette::Button,          Button);
        Palette.setColor(QPalette::ButtonText,      Text);
        Palette.setColor(QPalette::BrightText,      Qt::red);
        Palette.setColor(QPalette::Link,            Link);
        Palette.setColor(QPalette::Highlight,       Highlight);
        Palette.setColor(QPalette::HighlightedText, HighlightText);
        Palette.setColor(QPalette::PlaceholderText, DisabledText);

        Palette.setColor(QPalette::Disabled, QPalette::WindowText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Text,            DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::ButtonText,      DisabledText);
        Palette.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(80, 80, 80));
        Palette.setColor(QPalette::Disabled, QPalette::HighlightedText, DisabledText);

        _App.setPalette(Palette);

        LoadAndApplyStylesheets(_App);
    }
}
 
