pragma Singleton
import QtQuick

// Paleta unica do editor — conceito "Signal Graphite" (Docs/Design/EditorRedesign).
// Fonte de verdade dos componentes compartilhados e dos paines/janelas: antes cada .qml
// carregava a propria copia e elas ja tinham divergido. Mudou aqui, mudou em todo lugar.
//
// A direcao e grafite NEUTRO (sem tint quente), hairline de 1 px como elemento de identidade
// e um unico accent vivo — laranja-sinal — reservado a selecao, estado ativo e dirty state.
// Cores semanticas tem papel FIXO e nunca usam o accent: ambar = luz, azul = info/GPU,
// verde = pronto/ok, vermelho = erro.
QtObject {
    // ---- Tipografia ----
    // Familia unica da UI. Inter (SIL OFL) vem empacotada em Editor/Fonts e e registrada no
    // boot pelo main.cpp; se o registro falhar, o Qt cai na familia do sistema sozinho.
    readonly property string fontFamily: "Inter"

    // Mono e identidade do conceito, nao so "digito de largura fixa": vale para TODO dado —
    // numeros, coordenadas, timestamps, metricas, atalhos e contadores.
    //
    // A resolucao e feita AQUI, uma vez, e nao por fallback do Qt: o value type `font` do QML
    // expoe `family` (string unica) e nao `families`, entao nao ha como declarar uma cadeia de
    // preferencia no ponto de uso — uma familia ausente cairia direto na sans do sistema, que
    // e o pior resultado possivel para uma coluna de numeros. `Qt.fontFamilies()` lista o que
    // esta instalado; pegamos a primeira da nossa ordem que exista.
    //
    // A JetBrains Mono e a preferida do conceito mas nao vem com o Windows nem esta empacotada
    // em Editor/Fonts; hoje isto resolve para a Cascadia Mono. Basta soltar o .ttf da JetBrains
    // em Editor/Fonts (o main.cpp registra tudo que estiver la no boot) para o editor passar a
    // usa-la sem tocar em nenhum call site.
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
    // O conceito trabalha com tres tiers (principal/secundario/muted). `textNormal` fica no
    // meio deles porque a base de codigo ja distingue quatro niveis; remove-lo achataria a
    // hierarquia das janelas que ainda nao passaram pelo redesign.
    // Tres papeis fixos no Inspector, e e a distancia entre eles que faz a secao se ler:
    //   textSecondary -> CABECALHO de secao (mono, caixa alta, 10 px)
    //   textMuted     -> ROTULO de propriedade (sans, 11 px)
    //   textPrimary   -> VALOR (mono nos numeros, sans nos nomes)
    // Rotulo e cabecalho no mesmo tier empatam e a secao perde o inicio — foi o que
    // aconteceu com ESTATISTICAS antes de os rotulos descerem para textMuted.
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

    // ---- Compatibilidade ----
    // Nomes herdados, ainda usados pelas janelas que nao passaram pelo redesign.
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
