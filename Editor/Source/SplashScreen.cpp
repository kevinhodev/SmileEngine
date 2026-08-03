#include "SmileEditor/SplashScreen.h"
#include "SmileEditor/SmileLogo.h"
#include "Smile/Core/VersionInfo.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QEasingCurve>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QScreen>
#include <QtGlobal>
#include <QtMath>

namespace SmileEditor {
    namespace {
        // Medidas logicas (multiplicadas pelo devicePixelRatio na pintura). Batem 1:1 com
        // Editor/Design/SplashScreen_Reference.svg — mexer aqui pede mexer la.
        constexpr int kWidth  = 800;
        constexpr int kHeight = 450;
        constexpr qreal kCorner      = 6.0;
        constexpr qreal kMarginLeft  = 34.0;
        constexpr qreal kMarginRight = 766.0;
        constexpr qreal kBarHeight   = 3.0;

        const QColor kAccent    (0xD9, 0xA5, 0x14);
        const QColor kAccentLead(0xFF, 0xE9, 0xA8);
        const QColor kTextStrong(0xF7, 0xF2, 0xE6);
        const QColor kTextNormal(0xDD, 0xD8, 0xCA);
        const QColor kTextMuted (0x8A, 0x85, 0x7A);
        const QColor kTextDim   (0x6C, 0x6A, 0x61);
        const QColor kBorder    (0x2E, 0x2F, 0x28);

        QString ArtworkPath() {
        #ifdef SMILE_EDITOR_SOURCE_DIR
            const QString SourceCopy =
                QStringLiteral(SMILE_EDITOR_SOURCE_DIR "/Resources/splash_bg.png");
            if (QFile::exists(SourceCopy)) return SourceCopy;
        #endif
            return QDir(QCoreApplication::applicationDirPath())
                       .filePath(QStringLiteral("Editor/Resources/splash_bg.png"));
        }

        QFont UIFont(qreal PointlessPixelSize, QFont::Weight Weight = QFont::Normal) {
            QFont Font(QStringLiteral("Inter"));
            Font.setPixelSize(qRound(PointlessPixelSize));
            Font.setWeight(Weight);
            return Font;
        }

        QFont MonoFont(qreal PixelSize) {
            QFont Font(QStringLiteral("Consolas"));
            Font.setStyleHint(QFont::Monospace);
            Font.setPixelSize(qRound(PixelSize));
            return Font;
        }

        // Texto sobre foto precisa de sombra: o screenshot pode ter uma parede clara embaixo
        // de qualquer linha. Uma copia preta 1px abaixo custa nada e resolve.
        void DrawShadowText(QPainter& Painter, const QPointF& Baseline, const QString& Text,
                            const QColor& Color) {
            Painter.setPen(QColor(0, 0, 0, 190));
            Painter.drawText(Baseline + QPointF(0.0, 1.0), Text);
            Painter.setPen(Color);
            Painter.drawText(Baseline, Text);
        }

        void DrawShadowTextRight(QPainter& Painter, const QRectF& Bounds, const QString& Text,
                                 const QColor& Color) {
            Painter.setPen(QColor(0, 0, 0, 190));
            Painter.drawText(Bounds.translated(0.0, 1.0), Qt::AlignRight | Qt::AlignVCenter, Text);
            Painter.setPen(Color);
            Painter.drawText(Bounds, Qt::AlignRight | Qt::AlignVCenter, Text);
        }
    }

    SplashScreen::SplashScreen(QWidget* _Parent)
        // Topmost pelo tempo do boot: a MainWindow precisa aparecer cedo (o viewport so cria o
        // device D3D12 quando tem HWND), entao sem isto a splash cairia atras dela.
        : QWidget(_Parent, Qt::SplashScreen | Qt::FramelessWindowHint |
                           Qt::WindowStaysOnTopHint)
    {
        setObjectName(QStringLiteral("SmileSplashScreen"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose, false);
        setFixedSize(kWidth, kHeight);
        setWindowTitle(QStringLiteral("SmileEngine"));

        StageLabel = tr("Iniciando o editor…");
        LoadArtwork();
        // Sem o badge escuro: sobre a arte a marca dourada flutua, como no About.
        Logo = MakeSmileLogoPixmap(qRound(52.0 * devicePixelRatioF()), /*IncludeBadge=*/false);
        Logo.setDevicePixelRatio(devicePixelRatioF());

        // 60 Hz proprio: a barra tem easing e o ponto pulsa mesmo quando nenhuma etapa nova
        // chega — sem isto a splash parece travada durante uma etapa longa (PSOs, DXR).
        Ticker.setInterval(16);
        connect(&Ticker, &QTimer::timeout, this, [this]() {
            const qreal Delta = TargetProgress - DisplayProgress;
            if (qAbs(Delta) > 0.0005) DisplayProgress += Delta * 0.18;
            update();
        });
        Clock.start();
        Ticker.start();
    }

    void SplashScreen::LoadArtwork() {
        const qreal Ratio = devicePixelRatioF();
        if (!Artwork.isNull() && qFuzzyCompare(ArtworkRatio, Ratio)) return;
        ArtworkRatio = Ratio;

        QPixmap Source(ArtworkPath());
        if (Source.isNull()) return;   // fallback de gradiente no paintEvent

        // Cobre a janela inteira e recorta o excedente pelo centro: o screenshot pode ter
        // qualquer proporcao (o da Emerald Square e ~2.03:1 contra 16:9 daqui).
        const QSize DeviceSize(qRound(kWidth * Ratio), qRound(kHeight * Ratio));
        const QPixmap Covered = Source.scaled(DeviceSize, Qt::KeepAspectRatioByExpanding,
                                              Qt::SmoothTransformation);
        const int OffsetX = (Covered.width()  - DeviceSize.width())  / 2;
        const int OffsetY = (Covered.height() - DeviceSize.height()) / 2;
        Artwork = Covered.copy(OffsetX, OffsetY, DeviceSize.width(), DeviceSize.height());
        Artwork.setDevicePixelRatio(Ratio);
    }

    void SplashScreen::ShowCentered() {
        const QScreen* Screen = QGuiApplication::screenAt(QCursor::pos());
        if (!Screen) Screen = QGuiApplication::primaryScreen();
        if (Screen) {
            const QRect Available = Screen->availableGeometry();
            move(Available.center() - QPoint(width() / 2, height() / 2));
        }
        // A tela do cursor pode ter DPI diferente da primaria usada no construtor: recarrega
        // a arte na escala certa antes da primeira pintura.
        LoadArtwork();

        setWindowOpacity(0.0);
        show();
        raise();

        auto* FadeIn = new QPropertyAnimation(this, "windowOpacity", this);
        FadeIn->setDuration(160);
        FadeIn->setStartValue(0.0);
        FadeIn->setEndValue(1.0);
        FadeIn->setEasingCurve(QEasingCurve::OutQuad);
        FadeIn->start(QAbstractAnimation::DeleteWhenStopped);

        // Truque da CryEngine: garante a primeira pintura antes de a GUI entrar no construtor
        // da MainWindow, que nao devolve o controle ao event loop por varias centenas de ms.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    void SplashScreen::SetStage(const QString& _Label, const QString& _Detail, qreal _Fraction) {
        if (Finishing) return;
        if (!_Label.isEmpty())  StageLabel  = _Label;
        if (!_Detail.isEmpty()) AdapterText = _Detail;
        TargetProgress = qBound(TargetProgress, _Fraction, 1.0);
        update();
    }

    void SplashScreen::FinishWith(QWidget* _MainWindow) {
        if (Finishing) return;
        Finishing = true;
        TargetProgress = 1.0;

        auto* FadeOut = new QPropertyAnimation(this, "windowOpacity", this);
        FadeOut->setDuration(220);
        FadeOut->setStartValue(windowOpacity());
        FadeOut->setEndValue(0.0);
        FadeOut->setEasingCurve(QEasingCurve::InQuad);
        connect(FadeOut, &QPropertyAnimation::finished, this, [this]() {
            Ticker.stop();
            close();
        });
        FadeOut->start(QAbstractAnimation::DeleteWhenStopped);

        if (_MainWindow) {
            _MainWindow->raise();
            _MainWindow->activateWindow();
        }
    }

    void SplashScreen::closeEvent(QCloseEvent* _Event) {
        // Alt+F4 no meio do boot deixaria o editor subindo sem nenhum feedback: so o
        // FinishWith fecha esta janela.
        if (!Finishing) {
            _Event->ignore();
            return;
        }
        QWidget::closeEvent(_Event);
    }

    void SplashScreen::mousePressEvent(QMouseEvent* _Event) {
        if (_Event->button() == Qt::LeftButton)
            DragOffset = _Event->globalPosition().toPoint() - frameGeometry().topLeft();
        QWidget::mousePressEvent(_Event);
    }

    void SplashScreen::mouseMoveEvent(QMouseEvent* _Event) {
        if (_Event->buttons() & Qt::LeftButton)
            move(_Event->globalPosition().toPoint() - DragOffset);
        QWidget::mouseMoveEvent(_Event);
    }

    void SplashScreen::paintEvent(QPaintEvent*) {
        QPainter Painter(this);
        Painter.setRenderHint(QPainter::Antialiasing, true);
        Painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        Painter.setRenderHint(QPainter::TextAntialiasing, true);

        const QRectF Bounds(0.0, 0.0, kWidth, kHeight);
        QPainterPath Rounded;
        Rounded.addRoundedRect(Bounds, kCorner, kCorner);
        Painter.setClipPath(Rounded);

        if (!Artwork.isNull()) {
            Painter.drawPixmap(QPointF(0.0, 0.0), Artwork);
        } else {
            // Sem a arte a splash continua valida: gradiente do tema + brilho quente atras
            // da marca. Boot nunca depende de um asset opcional.
            QLinearGradient Fallback(0.0, 0.0, 0.0, kHeight);
            Fallback.setColorAt(0.0, QColor(0x1A, 0x1B, 0x15));
            Fallback.setColorAt(1.0, QColor(0x0B, 0x0C, 0x09));
            Painter.fillRect(Bounds, Fallback);

            QRadialGradient Warm(QPointF(120.0, 346.0), 220.0);
            Warm.setColorAt(0.0, QColor(0xD9, 0xA5, 0x14, 46));
            Warm.setColorAt(1.0, QColor(0xD9, 0xA5, 0x14, 0));
            Painter.fillRect(Bounds, Warm);
        }

        // Scrim: o rodape precisa de contraste garantido, independente do screenshot.
        QLinearGradient Scrim(0.0, kHeight * 0.33, 0.0, kHeight);
        Scrim.setColorAt(0.0,  QColor(5, 6, 10, 0));
        Scrim.setColorAt(0.42, QColor(5, 6, 10, 107));
        Scrim.setColorAt(0.72, QColor(5, 6, 10, 209));
        Scrim.setColorAt(1.0,  QColor(4, 5, 8, 242));
        Painter.fillRect(Bounds, Scrim);

        QRadialGradient Vignette(QPointF(kWidth * 0.5, kHeight * 0.44), kWidth * 0.78);
        Vignette.setColorAt(0.45, QColor(0, 0, 0, 0));
        Vignette.setColorAt(1.0,  QColor(0, 0, 0, 158));
        Painter.fillRect(Bounds, Vignette);

        // Chip do adaptador (aparece quando o D3D12 device nomeia a GPU)
        if (!AdapterText.isEmpty()) {
            Painter.setFont(UIFont(9.5));
            const qreal ChipWidth = Painter.fontMetrics().horizontalAdvance(AdapterText) + 20.0;
            const QRectF Chip(kMarginLeft, 22.0, ChipWidth, 20.0);
            Painter.setPen(Qt::NoPen);
            Painter.setBrush(QColor(5, 6, 10, 153));
            Painter.drawRoundedRect(Chip, 10.0, 10.0);
            Painter.setBrush(Qt::NoBrush);
            Painter.setPen(QColor(255, 255, 255, 31));
            Painter.drawRoundedRect(Chip, 10.0, 10.0);
            Painter.setPen(kTextNormal);
            Painter.drawText(Chip, Qt::AlignCenter, AdapterText);
        }

        // Credito da arte (a cena e do pacote ORCA, cedida pela Amazon Lumberyard)
        Painter.setFont(UIFont(8.5));
        Painter.setPen(QColor(0xE6, 0xE2, 0xD8, 97));
        Painter.drawText(QRectF(kMarginRight - 300.0, 20.0, 300.0, 14.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         tr("cena — Bistro · Amazon Lumberyard (ORCA)"));

        // Identidade
        Painter.drawPixmap(QPointF(kMarginLeft, 320.0), Logo);

        Painter.setFont(UIFont(30.0, QFont::DemiBold));
        DrawShadowText(Painter, QPointF(102.0, 352.0), QStringLiteral("SmileEngine"), kTextStrong);

        QFont Tagline = UIFont(9.0, QFont::DemiBold);
        Tagline.setLetterSpacing(QFont::AbsoluteSpacing, 2.8);
        Painter.setFont(Tagline);
        DrawShadowText(Painter, QPointF(104.0, 371.0), tr("REAL-TIME 3D ENGINE"), kAccent);

        // Build (mesma fonte do About: numero de versao em monoespacada)
        Painter.setFont(MonoFont(10.5));
        DrawShadowTextRight(Painter, QRectF(kMarginRight - 340.0, 338.0, 340.0, 14.0),
                            QStringLiteral("v%1 · build %2")
                                .arg(QStringLiteral(SMILE_VERSION_STRING),
                                     QStringLiteral(SMILE_BUILD_NUMBER)),
                            QColor(0xC2, 0xBB, 0xAB));
        Painter.setFont(UIFont(9.5));
        DrawShadowTextRight(Painter, QRectF(kMarginRight - 340.0, 354.0, 340.0, 14.0),
                            QStringLiteral("Qt %1 · DirectX 12 · DXC")
                                .arg(QString::fromLatin1(qVersion())),
                            kTextMuted);
        Painter.setFont(UIFont(9.0));
        DrawShadowTextRight(Painter, QRectF(kMarginRight - 340.0, 370.0, 340.0, 14.0),
                            tr("© 2026 Kevin Ramos"), kTextDim);

        Painter.setPen(QColor(255, 255, 255, 20));
        Painter.drawLine(QPointF(kMarginLeft, 398.0), QPointF(kMarginRight, 398.0));

        // Etapa corrente: o ponto pulsa para a janela nunca parecer congelada.
        const qreal Pulse = 0.55 + 0.45 * qSin(Clock.elapsed() * 0.004);
        Painter.setPen(Qt::NoPen);
        Painter.setBrush(QColor(kAccent.red(), kAccent.green(), kAccent.blue(),
                                qRound(46 * Pulse)));
        Painter.drawEllipse(QPointF(38.0, 419.0), 7.0, 7.0);
        Painter.setBrush(kAccent);
        Painter.drawEllipse(QPointF(38.0, 419.0), 3.2, 3.2);
        Painter.setBrush(Qt::NoBrush);

        Painter.setFont(UIFont(11.5));
        DrawShadowText(Painter, QPointF(52.0, 423.0), StageLabel, kTextNormal);

        Painter.setFont(MonoFont(10.5));
        DrawShadowTextRight(Painter, QRectF(kMarginRight - 120.0, 412.0, 120.0, 14.0),
                            QStringLiteral("%1%").arg(qRound(DisplayProgress * 100.0)),
                            kTextMuted);

        // Barra de progresso na borda inferior
        const QRectF Track(0.0, kHeight - kBarHeight, kWidth, kBarHeight);
        Painter.fillRect(Track, QColor(255, 255, 255, 23));
        const qreal FillWidth = kWidth * qBound(0.0, DisplayProgress, 1.0);
        if (FillWidth > 0.0) {
            QLinearGradient Gold(0.0, 0.0, FillWidth, 0.0);
            Gold.setColorAt(0.0, QColor(0x7A, 0x53, 0x05));
            Gold.setColorAt(0.6, kAccent);
            Gold.setColorAt(1.0, QColor(0xFF, 0xE0, 0x8A));
            Painter.fillRect(QRectF(0.0, Track.top(), FillWidth, kBarHeight), Gold);
            if (FillWidth > 24.0 && DisplayProgress < 0.999)
                Painter.fillRect(QRectF(FillWidth - 24.0, Track.top(), 24.0, kBarHeight),
                                 kAccentLead);
        }

        Painter.setClipping(false);
        Painter.setPen(kBorder);
        Painter.setBrush(Qt::NoBrush);
        Painter.drawPath(Rounded);
    }
}
