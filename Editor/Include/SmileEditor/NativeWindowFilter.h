#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QtGlobal>
#include <qwindowdefs.h> // WId

namespace SmileEditor {
    class WindowBridge;

    // Torna a janela frameless preservando resize/Aero Snap/sombra/maximizar nativos (Windows),
    // espelhando o Slate da Unreal (WindowsApplication.cpp: WM_NCCALCSIZE + WM_NCHITTEST). As
    // "zonas" (faixa da barra + areas interativas) vem do WindowBridge, alimentado pela QML.
    // Em outros SOs o filtro e no-op; um backend por-OS pode ser somado reusando o WindowBridge.
    class NativeWindowFilter : public QObject, public QAbstractNativeEventFilter {
        Q_OBJECT

    public:
        NativeWindowFilter(WId targetWindow, WindowBridge* bridge);

        bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

        // Forca o recalculo do frame + sombra DWM apos instalar o filtro (chamar com winId valido).
        static void EnableFrameless(WId targetWindow);

    signals:
        // O loop modal de mover/redimensionar do Windows. O viewport conserva o ultimo tamanho
        // durante o arraste e realoca swapchain/targets uma unica vez ao sair desse loop.
        void InteractiveResizeStarted();
        void InteractiveResizeFinished();

    private:
        WId           Target;
        WindowBridge* Bridge;
    };
}
