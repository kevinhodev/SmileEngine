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
    static constexpr int kResizeBorder = 6; // px da borda de redimensionamento

    void NativeWindowFilter::EnableFrameless(WId _Target) {
        HWND hWnd = reinterpret_cast<HWND>(_Target);
        if (!hWnd) return;
        // Margem pequena -> DWM volta a desenhar a sombra/arredondado da janela, mesmo sem moldura.
        const MARGINS Margins{ 0, 0, 0, 1 };
        ::DwmExtendFrameIntoClientArea(hWnd, &Margins);
        // Forca o WM_NCCALCSIZE a recalcular o frame agora que o filtro ja esta instalado.
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
        case WM_NCCALCSIZE: {
            if (!IsTop || Msg->wParam == FALSE) return false;
            // Client = janela inteira (remove a moldura visual). No maximizado, insere a borda
            // (cxWindowBorders) senao o conteudo vaza pra fora da tela / cobre a taskbar — Slate.
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
            // lParam = coords de tela; medimos sempre relativo ao TOP-LEVEL (vale p/ filhos tb).
            RECT Tr; ::GetWindowRect(Top, &Tr);
            const int X = GET_X_LPARAM(Msg->lParam) - Tr.left;
            const int Y = GET_Y_LPARAM(Msg->lParam) - Tr.top;
            const int W = Tr.right - Tr.left;
            const int H = Tr.bottom - Tr.top;

            // X/Y/W/H estao em pixels FISICOS (lParam/GetWindowRect). Espessura da borda escalada
            // pelo DPI p/ manter a mesma "grossura" visual em telas 125%/150%.
            const qreal Dpr = Bridge ? Bridge->Dpr() : 1.0;
            const int Border = static_cast<int>(kResizeBorder * Dpr + 0.5);

            // Borda de resize relativa ao top-level (desativada no maximizado).
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
                // Janela filha nativa (barra/viewport) engole o hit-test do top-level. Perto da
                // borda, devolve pro pai (HTTRANSPARENT) p/ habilitar o resize por cima dela.
                if (Edge && ::GetAncestor(Msg->hwnd, GA_ROOT) == Top) {
                    *_Result = HTTRANSPARENT;
                    return true;
                }
                return false; // resto: comportamento normal do filho (HTCLIENT)
            }

            if (Edge) { *_Result = Edge; return true; }

            // Faixa da barra: a altura/rects vem da QML em pixels LOGICOS; converte o ponto
            // fisico p/ logico antes de comparar. Controles -> HTCLIENT; vazio -> HTCAPTION.
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
