#pragma once

#include "SmileEditor/Viewport/RenderThread.h"
#include <QObject>
#include <QString>
#include <deque>
#include <functional>

namespace SmileEditor {
    // Fila da thread Qt para operacoes exclusivas do renderer.
    class RendererJobQueue final : public QObject {
        Q_OBJECT

    public:
        using Completion = std::function<void(bool, const QString&)>;

        explicit RendererJobQueue(RenderThread& Thread);

        bool Enqueue(const QString& CoalesceKey,
                     RenderThread::RendererJob Job,
                     Completion OnCompleted);
        void Stop();
        bool IsBusy() const { return Busy; }

    signals:
        void BecameBusy();
        void JobFinished();
        void BecameIdle();

    private:
        struct QueuedJob {
            QString                   CoalesceKey;
            RenderThread::RendererJob Execute;
            Completion                OnCompleted;
        };

        void DispatchNext();
        void FinishCurrent(RenderThread::JobCompletion Result, Completion OnCompleted);
        void PublishIdleIfDrained();

        RenderThread&         Thread;
        std::deque<QueuedJob> Pending;
        bool                  Active   = false;
        bool                  Busy     = false;
        bool                  Stopping = false;
    };
}
