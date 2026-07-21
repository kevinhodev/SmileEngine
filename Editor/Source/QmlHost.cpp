#include "SmileEditor/QmlHost.h"
#include "Smile/Core/Logger.h"

#include <QQuickWidget>
#include <QQmlContext>
#include <QQmlEngine>
#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QUrl>

namespace SmileEditor {
    namespace {
        // Hot-reload: observa a pasta do .qml e recarrega o painel quando QUALQUER .qml muda
        // (pega tambem componentes compartilhados, ex.: LucideIcon). Ativo sempre que o QML vem
        // de um arquivo em disco — vale para Debug e Release. As context properties e os image
        // providers vivem no rootContext/engine e sobrevivem ao setSource, nao precisam re-setar.
        void InstallQmlHotReload(QQuickWidget* _Widget, const QString& _Path) {
            const QString Dir = QFileInfo(_Path).absolutePath();
            if (Dir.isEmpty() || !QFileInfo::exists(_Path)) return;

            auto* Watcher = new QFileSystemWatcher(_Widget);

            // Debounce: editores salvam em varios passos (save atomico). Reloda so ao estabilizar.
            auto* Debounce = new QTimer(_Widget);
            Debounce->setSingleShot(true);
            Debounce->setInterval(120);

            // (Re)observa as pastas E todos os .qml delas. Os dois sinais sao necessarios: edicao
            // in-place dispara fileChanged; save atomico (rename) dispara directoryChanged e ainda
            // derruba o watch do arquivo trocado — por isso re-armamos os paths a cada evento.
            // Inclui a subpasta components/ (Theme singleton + componentes compartilhados): salvar
            // um componente recarrega todos os paineis abertos que o usam.
            auto Rearm = [Watcher, Dir]() {
                const QStringList Dirs{ Dir, Dir + "/components" };
                for (const QString& D : Dirs) {
                    if (!QFileInfo::exists(D)) continue;
                    if (!Watcher->directories().contains(D)) Watcher->addPath(D);
                    const QStringList Files = QDir(D).entryList(QStringList{ "*.qml" }, QDir::Files);
                    for (const QString& F : Files) {
                        const QString Fp = D + "/" + F;
                        if (!Watcher->files().contains(Fp)) Watcher->addPath(Fp);
                    }
                }
            };
            Rearm();
            Smile::LogInfo("QML hot-reload armado: " + QFileInfo(_Path).fileName().toStdString() +
                           " (" + std::to_string(Watcher->files().size()) + " .qml em " +
                           Dir.toStdString() + ")");

            const QUrl Url = QUrl::fromLocalFile(_Path);
            const QString File = QFileInfo(_Path).fileName();
            QObject::connect(Debounce, &QTimer::timeout, _Widget, [_Widget, Url, File, Rearm]() {
                Smile::LogInfo("QML hot-reload: recarregando " + File.toStdString());
                Rearm();                                   // re-arma watches perdidos no save
                _Widget->setSource(QUrl());                // descarrega a arvore atual
                _Widget->engine()->clearComponentCache();  // forca recompilar do disco
                _Widget->setSource(Url);
                if (_Widget->status() == QQuickWidget::Error)
                    for (const QQmlError& Err : _Widget->errors())
                        Smile::LogError("QML hot-reload: " + Err.toString().toStdString());
            });

            auto Kick = [Debounce]() { Debounce->start(); };
            QObject::connect(Watcher, &QFileSystemWatcher::fileChanged,      Debounce, Kick);
            QObject::connect(Watcher, &QFileSystemWatcher::directoryChanged, Debounce, Kick);
        }
    }

    QString ResolveQmlPath(const QString& _QmlFileName) {
#ifdef SMILE_EDITOR_SOURCE_DIR
        const QString SourcePath =
            QStringLiteral(SMILE_EDITOR_SOURCE_DIR) + QStringLiteral("/Qml/") + _QmlFileName;
        if (QFileInfo::exists(SourcePath)) return SourcePath;
#endif
        const QString Deployed =
            QDir(QCoreApplication::applicationDirPath()).filePath("Editor/Qml/" + _QmlFileName);
        if (QFileInfo::exists(Deployed)) return Deployed;

#ifdef SMILE_EDITOR_SOURCE_DIR
        return SourcePath; // ultimo recurso: deixa o QQuickWidget logar o erro de carga
#else
        return Deployed;
#endif
    }

    QQuickWidget* CreateQmlPanel(const QString& _QmlFileName,
                                 const QVector<QPair<QString, QObject*>>& _ContextProps,
                                 QWidget* _Parent,
                                 const QVector<QPair<QString, QQmlImageProviderBase*>>& _ImageProviders) {
        auto* Widget = new QQuickWidget(_Parent);
        Widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        Widget->setClearColor(QColor(0x10, 0x11, 0x0f)); // mesmo fundo do tema (#10110f)

        for (const auto& Provider : _ImageProviders)
            Widget->engine()->addImageProvider(Provider.first, Provider.second); // engine assume posse

        for (const auto& Prop : _ContextProps)
            Widget->rootContext()->setContextProperty(Prop.first, Prop.second);

        const QString Path = ResolveQmlPath(_QmlFileName);
        Widget->setSource(QUrl::fromLocalFile(Path));

        if (Widget->status() == QQuickWidget::Error) {
            for (const QQmlError& Err : Widget->errors())
                Smile::LogError("QML [" + _QmlFileName.toStdString() + "]: " +
                                Err.toString().toStdString());
        }

        InstallQmlHotReload(Widget, Path); // recarrega ao salvar o .qml (Debug e Release)
        return Widget;
    }
}
