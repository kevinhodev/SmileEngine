#include "SmileEditor/AboutDialog.h"
#include "Smile/Core/VersionInfo.h"
#include <QApplication>
#include <QClipboard>
#include <QEasingCurve>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSysInfo>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QtCore/qglobal.h>

namespace SmileEditor {
    namespace {
        constexpr const char* kAccent     = "#0E639C";
        constexpr const char* kAccentDark = "#094771";
        constexpr const char* kBgWindow   = "#1E1E1E";
        constexpr const char* kBgPanel    = "#252526";
        constexpr const char* kBgPanelAlt = "#2D2D30";
        constexpr const char* kBorder     = "#3C3C3C";
        constexpr const char* kText       = "#DCDCDC";
        constexpr const char* kTextMuted  = "#9A9A9A";

        QString CompilerString() {
        #if defined(_MSC_VER)
            const int Major = _MSC_VER / 100;
            const int Minor = _MSC_VER % 100;
            return QStringLiteral("MSVC %1.%2").arg(Major).arg(Minor);
        #elif defined(__clang__)
            return QStringLiteral("Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
        #elif defined(__GNUC__)
            return QStringLiteral("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
        #else
            return QStringLiteral("compilador desconhecido");
        #endif
        }

        QString FormatGigabytes(quint64 _Bytes) {
            const double gb = static_cast<double>(_Bytes) / (1024.0 * 1024.0 * 1024.0);
            return QString::number(gb, 'f', 1) + QStringLiteral(" GB");
        }
    } 

    AboutDialog::AboutDialog(QString _GPUDescription, QWidget* _Parent)
        : QDialog(_Parent),
          GPUDescription(std::move(_GPUDescription))
    {
        setWindowTitle(tr("Sobre o SmileEngine"));
        setModal(true);
        setFixedSize(620, 500);

        setStyleSheet(QString(R"(
            QDialog { background: %1; }
            QLabel { color: %5; }
            QTabWidget::pane {
                background: %2;
                border: 1px solid %4;
                border-top: 2px solid %7;
            }
            QTabBar::tab {
                background: %3;
                color: %6;
                padding: 7px 18px;
                border: 1px solid %4;
                border-bottom: none;
                margin-right: 2px;
            }
            QTabBar::tab:selected { background: %7; color: white; }
            QTabBar::tab:hover:!selected { background: %4; color: %5; }
            QTextBrowser {
                background: %2;
                color: %5;
                border: none;
                padding: 14px;
                selection-background-color: %7;
                selection-color: white;
            }
            QPushButton {
                background: %3;
                color: %5;
                padding: 7px 22px;
                border: 1px solid %4;
                border-radius: 2px;
                min-width: 80px;
            }
            QPushButton:hover  { background: %7; border-color: %7; color: white; }
            QPushButton:pressed { background: %8; border-color: %8; }
        )")
            .arg(kBgWindow,    kBgPanel,    kBgPanelAlt, kBorder,
                 kText,        kTextMuted,  kAccent,     kAccentDark));

        auto* RootLayout = new QHBoxLayout(this);
        RootLayout->setContentsMargins(0, 0, 0, 0);
        RootLayout->setSpacing(0);

        auto* AccentStrip = new QFrame(this);
        AccentStrip->setFixedWidth(4);
        AccentStrip->setStyleSheet(QStringLiteral("background: %1;").arg(kAccent));
        RootLayout->addWidget(AccentStrip);

        auto* ContentLayout = new QVBoxLayout();
        ContentLayout->setContentsMargins(0, 0, 0, 0);
        ContentLayout->setSpacing(0);
        RootLayout->addLayout(ContentLayout, 1);

        ContentLayout->addWidget(BuildHeader());

        auto* Tabs = new QTabWidget(this);
        Tabs->addTab(BuildAboutTab(),    tr("About"));
        Tabs->addTab(BuildSystemTab(),   tr("System Info"));
        Tabs->addTab(BuildCreditsTab(),  tr("Credits"));
        Tabs->addTab(BuildLicenseTab(),  tr("License"));
        ContentLayout->addWidget(Tabs, 1);

        auto* Footer = new QHBoxLayout();
        Footer->setContentsMargins(16, 12, 16, 14);
        Footer->setSpacing(8);

        auto* CopyButton = new QPushButton(tr("Copy info"), this);
        CopyButton->setToolTip(tr("Copia versao e informacoes de sistema para o clipboard"));
        connect(CopyButton, &QPushButton::clicked, this, &AboutDialog::OnCopyInfo);
        Footer->addWidget(CopyButton);

        Footer->addStretch(1);

        auto* CloseButton = new QPushButton(tr("Close"), this);
        CloseButton->setDefault(true);
        connect(CloseButton, &QPushButton::clicked, this, &QDialog::accept);
        Footer->addWidget(CloseButton);

        ContentLayout->addLayout(Footer);

        setWindowOpacity(0.0);
        QTimer::singleShot(0, this, [this]{
            auto* PropertyAnimation = new QPropertyAnimation(this, "windowOpacity", this);
            PropertyAnimation->setDuration(180);
            PropertyAnimation->setStartValue(0.0);
            PropertyAnimation->setEndValue(1.0);
            PropertyAnimation->setEasingCurve(QEasingCurve::OutQuad);
            PropertyAnimation->start(QAbstractAnimation::DeleteWhenStopped);
        });
    }

    QWidget* AboutDialog::BuildHeader() {
        auto* Header = new QWidget(this);
        Header->setFixedHeight(96);
        Header->setStyleSheet(QStringLiteral("background: %1;").arg(kBgPanelAlt));

        auto* Layout = new QHBoxLayout(Header);
        Layout->setContentsMargins(20, 14, 20, 14);
        Layout->setSpacing(18);

        auto* Logo = new QLabel(QStringLiteral("S"), Header);
        Logo->setFixedSize(64, 64);
        Logo->setAlignment(Qt::AlignCenter);
        Logo->setStyleSheet(QString(
            "background: %1; color: white; font-size: 40px; font-weight: bold;"
            "border-radius: 6px;").arg(kAccent));
        Layout->addWidget(Logo);

        auto* TextBox = new QVBoxLayout();
        TextBox->setContentsMargins(0, 0, 0, 0);
        TextBox->setSpacing(2);

        auto* Title = new QLabel(QStringLiteral("SmileEngine"), Header);
        Title->setStyleSheet(QString("color: %1; font-size: 24px; font-weight: 600;").arg(kText));
        TextBox->addWidget(Title);

        auto* Subtitle = new QLabel(tr("Game Engine"), Header);
        Subtitle->setStyleSheet(QString("color: %1; font-size: 11px; letter-spacing: 1px;").arg(kTextMuted));
        TextBox->addWidget(Subtitle);

        auto* Version = new QLabel(
            QStringLiteral("v%1 · build %2")
                .arg(QStringLiteral(SMILE_VERSION_STRING))
                .arg(QStringLiteral(SMILE_BUILD_NUMBER)),
            Header);
        Version->setStyleSheet(QString(
            "color: %1; font-family: 'Consolas', 'Cascadia Mono', monospace; font-size: 10px;"
        ).arg(kTextMuted));
        TextBox->addWidget(Version);

        Layout->addLayout(TextBox);
        Layout->addStretch(1);

        return Header;
    }

    QWidget* AboutDialog::BuildAboutTab() {
        auto* Browser = new QTextBrowser(this);
        Browser->setOpenExternalLinks(true);
        Browser->setHtml(QStringLiteral(R"(
    <p style="font-size:11pt; line-height:150%%;">
    <b>SmileEngine</b> eh uma game engine pessoal em desenvolvimento, focada em
    construir uma base moderna para experimentacao em renderizacao tempo-real
    e ferramentas de editor.
    </p>

    <p style="font-size:10pt; line-height:160%%; color:#9A9A9A;">
    Powered by <span style="color:%1;"><b>DirectX 12</b></span> e
    <span style="color:%1;"><b>Qt 6</b></span>.<br/>
    Construida com C++20, CMake e o compilador shader DXC.
    </p>

    <p style="font-size:9pt; color:#9A9A9A; margin-top:18px;">
    Copyright &copy; 2026 Kevin Ramos. Todos os direitos reservados.
    </p>
    )").arg(kAccent));
        return Browser;
    }

    QWidget* AboutDialog::BuildSystemTab() {
        auto* Browser = new QTextBrowser(this);
        Browser->setOpenExternalLinks(false);

        QString Rows;
        auto AddRow = [&](const QString& key, const QString& val) {
            Rows += QStringLiteral(
                "<tr>"
                "<td style='padding:4px 18px 4px 0; color:%1; font-weight:600;'>%2</td>"
                "<td style='padding:4px 0; color:%3; font-family:Consolas,monospace;'>%4</td>"
                "</tr>"
            ).arg(kAccent, key.toHtmlEscaped(), kText, val.toHtmlEscaped());
        };

        AddRow(tr("Version"),       QStringLiteral(SMILE_VERSION_STRING));
        AddRow(tr("Build number"),  QStringLiteral(SMILE_BUILD_NUMBER));
        AddRow(tr("Build date"),    QStringLiteral(SMILE_BUILD_DATE) + QStringLiteral(" UTC"));
        AddRow(tr("Compiler"),      CompilerString());
        AddRow(tr("Qt (build)"),    QStringLiteral(QT_VERSION_STR));
        AddRow(tr("Qt (runtime)"),  QString::fromLatin1(qVersion()));
        AddRow(tr("OS"),            QSysInfo::prettyProductName());
        AddRow(tr("Kernel"),        QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion());
        AddRow(tr("Architecture"),  QSysInfo::buildCpuArchitecture());
        AddRow(tr("GPU adapter"),   GPUDescription.isEmpty() ? tr("(GPU info indisponivel)") : GPUDescription);

        Browser->setHtml(QStringLiteral(
            "<table style='font-size:10pt; border-spacing:0;'>%1</table>"
        ).arg(Rows));

        return Browser;
    }

    QWidget* AboutDialog::BuildCreditsTab() {
        auto* Browser = new QTextBrowser(this);
        Browser->setOpenExternalLinks(true);
        Browser->setHtml(QStringLiteral(R"(
    <p style="font-size:11pt; color:#DCDCDC;"><b>Autor</b></p>
    <p style="margin-left:12px;">Kevin Ramos</p>

    <p style="font-size:11pt; color:#DCDCDC; margin-top:14px;"><b>Inspiracao &amp; Referencias</b></p>
    <ul style="margin-left:18px; line-height:160%%;">
      <li><span style="color:%1;">CryEngine</span> &mdash; padrao de UI do editor, organizacao de componentes</li>
      <li><span style="color:%1;">Unreal Engine</span> &mdash; design de viewport e ferramentas</li>
      <li><span style="color:%1;">Flax Engine</span> &mdash; arquitetura de renderer modular</li>
    </ul>

    <p style="font-size:11pt; color:#DCDCDC; margin-top:14px;"><b>Comunidade</b></p>
    <p style="margin-left:12px; color:#9A9A9A;">
    A todos que documentam DirectX 12 e Qt em blogs, livros e talks &mdash; valeu.
    </p>
    )").arg(kAccent));
        return Browser;
    }

    QWidget* AboutDialog::BuildLicenseTab() {
        auto* Browser = new QTextBrowser(this);
        Browser->setOpenExternalLinks(true);
        Browser->setHtml(QStringLiteral(R"(
    <p style="font-size:11pt; color:#DCDCDC;"><b>SmileEngine</b></p>
    <p style="margin-left:12px; color:#9A9A9A;">
    Distribuida sob licenca proprietaria (placeholder &mdash; a definir).
    </p>

    <p style="font-size:11pt; color:#DCDCDC; margin-top:14px;"><b>Bibliotecas terceiras</b></p>
    <table style="margin-left:12px; font-size:10pt; border-spacing:0;">
      <tr>
        <td style="padding:4px 18px 4px 0; color:%1; font-weight:600;">Qt 6</td>
        <td style="padding:4px 0; color:#DCDCDC;">LGPL v3 &mdash;
            <a href="https://www.gnu.org/licenses/lgpl-3.0.html" style="color:%1;">licenca</a>
        </td>
      </tr>
      <tr>
        <td style="padding:4px 18px 4px 0; color:%1; font-weight:600;">DirectX 12</td>
        <td style="padding:4px 0; color:#DCDCDC;">Microsoft proprietary (parte do Windows SDK)</td>
      </tr>
      <tr>
        <td style="padding:4px 18px 4px 0; color:%1; font-weight:600;">DXC</td>
        <td style="padding:4px 0; color:#DCDCDC;">University of Illinois / NCSA Open Source &mdash;
            <a href="https://github.com/microsoft/DirectXShaderCompiler" style="color:%1;">repo</a>
        </td>
      </tr>
    </table>

    <p style="font-size:9pt; color:#9A9A9A; margin-top:20px;">
    Os simbolos e marcas exibidas nesta aplicacao pertencem a seus respectivos donos.
    </p>
    )").arg(kAccent));
        return Browser;
    }

    QString AboutDialog::ComposeInfoText() const {
        QString Text;
        Text += QStringLiteral("SmileEngine v%1 (build %2 - %3 UTC)\n")
                    .arg(QStringLiteral(SMILE_VERSION_STRING),
                         QStringLiteral(SMILE_BUILD_NUMBER),
                         QStringLiteral(SMILE_BUILD_DATE));
        Text += QStringLiteral("Compiler:     %1\n").arg(CompilerString());
        Text += QStringLiteral("Qt build:     %1\n").arg(QStringLiteral(QT_VERSION_STR));
        Text += QStringLiteral("Qt runtime:   %1\n").arg(QString::fromLatin1(qVersion()));
        Text += QStringLiteral("OS:           %1\n").arg(QSysInfo::prettyProductName());
        Text += QStringLiteral("Kernel:       %1 %2\n").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());
        Text += QStringLiteral("Architecture: %1\n").arg(QSysInfo::buildCpuArchitecture());
        Text += QStringLiteral("GPU adapter:  %1\n").arg(GPUDescription.isEmpty()
                                                          ? tr("(GPU info indisponivel)")
                                                          : GPUDescription);
        return Text;
    }

    void AboutDialog::OnCopyInfo() {
        QApplication::clipboard()->setText(ComposeInfoText());
    }
} 
