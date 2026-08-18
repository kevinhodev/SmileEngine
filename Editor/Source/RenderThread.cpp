#include "SmileEditor/RenderThread.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Core/Logger.h"
#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

namespace SmileEditor {
    namespace {
        constexpr unsigned int kMaxConsecutiveFrameFailures = 3;
    }

    struct RendererHandle::Access::SharedState {
        std::recursive_mutex             Mutex;
        std::unique_ptr<Smile::Renderer> Instance = std::make_unique<Smile::Renderer>();
        std::atomic_bool                 Available{false};
    };

    RendererHandle::Access::Access(std::shared_ptr<SharedState> _State, bool _TryOnly)
        : StateRef(std::move(_State)),
          Lock(StateRef->Mutex, std::defer_lock)
    {
        if (_TryOnly) {
            if (!Lock.try_lock()) return;
        } else {
            Lock.lock();
        }
        Instance = StateRef->Instance.get();
    }

    RendererHandle::Access RendererHandle::Lock() const {
        if (!StateRef) return {};
        return Access(StateRef, false);
    }

    RendererHandle::Access RendererHandle::TryLock() const {
        if (!StateRef) return {};
        return Access(StateRef, true);
    }

    RendererHandle::operator bool() const {
        return StateRef && StateRef->Available.load(std::memory_order_acquire);
    }

    struct RenderThread::Impl {
        struct QueuedRendererJob {
            RendererJob Execute;
            std::function<void(JobCompletion)> Completion;
        };

        explicit Impl(std::shared_ptr<RendererHandle::Access::SharedState> _State)
            : State(std::move(_State)) {}

        // Unico acesso ao Renderer sem o mutex grande. Fica privado ao Impl para nenhuma
        // bridge conseguir apresentar por fora da ordem RenderFrame -> Present -> callback.
        void PresentSubmittedFrame() {
            State->Instance->PresentFrame();
        }

        std::shared_ptr<RendererHandle::Access::SharedState> State;
        std::thread              Worker;
        HWND                     NativeWindow = nullptr;
        std::mutex               WorkMutex;
        std::condition_variable  WorkReady;
        Callbacks                Hooks;
        bool                     StopRequested = false;
        bool                     FrameRequested = false;
        std::optional<QueuedRendererJob> PendingRendererJob;
        std::atomic_bool         Started{false};
        std::atomic_bool         Ready{false};
        std::atomic_bool         Exited{false};
        std::atomic_bool         FrameOutstanding{false};
        std::atomic_bool         JobOutstanding{false};
    };

    RenderThread::RenderThread() {
        auto State = std::make_shared<RendererHandle::Access::SharedState>();
        Handle = RendererHandle(State);
        Implementation = std::make_unique<Impl>(std::move(State));
    }

    RenderThread::~RenderThread() {
        RequestStop();
        Join();
    }

    void RenderThread::Start(void* _NativeWindow, Smile::u32 _Width, Smile::u32 _Height,
                             Callbacks _ThreadCallbacks) {
        Impl& I = *Implementation;
        bool Expected = false;
        if (!I.Started.compare_exchange_strong(Expected, true)) return;

        I.NativeWindow = static_cast<HWND>(_NativeWindow);
        I.Hooks = std::move(_ThreadCallbacks);
        I.Worker = std::thread([this, _NativeWindow, _Width, _Height]() {
            Impl& Thread = *Implementation;
            SetThreadDescription(GetCurrentThread(), L"Smile Render Thread");
            const HRESULT ComResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool ComInitialized = SUCCEEDED(ComResult);
            if (!ComInitialized && ComResult != RPC_E_CHANGED_MODE)
                Smile::LogWarning("COM nao inicializou na render thread");

            bool InitializationSucceeded = false;
            try {
                {
                    auto Renderer = Handle.Lock();
                    // Instalado so para o Initialize: o callback e limpo logo depois para nenhum
                    // caminho posterior segurar referencia ao Impl.
                    if (Thread.Hooks.Progress) {
                        Renderer->SetInitProgressCallback(
                            [&Thread](std::string_view Label, std::string_view Detail,
                                      Smile::f32 Fraction) {
                                Thread.Hooks.Progress(std::string(Label), std::string(Detail),
                                                      Fraction);
                            });
                    }
                    Renderer->Initialize(static_cast<HWND>(_NativeWindow), _Width, _Height);
                    Renderer->SetInitProgressCallback({});
                    // Reaplica a preferencia que existia antes do contexto e cria os targets
                    // internos uma unica vez ja na thread proprietaria.
                    Renderer->Settings().SetUpscaler(Renderer->Settings().GetUpscaler());
                }
                InitializationSucceeded = true;
            } catch (const std::exception& Error) {
                Smile::LogError(std::string("Falha ao inicializar a render thread: ") + Error.what());
                if (Thread.Hooks.InitializationFailed)
                    Thread.Hooks.InitializationFailed(Error.what());
            } catch (...) {
                Smile::LogError("Falha desconhecida ao inicializar a render thread");
                if (Thread.Hooks.InitializationFailed)
                    Thread.Hooks.InitializationFailed("falha desconhecida");
            }

            if (InitializationSucceeded) {
                bool PublishInitialization = false;
                {
                    std::lock_guard WorkLock(Thread.WorkMutex);
                    if (!Thread.StopRequested) {
                        // Publica Ready sob o mesmo mutex de RequestStop para a parada nunca
                        // ser sobrescrita por uma inicializacao que terminou ao mesmo tempo.
                        Thread.State->Available.store(true, std::memory_order_release);
                        Thread.Ready.store(true, std::memory_order_release);
                        PublishInitialization = true;
                    }
                }
                if (PublishInitialization && Thread.Hooks.Initialized)
                    Thread.Hooks.Initialized();
            }

            unsigned int ConsecutiveFailures = 0;
            while (Thread.Ready.load(std::memory_order_acquire)) {
                std::optional<Impl::QueuedRendererJob> RendererJob;
                bool ExecuteFrame = false;
                {
                    std::unique_lock WorkLock(Thread.WorkMutex);
                    Thread.WorkReady.wait(WorkLock, [&Thread]() {
                        return Thread.StopRequested || Thread.FrameRequested ||
                               (Thread.PendingRendererJob.has_value() &&
                                 !Thread.FrameOutstanding.load(std::memory_order_acquire));
                    });
                    if (Thread.StopRequested) break;
                    if (Thread.PendingRendererJob &&
                        !Thread.FrameOutstanding.load(std::memory_order_acquire)) {
                        RendererJob = std::move(Thread.PendingRendererJob);
                        Thread.PendingRendererJob.reset();
                    } else if (Thread.FrameRequested) {
                        Thread.FrameRequested = false;
                        ExecuteFrame = true;
                    }
                }

                if (RendererJob) {
                    JobCompletion Completion;
                    try {
                        auto Renderer = Handle.Lock();
                        Completion = RendererJob->Execute(*Renderer);
                    } catch (const std::exception& Error) {
                        Completion.Error = Error.what();
                    } catch (...) {
                        Completion.Error = "falha desconhecida no job do renderer";
                    }
                    if (!Completion.Success)
                        Smile::LogError("Falha no job do renderer: " + Completion.Error);
                    if (RendererJob->Completion)
                        RendererJob->Completion(std::move(Completion));
                    continue;
                }

                if (!ExecuteFrame) continue;
                FrameCompletion Completion;
                try {
                    {
                        auto Renderer = Handle.Lock();
                        Renderer->RenderFrame();
                    }
                    // Load-bearing: FrameOutstanding permanece true durante todo o Present.
                    // ApplyPendingResize so chama ResizeBuffers depois do callback completar,
                    // portanto Present e ResizeBuffers nunca concorrem na mesma swapchain.
                    Thread.PresentSubmittedFrame();
                    Completion.Success = true;
                    ConsecutiveFailures = 0;
                } catch (const std::exception& Error) {
                    Completion.Error = Error.what();
                } catch (...) {
                    Completion.Error = "falha desconhecida";
                }

                if (!Completion.Success) {
                    ++ConsecutiveFailures;
                    Completion.Terminal =
                        ConsecutiveFailures >= kMaxConsecutiveFrameFailures;
                    Smile::LogError(
                        std::string("Falha na render thread (") +
                        std::to_string(ConsecutiveFailures) + "/" +
                        std::to_string(kMaxConsecutiveFrameFailures) + "): " +
                        Completion.Error);
                    if (Completion.Terminal)
                        Thread.Ready.store(false, std::memory_order_release);
                }

                // Uma unica conclusao possui o flag de frame ate a GUI terminar de consumir o
                // resultado. Nunca mover este callback para antes de PresentSubmittedFrame().
                if (Thread.Hooks.FrameCompleted)
                    Thread.Hooks.FrameCompleted(std::move(Completion));
                if (!Thread.Ready.load(std::memory_order_acquire)) break;
            }

            // Um job enfileirado pode ser cancelado por shutdown ou por uma falha terminal de
            // frame. Entrega sua conclusao antes de Stopped para a GUI liberar o ownership.
            std::optional<Impl::QueuedRendererJob> CancelledRendererJob;
            {
                std::lock_guard WorkLock(Thread.WorkMutex);
                CancelledRendererJob = std::move(Thread.PendingRendererJob);
                Thread.PendingRendererJob.reset();
            }
            if (CancelledRendererJob && CancelledRendererJob->Completion) {
                JobCompletion Completion;
                Completion.Error = "render thread encerrada antes de executar o job";
                CancelledRendererJob->Completion(std::move(Completion));
            }

            try {
                Thread.Ready.store(false, std::memory_order_release);
                Thread.State->Available.store(false, std::memory_order_release);
                auto Renderer = Handle.Lock();
                Renderer->Shutdown();
            } catch (const std::exception& Error) {
                Smile::LogError(std::string("Falha absorvida ao encerrar a render thread: ") +
                                Error.what());
            } catch (...) {
                Smile::LogError("Falha desconhecida absorvida ao encerrar a render thread");
            }
            if (ComInitialized) CoUninitialize();
            Thread.Exited.store(true, std::memory_order_release);
            if (Thread.Hooks.Stopped) Thread.Hooks.Stopped();
        });
    }

    void RenderThread::RequestStop() {
        if (!Implementation || !Implementation->Started.load(std::memory_order_acquire)) return;
        Impl& I = *Implementation;
        I.Ready.store(false, std::memory_order_release);
        {
            std::lock_guard Lock(I.WorkMutex);
            I.StopRequested = true;
            I.FrameRequested = false;
        }
        I.WorkReady.notify_one();
    }

    void RenderThread::Join() {
        if (!Implementation) return;
        Impl& I = *Implementation;
        if (!I.Worker.joinable()) return;

        // O fluxo normal chega aqui pelo callback Stopped, quando o worker ja saiu de todo
        // DXGI. Este fallback cobre destrucoes excepcionais sem bloquear um SendMessage que
        // Initialize/Present tenha enviado para a thread dona do HWND.
        if (!I.Exited.load(std::memory_order_acquire) && I.NativeWindow &&
            GetWindowThreadProcessId(I.NativeWindow, nullptr) == GetCurrentThreadId()) {
            HANDLE WorkerHandle = static_cast<HANDLE>(I.Worker.native_handle());
            while (!I.Exited.load(std::memory_order_acquire)) {
                const DWORD WaitResult = MsgWaitForMultipleObjectsEx(
                    1, &WorkerHandle, INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
                if (WaitResult == WAIT_OBJECT_0) break;
                if (WaitResult != WAIT_OBJECT_0 + 1) break;
                // PeekMessage despacha mensagens enviadas de forma sincrona antes de consultar
                // a fila; PM_NOREMOVE evita rodar callbacks Qt postados durante o teardown.
                MSG Message{};
                PeekMessageW(&Message, nullptr, 0, 0, PM_NOREMOVE);
            }
        }

        I.Worker.join();
        I.Started.store(false, std::memory_order_release);
        I.FrameOutstanding.store(false, std::memory_order_release);
        I.JobOutstanding.store(false, std::memory_order_release);
    }

    bool RenderThread::RequestFrame() {
        Impl& I = *Implementation;
        if (!I.Ready.load(std::memory_order_acquire) ||
            I.JobOutstanding.load(std::memory_order_acquire)) return false;
        bool Expected = false;
        if (!I.FrameOutstanding.compare_exchange_strong(Expected, true,
                                                        std::memory_order_acq_rel))
            return false;
        {
            std::lock_guard Lock(I.WorkMutex);
            if (I.StopRequested) {
                I.FrameOutstanding.store(false, std::memory_order_release);
                return false;
            }
            I.FrameRequested = true;
        }
        I.WorkReady.notify_one();
        return true;
    }

    void RenderThread::CompleteFrame() {
        Impl& I = *Implementation;
        I.FrameOutstanding.store(false, std::memory_order_release);
        I.WorkReady.notify_one();
    }

    bool RenderThread::RequestRendererJob(
            RendererJob _Job,
            std::function<void(JobCompletion)> _Completion) {
        if (!_Job) return false;
        Impl& I = *Implementation;
        if (!I.Ready.load(std::memory_order_acquire)) return false;

        bool Expected = false;
        if (!I.JobOutstanding.compare_exchange_strong(Expected, true,
                                                       std::memory_order_acq_rel))
            return false;
        {
            std::lock_guard Lock(I.WorkMutex);
            if (I.StopRequested || !I.Ready.load(std::memory_order_acquire)) {
                I.JobOutstanding.store(false, std::memory_order_release);
                return false;
            }
            I.PendingRendererJob.emplace(Impl::QueuedRendererJob{
                std::move(_Job), std::move(_Completion) });
        }
        I.WorkReady.notify_one();
        return true;
    }

    void RenderThread::CompleteJob() {
        Impl& I = *Implementation;
        I.JobOutstanding.store(false, std::memory_order_release);
        I.WorkReady.notify_one();
    }

    bool RenderThread::IsStarted() const {
        return Implementation->Started.load(std::memory_order_acquire);
    }

    bool RenderThread::IsReady() const {
        return Implementation->Ready.load(std::memory_order_acquire);
    }

    bool RenderThread::IsStopped() const {
        return !Implementation->Started.load(std::memory_order_acquire) ||
               Implementation->Exited.load(std::memory_order_acquire);
    }

    bool RenderThread::HasFrameInFlight() const {
        return Implementation->FrameOutstanding.load(std::memory_order_acquire);
    }

    bool RenderThread::HasJobInFlight() const {
        return Implementation->JobOutstanding.load(std::memory_order_acquire);
    }
}
