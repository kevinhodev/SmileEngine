#include "Smile/Core/Logger.h"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <Windows.h>

namespace Smile {
    namespace {
        enum class ESinkState {
            Starting,
            Accepting,
            Retired
        };

        struct FSinkSlot {
            explicit FSinkSlot(LogSink _Sink)
                : Sink(std::move(_Sink)) {}

            LogSink                Sink;
            std::mutex             Mutex;
            std::condition_variable StateChanged;
            size_t                 InFlight = 0;
            ESinkState             State = ESinkState::Starting;
        };

        struct FBacklogEntry {
            LogLevel   Level;
            std::string Message;
        };

        constexpr size_t kMaxStartupBacklog = 512;

        std::mutex                 GSetSinkMutex;
        std::mutex                 GSinkMutex;
        std::shared_ptr<FSinkSlot> GLogSink;
        std::deque<FBacklogEntry>  GStartupBacklog;
        bool                       GCollectStartupBacklog = true;
        thread_local FSinkSlot*    GInvokingSink = nullptr;

        std::mutex    GFileMutex;
        std::ofstream GLogFile;

        const char* LevelTag(LogLevel _LogLevel) noexcept {
            switch (_LogLevel) {
                case LogLevel::Info:    return "INFO";
                case LogLevel::Warning: return "WARN";
                case LogLevel::Error:   return "ERR ";
            }
            return "????";
        }

        std::string FormatLine(LogLevel _LogLevel, std::string_view _Message) {
            SYSTEMTIME Time{};
            GetSystemTime(&Time);

            char Prefix[96]{};
            const int PrefixLength = std::snprintf(
                Prefix, sizeof(Prefix),
                "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%s] [T%lu] ",
                static_cast<unsigned>(Time.wYear),
                static_cast<unsigned>(Time.wMonth),
                static_cast<unsigned>(Time.wDay),
                static_cast<unsigned>(Time.wHour),
                static_cast<unsigned>(Time.wMinute),
                static_cast<unsigned>(Time.wSecond),
                static_cast<unsigned>(Time.wMilliseconds),
                LevelTag(_LogLevel),
                static_cast<unsigned long>(GetCurrentThreadId()));

            std::string Formatted;
            Formatted.reserve(static_cast<size_t>(PrefixLength) + _Message.size() + 1);
            Formatted.append(Prefix, static_cast<size_t>(PrefixLength));
            Formatted.append(_Message);
            Formatted.push_back('\n');
            return Formatted;
        }

        void OutputDebugUtf8(std::string_view _Text) {
            if (_Text.empty()) return;

            const int WideLength = MultiByteToWideChar(
                CP_UTF8, 0, _Text.data(), static_cast<int>(_Text.size()), nullptr, 0);
            if (WideLength <= 0) {
                OutputDebugStringA(std::string(_Text).c_str());
                return;
            }

            std::wstring Wide(static_cast<size_t>(WideLength), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, _Text.data(), static_cast<int>(_Text.size()),
                                Wide.data(), WideLength);
            OutputDebugStringW(Wide.c_str());
        }

        void RawLoggerFailure(const char* _Message) noexcept {
            OutputDebugStringA(_Message);
            std::fputs(_Message, stderr);
        }
    }

    bool InitializeLogger(const LogConfig& _Config) noexcept {
        try {
            {
                std::lock_guard Lock(GSinkMutex);
                GStartupBacklog.clear();
                GCollectStartupBacklog = true;
            }

            std::lock_guard Lock(GFileMutex);

            if (GLogFile.is_open()) {
                GLogFile.flush();
                GLogFile.close();
            }

            if (_Config.FilePath.empty()) return true;

            const std::filesystem::path Parent = _Config.FilePath.parent_path();
            if (!Parent.empty()) {
                std::error_code Error;
                std::filesystem::create_directories(Parent, Error);
                if (Error) return false;
            }

            const auto Mode = std::ios::binary | std::ios::out |
                (_Config.Append ? std::ios::app : std::ios::trunc);
            GLogFile.open(_Config.FilePath, Mode);
            return GLogFile.is_open();
        } catch (...) {
            return false;
        }
    }

    void ShutdownLogger() noexcept {
        try {
            SetLogSink({});
        } catch (...) {
        }

        std::lock_guard Lock(GFileMutex);
        if (!GLogFile.is_open()) return;
        GLogFile.flush();
        GLogFile.close();
    }

    void FlushLogs() noexcept {
        std::lock_guard Lock(GFileMutex);
        if (GLogFile.is_open()) GLogFile.flush();
        std::fflush(stderr);
    }

    void SetLogSink(LogSink _LogSink) {
        std::lock_guard SetLock(GSetSinkMutex);

        std::shared_ptr<FSinkSlot> NewSink;
        if (_LogSink) NewSink = std::make_shared<FSinkSlot>(std::move(_LogSink));

        std::shared_ptr<FSinkSlot> OldSink;
        std::deque<FBacklogEntry> StartupBacklog;
        {
            std::lock_guard Lock(GSinkMutex);
            OldSink = std::exchange(GLogSink, std::move(NewSink));
            if (GLogSink && GCollectStartupBacklog) {
                StartupBacklog.swap(GStartupBacklog);
                GCollectStartupBacklog = false;
            }
        }

        if (OldSink) {
            std::unique_lock Lock(OldSink->Mutex);
            OldSink->State = ESinkState::Retired;
            OldSink->StateChanged.notify_all();
            if (GInvokingSink != OldSink.get()) {
                OldSink->StateChanged.wait(
                    Lock, [&] { return OldSink->InFlight == 0; });
            }
        }

        std::shared_ptr<FSinkSlot> ActiveSink;
        {
            std::lock_guard Lock(GSinkMutex);
            ActiveSink = GLogSink;
        }
        if (!ActiveSink) return;

        FSinkSlot* PreviousSink = GInvokingSink;
        GInvokingSink = ActiveSink.get();
        for (const FBacklogEntry& Entry : StartupBacklog) {
            try {
                ActiveSink->Sink(Entry.Level, Entry.Message);
            } catch (...) {
                RawLoggerFailure("[ERR ] Excecao absorvida ao reproduzir backlog do logger\n");
            }
        }
        GInvokingSink = PreviousSink;

        {
            std::lock_guard Lock(ActiveSink->Mutex);
            ActiveSink->State = ESinkState::Accepting;
            ActiveSink->StateChanged.notify_all();
        }
    }

    void Log(LogLevel _LogLevel, std::string_view _Message) noexcept {
        try {
            const std::string Formatted = FormatLine(_LogLevel, _Message);
            OutputDebugUtf8(Formatted);
            std::fwrite(Formatted.data(), 1, Formatted.size(), stderr);

            {
                std::lock_guard Lock(GFileMutex);
                if (GLogFile.is_open()) {
                    GLogFile.write(Formatted.data(), static_cast<std::streamsize>(Formatted.size()));
                    if (_LogLevel != LogLevel::Info) GLogFile.flush();
                }
            }

            std::shared_ptr<FSinkSlot> Sink;
            {
                std::lock_guard Lock(GSinkMutex);
                Sink = GLogSink;
                if (!Sink && GCollectStartupBacklog) {
                    if (GStartupBacklog.size() >= kMaxStartupBacklog)
                        GStartupBacklog.pop_front();
                    GStartupBacklog.push_back({ _LogLevel, std::string(_Message) });
                }
            }

            if (!Sink) return;

            {
                std::unique_lock Lock(Sink->Mutex);
                Sink->StateChanged.wait(
                    Lock, [&] { return Sink->State != ESinkState::Starting; });
                if (Sink->State != ESinkState::Accepting) return;
                ++Sink->InFlight;
            }

            FSinkSlot* PreviousSink = GInvokingSink;
            GInvokingSink = Sink.get();
            try {
                Sink->Sink(_LogLevel, _Message);
            } catch (...) {
                RawLoggerFailure("[ERR ] Excecao absorvida no sink do logger\n");
            }
            GInvokingSink = PreviousSink;

            {
                std::lock_guard Lock(Sink->Mutex);
                --Sink->InFlight;
                if (Sink->State == ESinkState::Retired && Sink->InFlight == 0)
                    Sink->StateChanged.notify_all();
            }
        } catch (...) {
            RawLoggerFailure("[ERR ] Falha interna absorvida pelo logger\n");
        }
    }
} 
