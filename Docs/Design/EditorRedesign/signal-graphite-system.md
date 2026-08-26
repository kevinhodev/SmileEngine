# Smile Editor — Signal Graphite

## Direção

Conceito deliberadamente oposto ao Midnight Studio: em vez de superfícies suaves e cantos generosos, uma estética esquemática e "function-forward". Grafite neutro sem tint de cor, hairlines de 1 px como elemento de identidade (não apenas separadores), tipografia mono para tudo que é dado — números, coordenadas, métricas, console — e um único accent vivo (laranja-sinal) reservado para seleção, estado ativo e dirty state. A interface expõe a estrutura em vez de escondê-la: réguas com ticks no viewport, contadores visíveis, chrome fino e denso.

## Tokens principais

- Canvas (viewport): `#09090A`
- Shell: `#111113`
- Surface: `#18181B`
- Surface elevada: `#1F1F23`
- Hairline: `#26262B`
- Hairline forte: `#333338`
- Texto principal: `#F2F2F0`
- Texto secundário: `#A0A0A8`
- Texto muted: `#5F5F66`
- Accent / seleção / dirty: `#FF6B2C`
- Accent fundo: `rgba(255, 107, 44, 0.12)`
- Luz / atenção: `#F5C043`
- Info / GPU: `#6FA8FF`
- OK / pronto: `#46D68C`
- Erro: `#FF5C5C`

## Tipografia e escala

- Sans: Inter (já empacotada no editor) para labels, nomes e menus.
- Mono: JetBrains Mono, fallback Cascadia Mono/Consolas — identidade do conceito. Usada em todos os valores numéricos, coordenadas, timestamps, métricas, atalhos e contadores.
- 10 px: headers de seção em caixa alta com tracking de 1.5 px.
- 11 px mono: logs, métricas, badges de contagem.
- 12–13 px: controles, árvore de cena, campos.
- 14 px: nome do objeto selecionado.
- Espaçamento base: 4 px; linhas da árvore com 26 px; controles com 28 px.
- Raio: 2 px em chips e badges, 3 px em inputs e botões, 4 px em cards. Nada acima de 4 px.

## Decisões de UX

- Split clássico de engine: outliner à esquerda, inspector à direita, viewport máximo no centro — em vez do painel único do Midnight Studio.
- Toolbar de transformação encostada no topo do viewport (não flutuante): gizmo Q/W/E/R, espaço Global/Local, snaps com valores em mono, modos de shading e visualização.
- Console vira faixa inferior fixa com abas (Console/Build/Profiler/Assets) e um footer de métricas em mono: FPS, frame, GPU, meshes visíveis, memória. A barra de status separada deixa de existir.
- Dirty state é um ponto accent no breadcrumb da cena e no botão de salvar do outliner.
- Réguas com ticks e cantos marcados no viewport reforçam a leitura esquemática sem poluir o render.
- Ícones Lucide com traço de 1.7 px, caixa de 24 px, cantos e caps arredondados; recoloridos por estado (muted → secundário → primário → accent).
- Cores semânticas têm papel fixo: âmbar = luz, azul = info/GPU, verde = pronto/ok, vermelho = erro. O accent nunca é usado para semântica.
