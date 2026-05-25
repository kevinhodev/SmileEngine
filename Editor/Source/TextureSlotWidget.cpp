#include "SmileEditor/TextureSlotWidget.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QFileInfo>

namespace SmileEditor {

TextureSlotWidget::TextureSlotWidget(const QString& slotName, QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(64);

    // Outer frame styling — like CryEngine's panel slot rows
    setStyleSheet(
        "TextureSlotWidget {"
        "  background: #2a2a2e;"
        "  border: 1px solid #3c3c42;"
        "  border-radius: 4px;"
        "}"
    );

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(8);

    // Thumbnail
    ThumbnailLabel = new QLabel(this);
    ThumbnailLabel->setFixedSize(48, 48);
    ThumbnailLabel->setAlignment(Qt::AlignCenter);
    ThumbnailLabel->setStyleSheet(
        "border: 1px solid #555560; border-radius: 2px; background: transparent;");
    ClearThumbnail();
    root->addWidget(ThumbnailLabel);

    // Right column: name + path + buttons
    auto* right  = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(2);

    auto* nameLabel = new QLabel(slotName, this);
    nameLabel->setStyleSheet("color: #dcdcdc; font-weight: bold; font-size: 11px; border: none; background: transparent;");
    right->addWidget(nameLabel);

    PathLabel = new QLabel(tr("(nenhuma)"), this);
    PathLabel->setStyleSheet("color: #888; font-size: 10px; border: none; background: transparent;");
    PathLabel->setMaximumWidth(160);
    right->addWidget(PathLabel);

    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(4);

    BrowseBtn = new QPushButton(tr("Browse..."), this);
    BrowseBtn->setFixedHeight(20);
    BrowseBtn->setStyleSheet(
        "QPushButton { background: #3c3c3c; color: #dcdcdc; border: 1px solid #555; border-radius: 3px; font-size: 10px; padding: 0 6px; }"
        "QPushButton:hover { background: #4c4c4c; }"
        "QPushButton:pressed { background: #0e639c; }");

    ClearBtn = new QPushButton(tr("✕"), this);
    ClearBtn->setFixedSize(20, 20);
    ClearBtn->setToolTip(tr("Remover textura"));
    ClearBtn->setStyleSheet(
        "QPushButton { background: #3c3c3c; color: #aaa; border: 1px solid #555; border-radius: 3px; font-size: 11px; }"
        "QPushButton:hover { background: #8b1a1a; color: #fff; }");

    btnRow->addWidget(BrowseBtn);
    btnRow->addWidget(ClearBtn);
    btnRow->addStretch();
    right->addLayout(btnRow);

    root->addLayout(right);

    connect(BrowseBtn, &QPushButton::clicked, this, &TextureSlotWidget::BrowseRequested);
    connect(ClearBtn,  &QPushButton::clicked, this, &TextureSlotWidget::ClearRequested);
}

void TextureSlotWidget::SetThumbnail(const QPixmap& pixmap) {
    ThumbnailLabel->setPixmap(pixmap.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void TextureSlotWidget::ClearThumbnail() {
    ThumbnailLabel->setPixmap(MakeCheckerboard(48));
}

void TextureSlotWidget::SetPath(const QString& path) {
    if (path.isEmpty()) {
        PathLabel->setText(tr("(nenhuma)"));
        PathLabel->setToolTip(QString());
    } else {
        const QString name = QFileInfo(path).fileName();
        PathLabel->setText(name);
        PathLabel->setToolTip(path);
    }
}

// Generates a dark checkerboard pixmap to indicate "no texture" — same convention as Photoshop/UE.
QPixmap TextureSlotWidget::MakeCheckerboard(int size) {
    QPixmap pm(size, size);
    QPainter p(&pm);
    constexpr int Cell = 8;
    for (int y = 0; y < size; y += Cell) {
        for (int x = 0; x < size; x += Cell) {
            const bool dark = ((x / Cell) + (y / Cell)) % 2 == 0;
            p.fillRect(x, y, Cell, Cell, dark ? QColor(60, 60, 60) : QColor(40, 40, 40));
        }
    }
    return pm;
}

} // namespace SmileEditor
