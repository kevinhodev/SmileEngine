#pragma once

#include <QString>
#include <QVector>
#include <QPair>

class QWidget;
class QObject;
class QQuickWidget;
class QQmlImageProviderBase;

namespace SmileEditor {
    // Resolve o caminho de um painel QML: em dev usa SMILE_EDITOR_SOURCE_DIR/Qml (permite
    // hot-reload editando o .qml direto); em deploy cai para <exe>/Editor/Qml. Retorna o
    // primeiro que existir, ou o caminho de source dir como ultimo recurso.
    QString ResolveQmlPath(const QString& qmlFileName);

    // Cria um QQuickWidget hospedando Qml/<qmlFileName>, com fundo opaco do tema e
    // SizeRootObjectToView. contextProps expoe objetos C++ ao QML (ex.: a LogBridge);
    // imageProviders registra fontes "image://<nome>/..." (ex.: smilelogo). Tudo e setado
    // ANTES do setSource, entao ja esta visivel na carga do QML. A engine assume a posse
    // dos providers.
    // Hot-reload: enquanto o .qml for carregado de arquivo em disco (Debug e Release), salvar
    // qualquer .qml da pasta recarrega o painel vivo, sem recompilar C++.
    QQuickWidget* CreateQmlPanel(const QString& qmlFileName,
                                 const QVector<QPair<QString, QObject*>>& contextProps,
                                 QWidget* parent,
                                 const QVector<QPair<QString, QQmlImageProviderBase*>>& imageProviders = {});
}
