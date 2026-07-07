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

    void MenuBridge::SetTimeOfDayVisible(bool _V) {
        if (TimeOfDayVis == _V) return;
        TimeOfDayVis = _V;
        emit TimeOfDayVisibleChanged();
    }
}
