#include "SmileEditor/NativeWindowFilter.h"
#include "SmileEditor/WindowBridge.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace SmileEditor {
    NativeWindowFilter::NativeWindowFilter(WId _Target, WindowBridge* _Bridge)
        : Target(_Target), Bridge(_Bridge)
    {
    }

#ifdef Q_OS_WIN
    static constexpr int kResizeBorder = 6; 

    void NativeWindowFilter::EnableFrameless(WId _Target) {
        HWND hWnd = reinterpret_cast<HWND>(_Target);
        if (!hWnd) return;

        const MARGINS Margins{ 0, 0, 0, 1 };
        ::DwmExtendFrameIntoClientArea(hWnd, &Margins);
        ::SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                       SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    bool NativeWindowFilter::nativeEventFilter(const QByteArray& _EventType, void* _Message, qintptr* _Result) {
        if (_EventType != QByteArrayLiteral("windows_generic_MSG")) return false;
        MSG* Msg = static_cast<MSG*>(_Message);
        if (!Msg) return false;
        HWND Top = reinterpret_cast<HWND>(Target);
        const bool IsTop = (Msg->hwnd == Top);

        switch (Msg->message) {
        case WM_ENTERSIZEMOVE:
            if (IsTop) emit InteractiveResizeStarted();
            return false;
        case WM_EXITSIZEMOVE:
            if (IsTop) emit InteractiveResizeFinished();
            return false;
        case WM_NCCALCSIZE: {
            if (!IsTop || Msg->wParam == FALSE) return false;

            auto* Params = reinterpret_cast<NCCALCSIZE_PARAMS*>(Msg->lParam);
            if (::IsZoomed(Top)) {
                WINDOWINFO Wi{}; Wi.cbSize = sizeof(Wi);
                ::GetWindowInfo(Top, &Wi);
                Params->rgrc[0].left   += static_cast<LONG>(Wi.cxWindowBorders);
                Params->rgrc[0].top    += static_cast<LONG>(Wi.cyWindowBorders);
                Params->rgrc[0].right  -= static_cast<LONG>(Wi.cxWindowBorders);
                Params->rgrc[0].bottom -= static_cast<LONG>(Wi.cyWindowBorders);
            }
            *_Result = 0;
            return true;
        }
        case WM_NCHITTEST: {
            RECT Tr; ::GetWindowRect(Top, &Tr);
            const int X = GET_X_LPARAM(Msg->lParam) - Tr.left;
            const int Y = GET_Y_LPARAM(Msg->lParam) - Tr.top;
            const int W = Tr.right - Tr.left;
            const int H = Tr.bottom - Tr.top;

            const qreal Dpr = Bridge ? Bridge->Dpr() : 1.0;
            const int Border = static_cast<int>(kResizeBorder * Dpr + 0.5);

            int Edge = 0;
            if (!::IsZoomed(Top)) {
                const bool L = X >= 0 && X < Border;
                const bool R = X <  W && X >= W - Border;
                const bool T = Y >= 0 && Y < Border;
                const bool B = Y <  H && Y >= H - Border;
                if      (T && L) Edge = HTTOPLEFT;     else if (T && R) Edge = HTTOPRIGHT;
                else if (B && L) Edge = HTBOTTOMLEFT;  else if (B && R) Edge = HTBOTTOMRIGHT;
                else if (L)      Edge = HTLEFT;        else if (R)      Edge = HTRIGHT;
                else if (T)      Edge = HTTOP;         else if (B)      Edge = HTBOTTOM;
            }

            if (!IsTop) {
                if (Edge && ::GetAncestor(Msg->hwnd, GA_ROOT) == Top) {
                    *_Result = HTTRANSPARENT;
                    return true;
                }
                return false; 
            }

            if (Edge) { *_Result = Edge; return true; }

            const int BarH = Bridge ? Bridge->TitleBarHeight() : 0;
            const int LX = (Dpr > 0.0) ? static_cast<int>(X / Dpr) : X;
            const int LY = (Dpr > 0.0) ? static_cast<int>(Y / Dpr) : Y;
            if (LY < BarH) {
                if (Bridge && Bridge->PointIsInteractive(LX, LY)) { *_Result = HTCLIENT; return true; }
                *_Result = HTCAPTION;
                return true;
            }

            *_Result = HTCLIENT;
            return true;
        }
        default:
            return false;
        }
    }
#else
    void NativeWindowFilter::EnableFrameless(WId) {}
    bool NativeWindowFilter::nativeEventFilter(const QByteArray&, void*, qintptr*) { return false; }
#endif
}
