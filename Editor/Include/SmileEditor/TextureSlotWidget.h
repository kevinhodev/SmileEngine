#pragma once

#include <QWidget>
#include <QPixmap>

class QLabel;
class QPushButton;

namespace SmileEditor {
    class TextureSlotWidget : public QWidget {
        Q_OBJECT

    public:
        explicit TextureSlotWidget(const QString& SlotName, QWidget* Parent = nullptr);

        void SetThumbnail(const QPixmap& Pixmap);
        void ClearThumbnail(); 
        void SetPath(const QString& Path);

    signals:
        void BrowseRequested();
        void ClearRequested();

    private:
        static QPixmap MakeCheckerboard(int Size);

        QLabel*      ThumbnailLabel = nullptr;
        QLabel*      PathLabel      = nullptr;
        QPushButton* BrowseBtn      = nullptr;
        QPushButton* ClearBtn       = nullptr;
    };
} 
