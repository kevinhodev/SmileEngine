#include "SmileEditor/QmlHost.h"
#include "Smile/Core/Logger.h"

#include <QQuickWidget>
#include <QQuickImageProvider>
#include <QQmlEngine>
#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace SmileEditor {
    namespace {
        // Hot-reload: observa a pasta dos .qml e recarrega os paineis quando QUALQUER .qml muda
        // (pega tambem componentes compartilhados, ex.: LucideIcon). Ativo sempre que o QML vem
        // de arquivo em disco — vale para Debug e Release.
        struct FQmlPanelRegistration {
            QPointer<QQuickWidget> Widget;
            QUrl Url;
            QString File;
            QVariantMap InitialProperties;
        };

        // Uma engine compartilhada exige uma unica transacao de hot-reload. Primeiro descarregamos
        // TODAS as arvores registradas; so entao limpamos cache/singletons e carregamos tudo de novo.
        // Isso evita que um painel mantenha instancias de tipos QML que a mesma engine invalidou,
        // alem de trocar N watchers e N clears de cache por apenas um de cada.
        class FQmlHotReloadCoordinator final : public QObject {
        public:
            explicit FQmlHotReloadCoordinator(QQmlEngine& _Engine)
                : QObject(&_Engine), Engine(&_Engine), Watcher(this), Debounce(this) {
                setObjectName(QStringLiteral("SmileQmlHotReloadCoordinator"));
                Debounce.setSingleShot(true);
                Debounce.setInterval(120);

                QObject::connect(&Debounce, &QTimer::timeout, this, [this]() { Reload(); });
                QObject::connect(&Watcher, &QFileSystemWatcher::fileChanged,
                                 this, [this](const QString&) { Debounce.start(); });
                QObject::connect(&Watcher, &QFileSystemWatcher::directoryChanged,
                                 this, [this](const QString&) { Debounce.start(); });
            }

            void Register(QQuickWidget* _Widget,
                          const QString& _Path,
                          const QVariantMap& _InitialProperties) {
                const QString Dir = QFileInfo(_Path).absolutePath();
                if (!_Widget || Dir.isEmpty() || !QFileInfo::exists(_Path)) return;

                for (const FQmlPanelRegistration& Panel : Panels)
                    if (Panel.Widget == _Widget) return;

                Panels.push_back({ _Widget,
                                   QUrl::fromLocalFile(_Path),
                                   QFileInfo(_Path).fileName(),
                                   _InitialProperties });
                RootDirs.insert(QDir::cleanPath(Dir));
                Rearm();

                QObject::connect(_Widget, &QObject::destroyed, this, [this, _Widget]() {
                    for (qsizetype Index = Panels.size(); Index-- > 0;)
                        if (Panels[Index].Widget.isNull() || Panels[Index].Widget == _Widget)
                            Panels.removeAt(Index);
                });

                Smile::LogDebug("QML compartilhado: " +
                                std::to_string(Panels.size()) + " painel(is), " +
                                std::to_string(Watcher.files().size()) + " .qml observados");
            }

        private:
            void Rearm() {
                for (const QString& RootDir : RootDirs) {
                    const QStringList Dirs{ RootDir, QDir(RootDir).filePath("components") };
                    for (const QString& Dir : Dirs) {
                        if (!QFileInfo::exists(Dir)) continue;
                        if (!Watcher.directories().contains(Dir)) Watcher.addPath(Dir);

                        const QStringList Files =
                            QDir(Dir).entryList(QStringList{ "*.qml" }, QDir::Files);
                        for (const QString& File : Files) {
                            const QString Path = QDir(Dir).absoluteFilePath(File);
                            if (!Watcher.files().contains(Path)) Watcher.addPath(Path);
                        }
                    }
                }
            }

            void Reload() {
                for (qsizetype Index = Panels.size(); Index-- > 0;)
                    if (Panels[Index].Widget.isNull()) Panels.removeAt(Index);
                if (Panels.isEmpty()) return;

                Rearm(); // save atomico pode remover o watch do arquivo substituido
                Smile::LogInfo("QML hot-reload compartilhado: recarregando " +
                               std::to_string(Panels.size()) + " painel(is)");

                for (const FQmlPanelRegistration& Panel : Panels)
                    Panel.Widget->setSource(QUrl());

                Engine->clearComponentCache();
                Engine->clearSingletons(); // Theme.qml tambem pode ter sido alterado

                for (const FQmlPanelRegistration& Panel : Panels) {
                    // Nao dependemos de setInitialProperties sobreviver a troca de source.
                    if (!Panel.InitialProperties.isEmpty())
                        Panel.Widget->setInitialProperties(Panel.InitialProperties);
                    Panel.Widget->setSource(Panel.Url);
                    if (Panel.Widget->status() == QQuickWidget::Error)
                        for (const QQmlError& Err : Panel.Widget->errors())
                            Smile::LogError("QML hot-reload [" + Panel.File.toStdString() + "]: " +
                                            Err.toString().toStdString());
                }
            }

            QQmlEngine* Engine = nullptr;
            QFileSystemWatcher Watcher;
            QTimer Debounce;
            QVector<FQmlPanelRegistration> Panels;
            QSet<QString> RootDirs;
        };

        FQmlHotReloadCoordinator* HotReloadCoordinator(QQmlEngine& _Engine) {
            constexpr auto Name = "SmileQmlHotReloadCoordinator";
            if (QObject* Existing =
                    _Engine.findChild<QObject*>(QString::fromLatin1(Name),
                                                Qt::FindDirectChildrenOnly))
                return static_cast<FQmlHotReloadCoordinator*>(Existing);
            return new FQmlHotReloadCoordinator(_Engine);
        }

        void InstallQmlHotReload(QQmlEngine& _Engine,
                                 QQuickWidget* _Widget,
                                 const QString& _Path,
                                 const QVariantMap& _InitialProperties) {
            HotReloadCoordinator(_Engine)->Register(_Widget, _Path, _InitialProperties);
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

    QQuickWidget* CreateQmlPanel(QQmlEngine& _Engine,
                                 const QString& _QmlFileName,
                                 const QVector<QPair<QString, QObject*>>& _ObjectProperties,
                                 QWidget* _Parent,
                                 const QVector<QPair<QString, QQmlImageProviderBase*>>& _ImageProviders,
                                 const QVariantMap& _InitialProperties) {
        auto* Widget = new QQuickWidget(&_Engine, _Parent);
        Widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        Widget->setClearColor(QColor(0x10, 0x11, 0x0f)); // mesmo fundo do tema (#10110f)

        for (const auto& Provider : _ImageProviders) {
            if (_Engine.imageProvider(Provider.first)) {
                Smile::LogError("QML image provider duplicado na engine compartilhada: " +
                                Provider.first.toStdString());
                delete Provider.second; // a engine ainda nao assumiu a posse deste duplicado
                continue;
            }
            _Engine.addImageProvider(Provider.first, Provider.second); // engine assume posse
        }

        // Todo objeto C++ e uma propriedade EXPLICITA do componente raiz. Alem de dar contrato
        // verificavel ao QML (required property), isto deixa os paines prontos para compartilhar
        // uma QQmlEngine: rootContext() pertence a engine e vazaria estado entre widgets.
        QVariantMap Properties = _InitialProperties;
        for (const auto& Prop : _ObjectProperties)
            Properties.insert(Prop.first, QVariant::fromValue(Prop.second));
        if (!Properties.isEmpty()) Widget->setInitialProperties(Properties);

        const QString Path = ResolveQmlPath(_QmlFileName);
        Widget->setSource(QUrl::fromLocalFile(Path));

        if (Widget->status() == QQuickWidget::Error) {
            for (const QQmlError& Err : Widget->errors())
                Smile::LogError("QML [" + _QmlFileName.toStdString() + "]: " +
                                Err.toString().toStdString());
        }

        InstallQmlHotReload(_Engine, Widget, Path, Properties);
        return Widget;
    }
}
