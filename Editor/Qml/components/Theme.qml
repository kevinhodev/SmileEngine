pragma Singleton
import QtQuick

// Tokens do conceito Cartao 18% (Docs/Design/EditorRedesign/cartao-18.md): cinza NEUTRO escuro
// ao redor do render — a UI nao puxa o branco percebido do viewport, como acontecia com o preto
// e com tints azulados —, fios escuros entre paineis e um accent so (amarelo Smile, a cor do
// logo) reservado a selecao, painel com foco e ferramenta ativa.
// Cores semanticas tem papel FIXO e nunca usam o accent: laranja = luz/aviso, azul = info/GPU,
// verde = pronto/ok, vermelho = erro, verde-agua = ambiente.
QtObject {
    // ---- Tipografia ----
    // "Archivo Smile" = Archivo SemiCondensed com algarismos tabulares como padrao (gerada por
    // Tools/make_archivo_smile.py). Registrada no boot a partir de Editor/Fonts.
    readonly property string fontFamily: "Archivo Smile"
    // Numeros usam a MESMA familia da UI: os digitos ja sao tabulares, entao alinham em coluna e
    // nao dancam de largura a cada frame. Mono fica so p/ console, hex de cor e caminhos.
    readonly property string fontNum: fontFamily

    readonly property var monoFamilies: ["IBM Plex Mono", "Cascadia Mono", "Consolas"]
    readonly property string fontMono: {
        const Available = Qt.fontFamilies()
        for (let I = 0; I < monoFamilies.length; ++I) {
            if (Available.indexOf(monoFamilies[I]) >= 0) return monoFamilies[I]
        }
        return "monospace"
    }
    readonly property string fontConsole: fontMono

    // Escala (px inteiros — font.pixelSize e int no QML).
    readonly property int textTitle: 15     // nome do objeto selecionado
    readonly property int textSection: 13   // cabecalho de secao, aba ativa
    readonly property int textBody: 12      // rotulo, valor, item de arvore, controle
    readonly property int textMeta: 11      // contagem, dica, resumo de secao fechada

    // ---- Superficies (degraus de luminancia, sem tint) ----
    readonly property color canvas: "#121212"          // fundo atras do viewport / gutters
    readonly property color chrome: "#1A1A1A"          // barra de titulo, faixas de aba, status
    readonly property color shell: "#212121"           // corpo dos paineis (docks)
    readonly property color section: "#282828"         // faixa do cabecalho de secao
    readonly property color sectionTop: "#2D2D2D"      // brilho de 1 px no topo da faixa
    readonly property color surface: "#171717"         // campos: rebaixados em relacao ao painel
    readonly property color surfaceHover: "#1B1B1B"
    readonly property color surfaceRaised: "#2E2E2E"   // botoes, popups, badges
    readonly property color raisedHover: "#353535"
    readonly property color raisedOn: "#3C3C3C"        // segmento/botao ligado
    readonly property color raisedTop: "#363636"       // brilho de 1 px no topo do botao
    readonly property color hairline: "#2E2E2E"        // fio claro DENTRO de um painel
    readonly property color hairlineStrong: "#0F0F0F"  // fio escuro ENTRE paineis
    readonly property color treeGuide: "#303030"       // guia de indentacao da arvore
    readonly property color rowHover: "#282828"
    // Card das janelas flutuantes: um degrau ACIMA do painel, para os campos (rebaixados, cor
    // de `surface`) continuarem legiveis dentro dele.
    readonly property color card: "#282828"

    // Slider de preenchimento: o proprio campo enche ate o valor.
    readonly property color sliderFill: "#3A3A3A"
    readonly property color sliderEdge: "#5A5A5A"
    readonly property color sliderDrag: "#4A4428"

    // ---- Texto ----
    // Papeis fixos:
    //   textPrimary   -> VALOR, nome, cabecalho de secao
    //   textSecondary -> ROTULO de propriedade, item de menu
    //   textMuted     -> meta, contagem, placeholder, aba inativa
    //   textDisabled  -> desligado
    readonly property color textPrimary: "#EDEDED"
    readonly property color textNormal: "#D4D4D4"
    readonly property color textSecondary: "#BDBDBD"
    readonly property color textMuted: "#8C8C8C"
    readonly property color textDisabled: "#595959"

    // ---- Accent (selecao / foco / ativo) ----
    readonly property color accent: "#F4C430"
    readonly property color accentInk: "#1E1E1E"       // texto/icone SOBRE o accent
    readonly property color accentBg: "#3D3726"        // linha selecionada (accent ~13% no painel)
    readonly property color accentBgStrong: "#4A4228"
    readonly property color accentBorder: Qt.rgba(0.957, 0.769, 0.188, 0.55)
    readonly property color accentText: "#F2CF62"

    // ---- Semanticas (papel fixo) ----
    readonly property color amber: "#F2A33A"   // luz
    readonly property color blue: "#7FB2EC"    // info / GPU / mesh
    readonly property color green: "#6CC77E"   // pronto / ok
    readonly property color teal: "#6CC7A4"    // ambiente (sol, clima, nuvens, oceano)
    readonly property color error: "#F0524F"   // erro
    readonly property color warn: "#F2A33A"

    readonly property color warnRow: "#2E2920"
    readonly property color warnText: "#F4D9A8"
    readonly property color errorText: "#F7B0AE"

    // Fundos/bordas semanticas (badges e chips das janelas flutuantes).
    readonly property color blueBg: "#23303D"
    readonly property color blueBorder: "#3A5470"
    readonly property color greenBg: "#22312A"
    readonly property color greenBorder: "#3D5E48"
    readonly property color greenText: "#9BDDA9"
    readonly property color errorBg: "#362322"
    readonly property color errorBorder: "#5A3331"

    // Eixos do gizmo e dos campos de transform: X vermelho, Y verde, Z azul.
    readonly property color axisX: "#F0524F"
    readonly property color axisY: "#7BC94B"
    readonly property color axisZ: "#4C8DF0"

    // ---- Aliases (janelas que ainda usam os nomes antigos) ----
    readonly property color bg: shell
    readonly property color windowBorder: hairlineStrong
    readonly property color cardBg: card
    readonly property color borderColor: hairline
    readonly property color divider: hairline

    // Superficies de controle (toggles/chips/inputs)
    readonly property color controlBg: surface
    readonly property color controlBorder: hairlineStrong
    readonly property color controlHover: raisedHover
    readonly property color knob: "#D9D9D9"

    // ---- Metrica ----
    // Grade de 4 px; linhas de arvore 26 px; controles 24 (inspector) / 26 (toolbar).
    // Raio: 0 nos paineis (encostam com fio escuro), 2 em badges, 3 em controles, 4 em cards.
    readonly property int rowHeight: 26
    readonly property int controlHeight: 24
    readonly property int tabHeight: 30
    readonly property int sectionHeight: 28
    // Coluna de rotulo das linhas de propriedade: fracao da largura da linha, a MESMA do
    // ScrubRow (campo = 58%). Rotulo em PT-BR e comprido ("Projeta sombra"); proporcao em vez
    // de px fixo mantem o alinhamento entre linhas e nao espreme as janelas largas.
    readonly property real labelRatio: 0.42
    readonly property int radiusChip: 2
    readonly property int radiusControl: 3
    readonly property int radiusCard: 4
}
