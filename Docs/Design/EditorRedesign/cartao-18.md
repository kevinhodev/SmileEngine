# Smile Editor — Cartão 18%

Substitui o Signal Graphite (`signal-graphite-system.md`). Mockups: `smile-editor-cartao-18.svg`
(janela principal) e `smile-editor-cartao-18-sistema.svg` (tokens e componentes), com PNG de preview.

## Direção

O Signal Graphite caiu no visual genérico de ferramenta gerada: Inter, quase-preto, um laranja,
cabeçalhos em mono caixa-alta com tracking, chips em pílula, ícones de ponta redonda. O Cartão 18%
parte do domínio: é um editor de **render**, então a UI ao redor do viewport é cinza **neutro** — o mesmo raciocínio do Godot 4.6 (trocou o tint azul por cinza porque distorcia o branco
percebido no viewport) e da opção de fundo cinza do DaVinci Resolve.

- Painéis encostados, separados por um **fio escuro** (sulco), não por hairline clara.
- Campos **rebaixados** (mais escuros que o painel); botões **em relevo** (um degrau acima).
- Um accent só — **amarelo Smile** (`#F4C430`, a cor do logo) — para seleção, painel com foco e
  ferramenta ativa. Checkbox ligado, filtro ativo e aba ativa NÃO usam accent: usam cinza claro.
- Cores semânticas com papel fixo: laranja = luz/aviso, azul = info/GPU/mesh, verde = ok,
  verde-água = ambiente, vermelho = erro. Ícone da árvore colore pelo TIPO, não pela seleção.

## Tokens

Em 2026-10-26 a paleta desceu ~9 níveis (o cinza médio do primeiro corte ficou claro demais): os
degraus continuam com o mesmo intervalo entre si, então relevo e rebaixo seguem legíveis.

Fonte única: `Editor/Qml/components/Theme.qml`. O QSS (`Editor/Styles`) e a paleta Fusion
(`DarkTheme.cpp`) repetem os hex porque não leem o singleton.

| Papel | Token | Hex |
| --- | --- | --- |
| Fundo / gutters | `canvas` | `#121212` |
| Chrome (título, abas, status) | `chrome` | `#1A1A1A` |
| Painel | `shell` | `#212121` |
| Faixa de seção | `section` | `#282828` |
| Card de janela flutuante | `card` | `#282828` |
| Campo (rebaixado) | `surface` | `#171717` |
| Botão (relevo) / ligado | `surfaceRaised` / `raisedOn` | `#2E2E2E` / `#3C3C3C` |
| Fio claro dentro do painel | `hairline` | `#2E2E2E` |
| Fio escuro entre painéis | `hairlineStrong` | `#0F0F0F` |
| Texto valor / rótulo / meta / desligado | `textPrimary` / `textSecondary` / `textMuted` / `textDisabled` | `#EDEDED` / `#BDBDBD` / `#8C8C8C` / `#595959` |
| Accent / linha selecionada | `accent` / `accentBg` | `#F4C430` / `#3D3726` |

## Tipografia

- **Archivo Smile** — Archivo (SIL OFL) cortada em `wdth 88` (SemiCondensed), `wght 400..700`
  variável, com **algarismos tabulares como padrão**. Gerada por `Tools/make_archivo_smile.py`.
  Números usam a mesma família da UI (`Theme.fontNum`): alinham em coluna sem `font.features`.
- **IBM Plex Mono** (SIL OFL) — só console, hex de cor e caminhos (`Theme.fontMono`).
- Escala em px inteiros: 15 título de objeto · 13 seção/aba · 12 corpo/rótulo/valor · 11 meta.
- Caixa normal em tudo. Nada de caixa-alta com tracking.

## Métrica e forma

- Grade de 4 px; linha de árvore 26; controle 24 (inspector) / 26 (toolbar); faixa de aba 30;
  faixa de seção 28; barra de título 36; status 26.
- Raio: 0 nos painéis, 2 em badges, 3 em controles, 4 em cards.
- Coluna de rótulo das linhas de propriedade: `Theme.labelRatio` (42% da largura), a mesma do
  `ScrubRow` — campos de seções diferentes começam na mesma vertical.
- Ícones Lucide com ponta **reta** e junção em quina, traço 2 na caixa de 24.

## Componentes

- **Seção**: faixa de borda a borda (vaza a margem da coluna), título em semibold.
- **Aba de dock**: bloco da cor do painel ligado ao conteúdo; traço accent em cima só com foco.
- **Campo XYZ**: barra de 3 px na cor do eixo; valor à direita.
- **Slider de preenchimento** (`ScrubField`): o campo enche até o valor; arrastando, esquenta.
- **Checkbox** (`Toggle`) no lugar do switch: 14 px, ligado = cinza claro com marca escura.
- **Filtro**: controle segmentado num trilho rebaixado.
- **Console**: aba em bloco, filtros com ponto semântico + contagem, aviso/erro tingem a linha.

## Layout

O Inspector desce até o rodapé (`setCorner(BottomRight, RightDockWidgetArea)`); o console passa
só por baixo do outliner e do viewport.

## Pendente

As janelas flutuantes (Settings, Materiais, Stats, Time of Day, Render targets, ReSTIR) herdam
tokens e fontes, mas a estrutura interna delas ainda é a do Signal Graphite.
