#include "SmileEditor/LogBridge.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QTime>

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
        // Mesma paleta do antigo console HTML: azul/ambar/vermelho por nivel.
        const char* Color = _Level == Smile::LogLevel::Error   ? "#ff5f57"
                          : _Level == Smile::LogLevel::Warning ? "#f3b43f"
                                                               : "#a7b5ff";
        const char* Tag   = _Level == Smile::LogLevel::Error   ? "ERR"
                          : _Level == Smile::LogLevel::Warning ? "WARN"
                                                               : "INFO";

        const QString Time    = QTime::currentTime().toString("HH:mm:ss");
        const QString Message = QString::fromUtf8(_Message.data(),
                                                  static_cast<qsizetype>(_Message.size()));

        // O sink pode vir de qualquer thread (ex.: debug layer do D3D12); QueuedConnection
        // garante que beginInsertRows/endInsertRows rodem na thread da GUI (dona do model).
        QMetaObject::invokeMethod(this, "AppendRow", Qt::QueuedConnection,
                                  Q_ARG(QString, Time), Q_ARG(QString, QString(Tag)),
                                  Q_ARG(QString, Message), Q_ARG(QString, QString(Color)));
    }

    void LogBridge::AppendRow(const QString& _Time, const QString& _Tag,
                              const QString& _Message, const QString& _Color) {
        const int Row = static_cast<int>(Entries.size());
        beginInsertRows(QModelIndex(), Row, Row);
        Entries.push_back({ _Time, _Tag, _Message, _Color });
        endInsertRows();

        // Teto de linhas: descarta as mais antigas (FIFO).
        if (Entries.size() > MaxLines()) {
            beginRemoveRows(QModelIndex(), 0, 0);
            Entries.removeFirst();
            endRemoveRows();
        }
    }

    void LogBridge::Clear() {
        if (Entries.isEmpty()) return;
        beginResetModel();
        Entries.clear();
        endResetModel();
    }

    void LogBridge::CopyAll() const {
        QString Text;
        for (const Entry& E : Entries)
            Text += E.Time + "  " + E.Tag + "  " + E.Message + '\n';
        QGuiApplication::clipboard()->setText(Text);
    }

    void LogBridge::RequestClose() {
        emit CloseRequested();
    }
}
