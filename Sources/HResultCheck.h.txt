#pragma once

#include <stdexcept>
#include <string>
#include <winerror.h>
#include "Smile/Core/Logger.h"

namespace Smile {
    class HResultException : public std::runtime_error {
    public:
        HResultException(long hr, const char* expression, const char* file, int line)
            : std::runtime_error(BuildMessage(hr, expression, file, line)), m_hr(hr) {}

        long Code() const noexcept { return m_hr; }

    private:
        static std::string BuildMessage(long hr, const char* expr, const char* file, int line) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "HRESULT 0x%08lX em %s (%s:%d)", static_cast<unsigned long>(hr), expr, file, line);
            return buf;
        }
        long m_hr;
    };
} 

#define SMILE_HR(expr)                                                                  \
    do {                                                                                \
        const HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                              \
            ::Smile::LogError(std::string("[HR FAIL] ") + #expr);                       \
            throw ::Smile::HResultException(_hr, #expr, __FILE__, __LINE__);            \
        }                                                                               \
    } while (0)
