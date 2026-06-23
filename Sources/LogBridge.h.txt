#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <string_view>
#include "Smile/Core/Logger.h"

namespace SmileEditor {
    // Model do console (C++ -> QML). Recebe as linhas de log da Engine (possivelmente de outra
    // thread), normaliza para time/tag/cor e as guarda num list model que o ConsolePanel.qml
    // consome via `model:`. Escolha por model (e nao signal): nada se perde se algo for logado
    // antes da UI montar, e some a fragilidade de nome de signal no QML. Teto em maxLines.
    class LogBridge : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(int maxLines READ MaxLines CONSTANT)

    public:
        enum Roles {
            TimeRole = Qt::UserRole + 1,
            TagRole,
            MessageRole,
            LevelColorRole,
        };

        explicit LogBridge(QObject* parent = nullptr);

        // Chamada pelo sink de log da Engine; thread-safe (marshala para a thread da GUI).
        void Append(Smile::LogLevel level, std::string_view message);

        int MaxLines() const { return 500; }

        // QAbstractListModel
        int rowCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        // Acoes do menu (⋮) do console, chamadas pelo QML.
        Q_INVOKABLE void CopyAll() const;  // copia todo o log (texto plano) pro clipboard
        Q_INVOKABLE void RequestClose();   // pede o fechamento do dock (emite CloseRequested)

    public slots:
        // Botao "limpar" do painel QML.
        void Clear();

    signals:
        // MainWindow liga isto ao close() do QDockWidget (a barra de titulo virou 100% QML).
        void CloseRequested();

    private:
        struct Entry {
            QString Time;
            QString Tag;
            QString Message;
            QString Color; // hex CSS (#rrggbb) por nivel
        };

        // Insere uma linha; roda sempre na thread da GUI (chamada via QueuedConnection).
        Q_INVOKABLE void AppendRow(const QString& time, const QString& tag,
                                   const QString& message, const QString& color);

        QVector<Entry> Entries;
    };
}
