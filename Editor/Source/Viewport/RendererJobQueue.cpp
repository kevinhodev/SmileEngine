#include "SmileEditor/Viewport/RendererJobQueue.h"
#include "Smile/Core/Logger.h"
#include <QMetaObject>
#include <QPointer>
#include <exception>
#include <utility>

namespace SmileEditor {
    namespace {
        void InvokeCompletion(
                const RendererJobQueue::Completion& _Completion,
                bool _Success,
                const QString& _Error) {
            if (!_Completion) return;
            try {
                _Completion(_Success, _Error);
            } catch (const std::exception& Error) {
                Smile::LogError(
                    std::string("Falha no callback do job do renderer: ") + Error.what());
            } catch (...) {
                Smile::LogError("Falha desconhecida no callback do job do renderer");
            }
        }
    }

    RendererJobQueue::RendererJobQueue(RenderThread& _Thread)
        : Thread(_Thread) {}

    bool RendererJobQueue::Enqueue(
            const QString& _CoalesceKey,
            RenderThread::RendererJob _Job,
            Completion _OnCompleted) {
        if (!_Job || Stopping || !Thread.IsReady()) return false;

        if (!_CoalesceKey.isEmpty()) {
            for (auto It = Pending.rbegin(); It != Pending.rend(); ++It) {
                if (It->CoalesceKey == _CoalesceKey) {
                    It->Execute = std::move(_Job);
                    It->OnCompleted = std::move(_OnCompleted);
                    return true;
                }
            }
        }

        Pending.push_back(QueuedJob{
            _CoalesceKey, std::move(_Job), std::move(_OnCompleted) });
        if (!Busy) {
            Busy = true;
            QPointer<RendererJobQueue> Guard(this);
            emit BecameBusy();
            if (!Guard) return false;
        }
        DispatchNext();
        return true;
    }

    void RendererJobQueue::Stop() {
        if (Stopping) return;
        Stopping = true;
        Pending.clear();
        PublishIdleIfDrained();
    }

    void RendererJobQueue::DispatchNext() {
        if (Active || Stopping) return;

        while (!Pending.empty()) {
            QueuedJob Job = std::move(Pending.front());
            Pending.pop_front();
            Active = true;

            Completion FailureCompletion = Job.OnCompleted;
            QPointer<RendererJobQueue> Self(this);
            const bool Submitted = Thread.RequestRendererJob(
                std::move(Job.Execute),
                [Self, OnCompleted = std::move(Job.OnCompleted)](
                        RenderThread::JobCompletion _Result) mutable {
                    if (!Self) return;
                    QMetaObject::invokeMethod(
                        Self,
                        [Self, Result = std::move(_Result),
                         OnCompleted = std::move(OnCompleted)]() mutable {
                            if (Self)
                                Self->FinishCurrent(
                                    std::move(Result), std::move(OnCompleted));
                        },
                        Qt::QueuedConnection);
                });
            if (Submitted) return;

            Active = false;
            QPointer<RendererJobQueue> Guard(this);
            if (!Stopping)
                InvokeCompletion(FailureCompletion, false,
                                 QStringLiteral("render thread indisponivel"));
            if (!Guard) return;
            emit JobFinished();
            if (!Guard) return;
        }

        PublishIdleIfDrained();
    }

    void RendererJobQueue::FinishCurrent(
            RenderThread::JobCompletion _Result,
            Completion _OnCompleted) {
        QPointer<RendererJobQueue> Guard(this);
        if (!Stopping)
            InvokeCompletion(_OnCompleted, _Result.Success,
                             QString::fromStdString(_Result.Error));
        if (!Guard) return;

        Thread.CompleteJob();
        Active = false;
        emit JobFinished();
        if (!Guard) return;
        DispatchNext();
        if (!Guard) return;
        PublishIdleIfDrained();
    }

    void RendererJobQueue::PublishIdleIfDrained() {
        if (!Busy || Active || !Pending.empty()) return;
        Busy = false;
        emit BecameIdle();
    }
}
