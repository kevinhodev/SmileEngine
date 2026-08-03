#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWidget>

class QCloseEvent;
class QMouseEvent;
class QPaintEvent;

namespace SmileEditor {
    // Janela de boot do editor: screenshot da cena em full-bleed + etapa corrente do
    // Renderer::Initialize. Vive do main() ate o renderer ficar pronto (ou a cena de boot
    // terminar de carregar). Padroes: arte + progresso sobre a imagem da Unreal, janela sem
    // borda e nao fechavel pelo usuario da Flax, status publicado por terceiros da CryEngine.
    class SplashScreen : public QWidget {
        Q_OBJECT

    public:
        explicit SplashScreen(QWidget* parent = nullptr);

        // Centraliza na tela do cursor, faz fade-in e forca a primeira pintura antes de a GUI
        // entrar no construtor pesado da MainWindow.
        void ShowCentered();

    public slots:
        // Fraction 0..1. Nunca retrocede: a barra so anda para frente, mesmo se uma etapa
        // reportar valor menor. Detail (opcional) vira o chip do adaptador.
        void SetStage(const QString& label, const QString& detail, qreal fraction);
        // Completa a barra, faz fade-out e fecha, trazendo a janela principal para a frente.
        void FinishWith(QWidget* mainWindow);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void closeEvent(QCloseEvent* event) override;   // ignora fechamento pelo usuario
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;

    private:
        void LoadArtwork();

        QPixmap       Artwork;                 // ja recortada/escalada para o tamanho final
        qreal         ArtworkRatio    = 0.0;   // DPR usado no recorte corrente
        QPixmap       Logo;
        QString       StageLabel;
        QString       AdapterText;
        qreal         TargetProgress  = 0.0;
        qreal         DisplayProgress = 0.0;   // segue TargetProgress com easing, por tick
        bool          Finishing       = false;
        QPoint        DragOffset;
        QTimer        Ticker;
        QElapsedTimer Clock;
    };
}
