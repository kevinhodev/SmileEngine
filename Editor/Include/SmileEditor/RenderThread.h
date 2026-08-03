#pragma once

#include "Smile/Core/Types.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Handle compartilhado do Renderer. Cada acesso externo adquire o mesmo lock usado pela
    // render thread, portanto nenhuma bridge da UI observa o estado enquanto RenderFrame grava
    // command lists ou apresenta a swap chain.
    class RendererHandle {
    public:
        class Access {
        public:
            Access() = default;
            Access(Access&&) noexcept = default;
            Access& operator=(Access&&) noexcept = default;
            Access(const Access&) = delete;
            Access& operator=(const Access&) = delete;

            Smile::Renderer* operator->() const { return Instance; }
            Smile::Renderer& operator*()  const { return *Instance; }
            explicit operator bool() const { return Instance != nullptr && Lock.owns_lock(); }

        private:
            struct SharedState;
            friend class RendererHandle;
            friend class RenderThread;

            Access(std::shared_ptr<SharedState> State, bool TryOnly);

            std::shared_ptr<SharedState>       StateRef;
            std::unique_lock<std::recursive_mutex> Lock;
            Smile::Renderer*                  Instance = nullptr;
        };

        RendererHandle() = default;

        // Habilita Renderer->Method(): o temporario Access conserva o lock ate o fim da
        // expressao. Para uma sequencia que guarda referencias internas, use Lock().
        Access operator->() const { return Lock(); }
        Access Lock() const;
        Access TryLock() const;
        explicit operator bool() const;

    private:
        friend class RenderThread;
        explicit RendererHandle(std::shared_ptr<Access::SharedState> State)
            : StateRef(std::move(State)) {}

        std::shared_ptr<Access::SharedState> StateRef;
    };

    // Executa Initialize, RenderFrame, Present e Shutdown em uma thread dedicada. Frames sao
    // coalescidos: a UI so pode pedir outro depois de consumir a conclusao do anterior.
    class RenderThread {
    public:
        struct FrameCompletion {
            bool        Success  = false;
            bool        Terminal = false;
            std::string Error;
        };

        struct JobCompletion {
            bool        Success = false;
            std::string Error;
        };
        using RendererJob = std::function<JobCompletion(Smile::Renderer&)>;

        struct Callbacks {
            std::function<void()>                             Initialized;
            std::function<void(FrameCompletion)>              FrameCompleted;
            std::function<void(const std::string&)>            InitializationFailed;
            std::function<void()>                             Stopped;
        };

        RenderThread();
        ~RenderThread();

        RenderThread(const RenderThread&) = delete;
        RenderThread& operator=(const RenderThread&) = delete;

        void Start(void* NativeWindow, Smile::u32 Width, Smile::u32 Height,
                   Callbacks ThreadCallbacks);
        // RequestStop nunca espera pelo worker: a thread que bombeia mensagens deve continuar
        // viva ate o callback Stopped, pois Initialize/Present podem usar SendMessage.
        void RequestStop();
        // Chamado normalmente depois de Stopped. O fallback de teardown bombeia apenas
        // mensagens sincronas do Win32 enquanto aguarda, nunca eventos Qt postados.
        void Join();

        // Retorna false quando ja existe um frame solicitado/em execucao/aguardando consumo.
        bool RequestFrame();
        void CompleteFrame();

        // Jobs GPU sao serializados no mesmo worker dos frames. JobOutstanding permanece ativo
        // ate a GUI consumir o callback, impedindo que o proximo frame observe bridges antigas.
        bool RequestRendererJob(RendererJob Job,
                                std::function<void(JobCompletion)> Completion);
        void CompleteJob();

        bool IsStarted() const;
        bool IsReady() const;
        bool IsStopped() const;
        bool HasFrameInFlight() const;
        bool HasJobInFlight() const;

        RendererHandle GetRenderer() const { return Handle; }

    private:
        struct Impl;
        RendererHandle       Handle;
        std::unique_ptr<Impl> Implementation;
    };
}
