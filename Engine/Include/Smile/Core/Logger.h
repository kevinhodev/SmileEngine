#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace Smile {
    enum class LogLevel {
        Info,
        Warning,
        Error
    };

    using LogSink = std::function<void(LogLevel, std::string_view)>;

    struct LogConfig {
        std::filesystem::path FilePath;
        bool                  Append = false;
    };

    // Inicializa o arquivo persistente do logger. O sink de UI e independente e pode ser
    // conectado depois; assim o boot inteiro fica registrado mesmo antes do editor montar.
    [[nodiscard]] bool InitializeLogger(const LogConfig& config) noexcept;
    void ShutdownLogger() noexcept;
    void FlushLogs() noexcept;

    // Thread-safe e quiescente: ao retornar, o sink anterior nao sera mais chamado.
    void SetLogSink(LogSink sink);
    void Log(LogLevel level, std::string_view message) noexcept;

    inline void LogInfo(std::string_view m) noexcept    { Log(LogLevel::Info, m); }
    inline void LogWarning(std::string_view m) noexcept { Log(LogLevel::Warning, m); }
    inline void LogError(std::string_view m) noexcept   { Log(LogLevel::Error, m); }
} 
