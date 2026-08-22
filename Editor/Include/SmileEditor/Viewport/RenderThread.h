#pragma once

#include "Smile/Core/Types.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Serializa o acesso externo com RenderFrame e os jobs do renderer.
    class RendererHandle {
    public:
        class Access {
        public:
            Access() = default;
            Access(Access&& Other) noexcept;
            Access& operator=(Access&& Other) noexcept;
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

        // O temporario mantem o lock ate o fim da expressao; use Lock() para sequencias.
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

    // Worker do renderer do Editor. Mantem no maximo um frame pendente.
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
            // Executado no worker com o renderer bloqueado; nao deve reentrar no handle.
            std::function<void(const std::string& Label,
                               const std::string& Detail, float Fraction)> Progress;
        };

        RenderThread();
        ~RenderThread();

        RenderThread(const RenderThread&) = delete;
        RenderThread& operator=(const RenderThread&) = delete;

        void Start(void* NativeWindow, Smile::u32 Width, Smile::u32 Height,
                   Callbacks ThreadCallbacks);
        // Nao bloqueia: a thread do HWND precisa continuar bombeando mensagens.
        void RequestStop();
        // Enquanto aguarda, processa apenas mensagens sincronas do Win32.
        void Join();

        // Retorna false quando ja existe um frame solicitado/em execucao/aguardando consumo.
        bool RequestFrame();
        void CompleteFrame();

        // O job permanece ativo ate a GUI consumir a conclusao com CompleteJob().
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
