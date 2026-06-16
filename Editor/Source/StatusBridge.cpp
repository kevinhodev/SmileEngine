#include "SmileEditor/StatusBridge.h"
#include <QTimer>

namespace SmileEditor {
    StatusBridge::StatusBridge(QObject* _Parent)
        : QObject(_Parent), Message_(QStringLiteral("Pronto"))
    {
        MsgTimer = new QTimer(this);
        MsgTimer->setSingleShot(true);
        connect(MsgTimer, &QTimer::timeout, this, [this]() {
            Message_ = QStringLiteral("Pronto");
            emit MessageChanged();
        });
    }

    void StatusBridge::SetStats(double _Fps, double _FrameMs, const QString& _Scene, const QString& _Ocean) {
        Fps_ = _Fps; FrameMs_ = _FrameMs; Scene_ = _Scene; Ocean_ = _Ocean;
        emit StatsChanged();
    }

    void StatusBridge::ShowMessage(const QString& _Text, int _TimeoutMs) {
        Message_ = _Text;
        emit MessageChanged();
        if (_TimeoutMs > 0) MsgTimer->start(_TimeoutMs);
        else                MsgTimer->stop();
    }
}
