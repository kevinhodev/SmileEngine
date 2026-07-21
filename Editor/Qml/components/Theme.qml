pragma Singleton
import QtQuick

// Paleta unica do editor (dark quente). Fonte de verdade dos componentes compartilhados
// e dos paines/janelas — antes cada .qml carregava a propria copia e elas ja tinham
// divergido (blueBg do TOD != do Materials). Mudou aqui, mudou em todo lugar.
QtObject {
    readonly property color bg: "#141511"
    readonly property color windowBorder: "#2e2f28"
    readonly property color cardBg: "#1a1b15"
    readonly property color borderColor: "#2a2b24"
    readonly property color divider: "#23241d"

    readonly property color textPrimary: "#e6e2d8"
    readonly property color textNormal: "#c8c2b4"
    readonly property color textSecondary: "#9a958a"
    readonly property color textMuted: "#6c6a61"

    readonly property color blue: "#5b9dff"
    readonly property color blueBg: "#1d2735"
    readonly property color blueBorder: "#31486b"
    readonly property color amber: "#e8c565"
    readonly property color green: "#9ac055"
    readonly property color warn: "#e0885a"

    // Superficies de controle (toggles/chips/inputs)
    readonly property color controlBg: "#23241d"
    readonly property color controlBorder: "#33342c"
    readonly property color controlHover: "#2a2b23"
    readonly property color knob: "#f2efe6"
}
