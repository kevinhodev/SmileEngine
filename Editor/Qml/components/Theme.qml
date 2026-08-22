pragma Singleton
import QtQuick

// Tokens do conceito Signal Graphite (Docs/Design/EditorRedesign): grafite neutro,
// hairlines de 1 px e accent laranja reservado a selecao, estado ativo e dirty.
// Cores semanticas tem papel FIXO e nunca usam o accent: ambar = luz, azul = info/GPU,
// verde = pronto/ok, vermelho = erro.
QtObject {
    // ---- Tipografia ----
    // Inter e registrada no boot a partir de Editor/Fonts.
    readonly property string fontFamily: "Inter"

    // Dados numericos, coordenadas, atalhos e metricas usam a primeira mono disponivel.
    readonly property var monoFamilies: ["JetBrains Mono", "Cascadia Mono", "Consolas"]
    readonly property string fontMono: {
        const Available = Qt.fontFamilies()
        for (let I = 0; I < monoFamilies.length; ++I) {
            if (Available.indexOf(monoFamilies[I]) >= 0) return monoFamilies[I]
        }
        return "monospace"
    }
    readonly property string fontConsole: fontMono

    // ---- Superficies ----
    readonly property color canvas: "#09090A"          // viewport
    readonly property color shell: "#111113"           // chrome (barras, docks)
    readonly property color surface: "#18181B"         // cards, inputs, chips
    readonly property color surfaceRaised: "#1F1F23"   // popups, badges
    readonly property color hairline: "#26262B"        // fio de 1 px padrao
    readonly property color hairlineStrong: "#333338"  // fio de separacao forte

    // ---- Texto ----
    // Papeis fixos no Inspector:
    //   textSecondary -> CABECALHO de secao (mono, caixa alta, 10 px)
    //   textMuted     -> ROTULO de propriedade (sans, 11 px)
    //   textPrimary   -> VALOR (mono nos numeros, sans nos nomes)
    readonly property color textPrimary: "#F2F2F0"
    readonly property color textNormal: "#C9C9CF"
    readonly property color textSecondary: "#A0A0A8"
    readonly property color textMuted: "#5F5F66"
    readonly property color textDisabled: "#3A3A40"

    // ---- Accent (selecao / ativo / dirty) ----
    readonly property color accent: "#FF6B2C"
    readonly property color accentBg: Qt.rgba(1.0, 0.42, 0.173, 0.12)
    readonly property color accentBgStrong: Qt.rgba(1.0, 0.42, 0.173, 0.18)
    readonly property color accentBorder: Qt.rgba(1.0, 0.42, 0.173, 0.45)
    readonly property color accentText: "#FF8A54"

    // ---- Semanticas (papel fixo) ----
    readonly property color amber: "#F5C043"   // luz
    readonly property color blue: "#6FA8FF"    // info / GPU
    readonly property color green: "#46D68C"   // pronto / ok
    readonly property color error: "#FF5C5C"   // erro
    readonly property color warn: "#F5C043"

    // Fundos/bordas semanticas (badges e chips das janelas ainda nao redesenhadas).
    readonly property color blueBg: "#14202E"
    readonly property color blueBorder: "#2A3F5C"
    readonly property color greenBg: "#122A20"
    readonly property color greenBorder: "#2A5C44"
    readonly property color greenText: "#6FE0A8"
    readonly property color errorBg: "#2A1414"
    readonly property color errorBorder: "#5C2A2A"

    // Eixos do gizmo e dos campos de transform: X vermelho, Y verde, Z azul.
    readonly property color axisX: "#FF5C5C"
    readonly property color axisY: "#46D68C"
    readonly property color axisZ: "#6FA8FF"

    // ---- Aliases de superficie ----
    readonly property color bg: shell
    readonly property color windowBorder: hairlineStrong
    readonly property color cardBg: surface
    readonly property color borderColor: hairline
    readonly property color divider: hairline

    // Superficies de controle (toggles/chips/inputs)
    readonly property color controlBg: surface
    readonly property color controlBorder: hairlineStrong
    readonly property color controlHover: surfaceRaised
    readonly property color knob: "#F2F2F0"

    // ---- Metrica ----
    // Espacamento base 4 px; linhas de arvore 26 px; controles 28 px. Raio: 2 em chips e
    // badges, 3 em inputs e botoes, 4 em cards — nada acima de 4.
    readonly property int rowHeight: 26
    readonly property int controlHeight: 28
    readonly property int radiusChip: 2
    readonly property int radiusControl: 3
    readonly property int radiusCard: 4
}
