#include "SmileEditor/UI/LogBridge.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QThread>
#include <QTime>
#include <algorithm>
#include <utility>

namespace SmileEditor {
    LogBridge::LogBridge(QObject* _Parent)
        : QAbstractListModel(_Parent)
    {
    }

    int LogBridge::rowCount(const QModelIndex& _Parent) const {
        if (_Parent.isValid()) return 0; // lista simples, sem hierarquia
        return static_cast<int>(Entries.size());
    }

    QVariant LogBridge::data(const QModelIndex& _Index, int _Role) const {
        if (!_Index.isValid() || _Index.row() < 0 || _Index.row() >= Entries.size())
            return {};
        const Entry& E = Entries.at(_Index.row());
        switch (_Role) {
            case TimeRole:       return E.Time;
            case TagRole:        return E.Tag;
            case MessageRole:    return E.Message;
            case LevelColorRole: return E.Color;
            default:             return {};
        }
    }

    QHash<int, QByteArray> LogBridge::roleNames() const {
        return {
            { TimeRole,       "time" },
            { TagRole,        "tag" },
            { MessageRole,    "message" },
            { LevelColorRole, "levelColor" },
        };
    }

    void LogBridge::Append(Smile::LogLevel _Level, std::string_view _Message) {
        // Diagnostico detalhado permanece no log persistente da sessao, mas nao compete com
        // eventos acionaveis no console do editor. O arquivo continua sendo a fonte completa.
        if (_Level == Smile::LogLevel::Debug) return;

        // Mesma paleta do antigo console HTML: azul/ambar/vermelho por nivel.
        const char* Color = _Level == Smile::LogLevel::Error   ? "#ff5f57"
                          : _Level == Smile::LogLevel::Warning ? "#f3b43f"
                                                               : "#a7b5ff";
        const char* Tag   = _Level == Smile::LogLevel::Error   ? "ERR"
                          : _Level == Smile::LogLevel::Warning ? "WARN"
                                                               : "INFO";

        const size_t BytesToCopy = std::min(_Message.size(), kMaxConsoleMessageBytes);
        QString Message = QString::fromUtf8(
            _Message.data(), static_cast<qsizetype>(BytesToCopy));
        if (BytesToCopy < _Message.size()) {
            Message += QStringLiteral(
                "\n… [mensagem truncada no console; consulte o log persistente da sessão]");
        }

        Entry PendingEntry{
            QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
            QString::fromLatin1(Tag),
            std::move(Message),
            QString::fromLatin1(Color)
        };

        bool MustSchedule = false;
        {
            std::lock_guard Lock(PendingMutex);
            if (Pending.size() >= kMaxPendingEntries) {
                Pending.pop_front();
                ++PendingDropped;
            }
            Pending.push_back(std::move(PendingEntry));
            if (!DrainScheduled) {
                DrainScheduled = true;
                MustSchedule = true;
            }
        }

        if (MustSchedule) {
            const bool Scheduled = QMetaObject::invokeMethod(
                this, [this] { DrainPending(); }, Qt::QueuedConnection);
            if (!Scheduled) {
                std::lock_guard Lock(PendingMutex);
                DrainScheduled = false;
            }
        }
    }

    void LogBridge::DrainPending() {
        Q_ASSERT(QThread::currentThread() == thread());

        std::deque<Entry> Batch;
        qulonglong Dropped = 0;
        {
            std::lock_guard Lock(PendingMutex);
            Batch.swap(Pending);
            Dropped = std::exchange(PendingDropped, 0);
            DrainScheduled = false;
        }

        bool StatsDirty = false;
        if (Dropped > 0) {
            DroppedCount_ += Dropped;
            StatsDirty = true;
        }
        if (Batch.empty()) {
            if (StatsDirty) emit StatsChanged();
            return;
        }

        const qsizetype Existing = Entries.size();
        const qsizetype Incoming = static_cast<qsizetype>(Batch.size());
        const qsizetype Overflow = std::max<qsizetype>(
            0, Existing + Incoming - static_cast<qsizetype>(MaxLines()));
        const qsizetype RemoveExisting = std::min(Overflow, Existing);
        const qsizetype SkipIncoming = Overflow - RemoveExisting;

        if (RemoveExisting > 0) {
            beginRemoveRows(QModelIndex(), 0, static_cast<int>(RemoveExisting - 1));
            Entries.erase(Entries.begin(), Entries.begin() + RemoveExisting);
            endRemoveRows();
        }
        for (qsizetype I = 0; I < SkipIncoming; ++I) Batch.pop_front();

        if (Overflow > 0) {
            EvictedCount_ += static_cast<qulonglong>(Overflow);
            StatsDirty = true;
        }

        if (!Batch.empty()) {
            const int First = static_cast<int>(Entries.size());
            const int Last  = First + static_cast<int>(Batch.size()) - 1;
            beginInsertRows(QModelIndex(), First, Last);
            while (!Batch.empty()) {
                Entries.push_back(std::move(Batch.front()));
                Batch.pop_front();
            }
            endInsertRows();
            StatsDirty = true;
            emit RowsAppended();
        }

        if (StatsDirty) emit StatsChanged();
    }

    void LogBridge::Clear() {
        Q_ASSERT(QThread::currentThread() == thread());

        {
            std::lock_guard Lock(PendingMutex);
            Pending.clear();
            PendingDropped = 0;
            // Se um drain ja esta postado, ele permanece responsavel por liberar a flag.
        }

        if (!Entries.isEmpty()) {
            beginResetModel();
            Entries.clear();
            endResetModel();
        }
        DroppedCount_ = 0;
        EvictedCount_ = 0;
        emit StatsChanged();
    }

    void LogBridge::CopyAll() const {
        Q_ASSERT(QThread::currentThread() == thread());
        QString Text;
        Text.reserve(Entries.size() * 96);
        for (const Entry& E : Entries)
            Text += E.Time + "  " + E.Tag + "  " + E.Message + '\n';
        QGuiApplication::clipboard()->setText(Text);
    }

    void LogBridge::RequestClose() {
        emit CloseRequested();
    }
}
