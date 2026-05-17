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

// Cores da paleta - mantidas em sincronia com DarkTheme.cpp
constexpr const char* kAccent     = "#0E639C";
constexpr const char* kAccentDark = "#094771";
constexpr const char* kBgWindow   = "#1E1E1E";
constexpr const char* kBgPanel    = "#252526";
constexpr const char* kBgPanelAlt = "#2D2D30";
constexpr const char* kBorder     = "#3C3C3C";
constexpr const char* kText       = "#DCDCDC";
constexpr const char* kTextMuted  = "#9A9A9A";

// Formata o numero do compilador MSVC em algo legivel ("MSVC 19.42")
QString CompilerString() {
#if defined(_MSC_VER)
    const int major = _MSC_VER / 100;
    const int minor = _MSC_VER % 100;
    return QStringLiteral("MSVC %1.%2").arg(major).arg(minor);
#elif defined(__clang__)
    return QStringLiteral("Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__)
    return QStringLiteral("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#else
    return QStringLiteral("compilador desconhecido");
#endif
}

// Formata bytes em GB com 1 casa decimal (para a memoria de video do adapter)
QString FormatGigabytes(quint64 bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return QString::number(gb, 'f', 1) + QStringLiteral(" GB");
}

} // anon

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

    // Layout raiz: faixa accent (4px) + coluna principal
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* accentStrip = new QFrame(this);
    accentStrip->setFixedWidth(4);
    accentStrip->setStyleSheet(QStringLiteral("background: %1;").arg(kAccent));
    rootLayout->addWidget(accentStrip);

    auto* contentLayout = new QVBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    rootLayout->addLayout(contentLayout, 1);

    // Header - logo + titulo + versao
    contentLayout->addWidget(BuildHeader());

    // Tabs
    auto* tabs = new QTabWidget(this);
    tabs->addTab(BuildAboutTab(),    tr("About"));
    tabs->addTab(BuildSystemTab(),   tr("System Info"));
    tabs->addTab(BuildCreditsTab(),  tr("Credits"));
    tabs->addTab(BuildLicenseTab(),  tr("License"));
    contentLayout->addWidget(tabs, 1);

    // Footer - copy info (esquerda) + close (direita)
    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(16, 12, 16, 14);
    footer->setSpacing(8);

    auto* copyButton = new QPushButton(tr("Copy info"), this);
    copyButton->setToolTip(tr("Copia versao e informacoes de sistema para o clipboard"));
    connect(copyButton, &QPushButton::clicked, this, &AboutDialog::OnCopyInfo);
    footer->addWidget(copyButton);

    footer->addStretch(1);

    auto* closeButton = new QPushButton(tr("Close"), this);
    closeButton->setDefault(true);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(closeButton);

    contentLayout->addLayout(footer);

    // Fade-in suave - 180ms
    setWindowOpacity(0.0);
    QTimer::singleShot(0, this, [this]{
        auto* anim = new QPropertyAnimation(this, "windowOpacity", this);
        anim->setDuration(180);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutQuad);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

QWidget* AboutDialog::BuildHeader() {
    auto* header = new QWidget(this);
    header->setFixedHeight(96);
    header->setStyleSheet(QStringLiteral("background: %1;").arg(kBgPanelAlt));

    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(20, 14, 20, 14);
    layout->setSpacing(18);

    // "Logo" textual estilizado: letra "S" grande dentro de um quadrado accent
    auto* logo = new QLabel(QStringLiteral("S"), header);
    logo->setFixedSize(64, 64);
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet(QString(
        "background: %1; color: white; font-size: 40px; font-weight: bold;"
        "border-radius: 6px;").arg(kAccent));
    layout->addWidget(logo);

    // Bloco de titulo + versao
    auto* textBox = new QVBoxLayout();
    textBox->setContentsMargins(0, 0, 0, 0);
    textBox->setSpacing(2);

    auto* title = new QLabel(QStringLiteral("SmileEngine"), header);
    title->setStyleSheet(QString("color: %1; font-size: 24px; font-weight: 600;").arg(kText));
    textBox->addWidget(title);

    auto* subtitle = new QLabel(tr("Game Engine"), header);
    subtitle->setStyleSheet(QString("color: %1; font-size: 11px; letter-spacing: 1px;").arg(kTextMuted));
    textBox->addWidget(subtitle);

    auto* version = new QLabel(
        QStringLiteral("v%1 · build %2")
            .arg(QStringLiteral(SMILE_VERSION_STRING))
            .arg(QStringLiteral(SMILE_BUILD_NUMBER)),
        header);
    version->setStyleSheet(QString(
        "color: %1; font-family: 'Consolas', 'Cascadia Mono', monospace; font-size: 10px;"
    ).arg(kTextMuted));
    textBox->addWidget(version);

    layout->addLayout(textBox);
    layout->addStretch(1);

    return header;
}

QWidget* AboutDialog::BuildAboutTab() {
    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(true);
    browser->setHtml(QStringLiteral(R"(
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
    return browser;
}

QWidget* AboutDialog::BuildSystemTab() {
    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(false);

    // Tabela HTML com chave/valor - cor de chave em accent para hierarquia visual
    QString rows;
    auto addRow = [&](const QString& key, const QString& val) {
        rows += QStringLiteral(
            "<tr>"
            "<td style='padding:4px 18px 4px 0; color:%1; font-weight:600;'>%2</td>"
            "<td style='padding:4px 0; color:%3; font-family:Consolas,monospace;'>%4</td>"
            "</tr>"
        ).arg(kAccent, key.toHtmlEscaped(), kText, val.toHtmlEscaped());
    };

    addRow(tr("Version"),       QStringLiteral(SMILE_VERSION_STRING));
    addRow(tr("Build number"),  QStringLiteral(SMILE_BUILD_NUMBER));
    addRow(tr("Build date"),    QStringLiteral(SMILE_BUILD_DATE) + QStringLiteral(" UTC"));
    addRow(tr("Compiler"),      CompilerString());
    addRow(tr("Qt (build)"),    QStringLiteral(QT_VERSION_STR));
    addRow(tr("Qt (runtime)"),  QString::fromLatin1(qVersion()));
    addRow(tr("OS"),            QSysInfo::prettyProductName());
    addRow(tr("Kernel"),        QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion());
    addRow(tr("Architecture"),  QSysInfo::buildCpuArchitecture());
    addRow(tr("GPU adapter"),   GPUDescription.isEmpty() ? tr("(GPU info indisponivel)") : GPUDescription);

    browser->setHtml(QStringLiteral(
        "<table style='font-size:10pt; border-spacing:0;'>%1</table>"
    ).arg(rows));

    return browser;
}

QWidget* AboutDialog::BuildCreditsTab() {
    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(true);
    browser->setHtml(QStringLiteral(R"(
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
    return browser;
}

QWidget* AboutDialog::BuildLicenseTab() {
    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(true);
    browser->setHtml(QStringLiteral(R"(
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
    return browser;
}

QString AboutDialog::ComposeInfoText() const {
    QString text;
    text += QStringLiteral("SmileEngine v%1 (build %2 - %3 UTC)\n")
                .arg(QStringLiteral(SMILE_VERSION_STRING),
                     QStringLiteral(SMILE_BUILD_NUMBER),
                     QStringLiteral(SMILE_BUILD_DATE));
    text += QStringLiteral("Compiler:     %1\n").arg(CompilerString());
    text += QStringLiteral("Qt build:     %1\n").arg(QStringLiteral(QT_VERSION_STR));
    text += QStringLiteral("Qt runtime:   %1\n").arg(QString::fromLatin1(qVersion()));
    text += QStringLiteral("OS:           %1\n").arg(QSysInfo::prettyProductName());
    text += QStringLiteral("Kernel:       %1 %2\n").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());
    text += QStringLiteral("Architecture: %1\n").arg(QSysInfo::buildCpuArchitecture());
    text += QStringLiteral("GPU adapter:  %1\n").arg(GPUDescription.isEmpty()
                                                      ? tr("(GPU info indisponivel)")
                                                      : GPUDescription);
    return text;
}

void AboutDialog::OnCopyInfo() {
    QApplication::clipboard()->setText(ComposeInfoText());
}

} // namespace SmileEditor
