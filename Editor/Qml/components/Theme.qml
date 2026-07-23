pragma Singleton
import QtQuick

// Paleta unica do editor (dark quente). Fonte de verdade dos componentes compartilhados
// e dos paines/janelas — antes cada .qml carregava a propria copia e elas ja tinham
// divergido (blueBg do TOD != do Materials). Mudou aqui, mudou em todo lugar.
QtObject {
    // Familia unica da UI. Trocar aqui troca no editor inteiro — antes eram 182 literais
    // "Segoe UI" espalhados. Se a fonte escolhida nao estiver instalada/registrada, o Qt
    // cai para a proxima da lista sozinho.
    // Inter (SIL OFL) vem empacotada em Editor/Fonts e e registrada no boot pelo main.cpp.
    // Se o registro falhar, o Qt cai na familia do sistema sozinho.
    readonly property string fontFamily: "Inter"
    // Para numeros que mudam a cada frame (fps, ms, resolucao): digitos de largura fixa
    // evitam o texto "dancando" de largura a cada atualizacao.
    readonly property string fontMono: "Consolas"

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
