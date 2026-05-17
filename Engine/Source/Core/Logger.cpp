#include "Smile/Core/Logger.h"

#include <cstdio>
#include <string>
#include <Windows.h>

namespace Smile {

namespace {
LogSink g_sink;

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:    return "[INFO] ";
        case LogLevel::Warning: return "[WARN] ";
        case LogLevel::Error:   return "[ERR ] ";
    }
    return "[????] ";
}
} // anon

void SetLogSink(LogSink sink) {
    g_sink = std::move(sink);
}

void Log(LogLevel level, std::string_view message) {
    std::string formatted;
    formatted.reserve(message.size() + 16);
    formatted.append(LevelTag(level));
    formatted.append(message);
    formatted.push_back('\n');

    OutputDebugStringA(formatted.c_str());
    std::fputs(formatted.c_str(), stderr);

    if (g_sink) {
        g_sink(level, message);
    }
}

} // namespace Smile
