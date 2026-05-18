#include "Smile/Core/Logger.h"
#include <cstdio>
#include <string>
#include <Windows.h>

namespace Smile {
    namespace {
        LogSink GLogSink;

        const char* LevelTag(LogLevel _LogLevel) {
            switch (_LogLevel) {
                case LogLevel::Info:    return "[INFO] ";
                case LogLevel::Warning: return "[WARN] ";
                case LogLevel::Error:   return "[ERR ] ";
            }
            return "[????] ";
        }
    } 

    void SetLogSink(LogSink _LogSink) {
        GLogSink = std::move(_LogSink);
    }

    void Log(LogLevel _LogLevel, std::string_view _Message) {
        std::string Formatted;
        Formatted.reserve(_Message.size() + 16);
        Formatted.append(LevelTag(_LogLevel));
        Formatted.append(_Message);
        Formatted.push_back('\n');

        OutputDebugStringA(Formatted.c_str());
        std::fputs(Formatted.c_str(), stderr);

        if (GLogSink) {
            GLogSink(_LogLevel, _Message);
        }
    }
} 
