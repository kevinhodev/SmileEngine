#pragma once

#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QPair>

class QWidget;
class QObject;
class QQuickWidget;
class QQmlEngine;
class QQmlImageProviderBase;

namespace SmileEditor {
    // Resolve o caminho de um painel QML: em dev usa SMILE_EDITOR_SOURCE_DIR/Qml (permite
    // hot-reload editando o .qml direto); em deploy cai para <exe>/Editor/Qml. Retorna o
    // primeiro que existir, ou o caminho de source dir como ultimo recurso.
    QString ResolveQmlPath(const QString& qmlFileName);

    // Cria um QQuickWidget hospedando Qml/<qmlFileName>, com fundo opaco do tema e
    // SizeRootObjectToView. objectProperties satisfaz propriedades QObject* declaradas no
    // componente raiz (inclusive required); initialProperties faz o mesmo para valores gerais;
    // imageProviders registra fontes "image://<nome>/..." (ex.: smilelogo). Tudo e setado
    // ANTES do setSource, entao ja esta visivel na carga do QML. A engine assume a posse
    // dos providers.
    // Hot-reload: enquanto os .qml forem carregados de arquivo em disco (Debug e Release), salvar
    // qualquer .qml da pasta recarrega em conjunto todos os paineis vivos, sem recompilar C++.
    // A engine e externa e deve sobreviver ao widget. Todos os paines do editor recebem a
    // mesma instancia criada no main, eliminando heaps/caches QML duplicados.
    QQuickWidget* CreateQmlPanel(QQmlEngine& engine,
                                 const QString& qmlFileName,
                                 const QVector<QPair<QString, QObject*>>& objectProperties,
                                 QWidget* parent,
                                 const QVector<QPair<QString, QQmlImageProviderBase*>>& imageProviders = {},
                                 const QVariantMap& initialProperties = {});
}
