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
        setAttribute(Qt::WA_StyledBackground, true);
        setObjectName("TextureSlotWidget");

        auto* RootLayout = new QHBoxLayout(this);
        RootLayout->setContentsMargins(6, 6, 6, 6);
        RootLayout->setSpacing(8);

        ThumbnailLabel = new QLabel(this);
        ThumbnailLabel->setObjectName("SlotThumbnail");
        ThumbnailLabel->setFixedSize(48, 48);
        ThumbnailLabel->setAlignment(Qt::AlignCenter);
        ClearThumbnail();
        RootLayout->addWidget(ThumbnailLabel);

        auto* RightColumn  = new QVBoxLayout;
        RightColumn->setContentsMargins(0, 0, 0, 0);
        RightColumn->setSpacing(2);

        auto* NameLabel = new QLabel(_SlotName, this);
        NameLabel->setObjectName("SlotNameLabel");
        RightColumn->addWidget(NameLabel);

        PathLabel = new QLabel(tr("(nenhuma)"), this);
        PathLabel->setObjectName("SlotPathLabel");
        PathLabel->setMaximumWidth(160);
        RightColumn->addWidget(PathLabel);

        auto* BtnRow = new QHBoxLayout;
        BtnRow->setContentsMargins(0, 0, 0, 0);
        BtnRow->setSpacing(4);

        BrowseBtn = new QPushButton(tr("Browse..."), this);
        BrowseBtn->setObjectName("SlotBrowseBtn");
        BrowseBtn->setFixedHeight(20);

        ClearBtn = new QPushButton(tr("✕"), this);
        ClearBtn->setObjectName("SlotClearBtn");
        ClearBtn->setFixedSize(20, 20);
        ClearBtn->setToolTip(tr("Remover textura"));

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
