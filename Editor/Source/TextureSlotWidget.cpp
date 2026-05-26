#include "SmileEditor/TextureSlotWidget.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QFileInfo>

namespace SmileEditor {
    TextureSlotWidget::TextureSlotWidget(const QString& _SlotName, QWidget* _Parent)
        : QWidget(_Parent)
    {
        setFixedHeight(64);

        setStyleSheet(
            "TextureSlotWidget {"
            "  background: #2a2a2e;"
            "  border: 1px solid #3c3c42;"
            "  border-radius: 4px;"
            "}"
        );

        auto* RootLayout = new QHBoxLayout(this);
        RootLayout->setContentsMargins(6, 6, 6, 6);
        RootLayout->setSpacing(8);

        ThumbnailLabel = new QLabel(this);
        ThumbnailLabel->setFixedSize(48, 48);
        ThumbnailLabel->setAlignment(Qt::AlignCenter);
        ThumbnailLabel->setStyleSheet(
            "border: 1px solid #555560; border-radius: 2px; background: transparent;");
        ClearThumbnail();
        RootLayout->addWidget(ThumbnailLabel);

        auto* RightColumn  = new QVBoxLayout;
        RightColumn->setContentsMargins(0, 0, 0, 0);
        RightColumn->setSpacing(2);

        auto* NameLabel = new QLabel(_SlotName, this);
        NameLabel->setStyleSheet("color: #dcdcdc; font-weight: bold; font-size: 11px; border: none; background: transparent;");
        RightColumn->addWidget(NameLabel);

        PathLabel = new QLabel(tr("(nenhuma)"), this);
        PathLabel->setStyleSheet("color: #888; font-size: 10px; border: none; background: transparent;");
        PathLabel->setMaximumWidth(160);
        RightColumn->addWidget(PathLabel);

        auto* BtnRow = new QHBoxLayout;
        BtnRow->setContentsMargins(0, 0, 0, 0);
        BtnRow->setSpacing(4);

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

        BtnRow->addWidget(BrowseBtn);
        BtnRow->addWidget(ClearBtn);
        BtnRow->addStretch();
        RightColumn->addLayout(BtnRow);

        RootLayout->addLayout(RightColumn);

        connect(BrowseBtn, &QPushButton::clicked, this, &TextureSlotWidget::BrowseRequested);
        connect(ClearBtn,  &QPushButton::clicked, this, &TextureSlotWidget::ClearRequested);
    }

    void TextureSlotWidget::SetThumbnail(const QPixmap& _Pixmap) {
        ThumbnailLabel->setPixmap(_Pixmap.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    void TextureSlotWidget::ClearThumbnail() {
        ThumbnailLabel->setPixmap(MakeCheckerboard(48));
    }

    void TextureSlotWidget::SetPath(const QString& _Path) {
        if (_Path.isEmpty()) {
            PathLabel->setText(tr("(nenhuma)"));
            PathLabel->setToolTip(QString());
        } else {
            const QString name = QFileInfo(_Path).fileName();
            PathLabel->setText(name);
            PathLabel->setToolTip(_Path);
        }
    }

    QPixmap TextureSlotWidget::MakeCheckerboard(int _Size) {
        QPixmap Pixmap(_Size, _Size);
        QPainter Painter(&Pixmap);
        constexpr int Cell = 8;
        for (int y = 0; y < _Size; y += Cell) {
            for (int x = 0; x < _Size; x += Cell) {
                const bool Dark = ((x / Cell) + (y / Cell)) % 2 == 0;
                Painter.fillRect(x, y, Cell, Cell, Dark ? QColor(60, 60, 60) : QColor(40, 40, 40));
            }
        }
        return Pixmap;
    }
} 
