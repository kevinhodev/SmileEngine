#include "SmileEditor/MenuBridge.h"

namespace SmileEditor {
    MenuBridge::MenuBridge(QObject* _Parent)
        : QObject(_Parent)
    {
    }

    void MenuBridge::SetConsoleVisible(bool _V) {
        if (ConsoleVis == _V) return;
        ConsoleVis = _V;
        emit ConsoleVisibleChanged();
    }

    void MenuBridge::SetPathTracerEnabled(bool _V) {
        if (PathTracerOn == _V) return;
        PathTracerOn = _V;
        emit PathTracerEnabledChanged();
    }
}
