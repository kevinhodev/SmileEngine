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

    void MenuBridge::SetLightsVisible(bool _V) {
        if (LightsVis == _V) return;
        LightsVis = _V;
        emit LightsVisibleChanged();
    }

    void MenuBridge::SetStatsVisible(bool _V) {
        if (StatsVis == _V) return;
        StatsVis = _V;
        emit StatsVisibleChanged();
    }
}
