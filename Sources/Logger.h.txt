#pragma once

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

    void SetLogSink(LogSink sink);
    void Log(LogLevel level, std::string_view message);

    inline void LogInfo(std::string_view m)    { Log(LogLevel::Info, m); }
    inline void LogWarning(std::string_view m) { Log(LogLevel::Warning, m); }
    inline void LogError(std::string_view m)   { Log(LogLevel::Error, m); }
} 
