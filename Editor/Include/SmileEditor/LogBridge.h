#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <deque>
#include <mutex>
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
        Q_PROPERTY(int count READ Count NOTIFY StatsChanged)
        Q_PROPERTY(qulonglong droppedCount READ DroppedCount NOTIFY StatsChanged)
        Q_PROPERTY(qulonglong evictedCount READ EvictedCount NOTIFY StatsChanged)

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
        int Count() const { return static_cast<int>(Entries.size()); }
        qulonglong DroppedCount() const { return DroppedCount_; }
        qulonglong EvictedCount() const { return EvictedCount_; }

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
        void StatsChanged();
        // Sinal explicito: no teto FIFO, count/contentHeight podem ficar iguais depois do lote.
        void RowsAppended();

    private:
        struct Entry {
            QString Time;
            QString Tag;
            QString Message;
            QString Color; // hex CSS (#rrggbb) por nivel
        };

        // Drena a fila concorrente em um unico lote; roda sempre na thread da GUI.
        void DrainPending();

        QVector<Entry> Entries;

        static constexpr size_t kMaxPendingEntries = 4096;
        static constexpr size_t kMaxConsoleMessageBytes = 256 * 1024;

        std::mutex        PendingMutex;
        std::deque<Entry> Pending;
        qulonglong        PendingDropped = 0;
        bool              DrainScheduled = false;

        qulonglong DroppedCount_ = 0; // pressao na fila: nunca chegou ao model
        qulonglong EvictedCount_ = 0; // FIFO normal: saiu pelo teto de historico
    };
}
