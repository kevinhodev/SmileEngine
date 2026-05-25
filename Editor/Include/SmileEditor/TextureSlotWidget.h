#pragma once

#include <QWidget>
#include <QPixmap>

class QLabel;
class QPushButton;

namespace SmileEditor {

// A single PBR texture slot row, inspired by CryEngine Sandbox's material panel.
// Shows a 48×48 thumbnail, slot name, truncated file path, Browse and Clear buttons.
class TextureSlotWidget : public QWidget {
    Q_OBJECT

public:
    explicit TextureSlotWidget(const QString& slotName, QWidget* parent = nullptr);

    void SetThumbnail(const QPixmap& pixmap);
    void ClearThumbnail(); // reverts to checkerboard (no texture)
    void SetPath(const QString& path);

signals:
    void BrowseRequested();
    void ClearRequested();

private:
    static QPixmap MakeCheckerboard(int size);

    QLabel*      ThumbnailLabel = nullptr;
    QLabel*      PathLabel      = nullptr;
    QPushButton* BrowseBtn      = nullptr;
    QPushButton* ClearBtn       = nullptr;
};

} // namespace SmileEditor
