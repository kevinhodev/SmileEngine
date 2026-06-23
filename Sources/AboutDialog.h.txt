#pragma once

// Dialogo "About" do SmileEditor.
// Modal, nao-redimensionavel, com tabs (About / System / Credits / License).
// Inspirado no padrao do CryEngine (CAboutDialog) com modernizacoes visuais.

#include <QDialog>
#include <QString>

namespace SmileEditor {

class AboutDialog : public QDialog {
    Q_OBJECT

public:
    // gpuDescription eh exibido na aba "System Info". Pode ser vazio se o renderer
    // ainda nao foi inicializado quando o dialogo eh criado.
    explicit AboutDialog(QString gpuDescription, QWidget* parent = nullptr);

private slots:
    // Copia um bloco de texto multilinhas (versao + sistema + GPU) para o clipboard
    void OnCopyInfo();

private:
    QWidget* BuildHeader();
    QWidget* BuildAboutTab();
    QWidget* BuildSystemTab();
    QWidget* BuildCreditsTab();
    QWidget* BuildLicenseTab();

    // Texto que vai para o clipboard - tambem usado para popular a aba System Info
    QString ComposeInfoText() const;

    QString GPUDescription;
};

} // namespace SmileEditor
