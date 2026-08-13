# Protocolo de captura determinística

Infraestrutura de medição da série SHaRC (`SHARC-PRIMARY-GI-PLAN.md`). O plano proíbe fechar fase
por "parece melhor"; este documento é o que dá sentido a essa proibição — sem uma régua, "melhor"
não é verificável.

O que se compara a partir da Fase 3 é ruído, convergência temporal e ocupação do hash do radiance
cache. Os três são função da posição **exata** da câmera e do estado inicial dos acumuladores, e
não da cena "mais ou menos igual".

## Estado

| Peça | Situação |
|---|---|
| Bookmarks de câmera (`<cena>.cameras.json`, 4 slots) | **Feito** — commit `3361762` |
| `TemporalSampleIndex` separado do `FrameIndex` | **Feito** |
| Domínio `DeterministicCapture` no `HistoryDomain` | **Feito** |
| Contador de aquecimento por frame **renderizado** | **Feito** — `FFrameCapture` |
| PNG + manifesto | **Feito** — WIC 24bppBGR + JSON plano ao lado |
| Presets científico/gameplay | **Feito** |
| Calibração do N | **Feito** — N = 128, ver o sweep abaixo |

### Onde mora cada peça

| Peça | Arquivo |
|---|---|
| Domínio de reset | `HistoryDomain::DeterministicCapture` → `FRenderSettings::NotifyDeterministicCapture` |
| Máquina de estados, readback, PNG e manifesto | `Engine/{Include/Smile,Source}/Graphics/FrameCapture.*` |
| Preset, coleta de estado e os 3 call sites | `Renderer::UpdateFrameCapture` / `RecordPost` / `FinishFrameCapture` |
| Disparo pela UI | `Editor/…/CaptureBridge.*` + card no `SettingsWindow.qml` |
| Commit da build no manifesto | `cmake/StampVersion.cmake` → `SMILE_BUILD_COMMIT` |

O protocolo está fechado. O N default (128) saiu de medição — ver "Calibração do N" abaixo; o
slider continua existindo para quem quiser exercitar outro ponto.

### Uso

Configurações → Renderização → cards *Câmeras de referência* e *Captura determinística*.
"Capturar" num slot restaura a pose e dispara na mesma ação — em dois cliques haveria espaço para
capturar da pose errada, que é justamente o que os slots existem para impedir. "Capturar aqui"
usa a câmera livre e grava `bookmarkSlot: -1`.

Saída em `<exe>/Captures/`, com nome derivado do estado real:
`<cena>_slot0_sci_N128_gi-ddgi-rgi-rcUQ_20260812-143355.{png,json}`.

O PNG é o backbuffer **depois do tonemap e antes dos overlays do editor** — contorno de seleção e
gizmos são a ferramenta, não a imagem.

## Ordem de uma captura

Fixa, e a ordem importa em cada passo:

1. **Aplicar preset** — antes de tudo, porque preset muda resolução e upscaler, e isso realoca alvos.
2. **Restaurar câmera** do slot.
3. **Reset determinístico** — domínio `DeterministicCapture`, mais forte que o corte de câmera.
4. **Zerar `TemporalSampleIndex`**.
5. **Aquecer N frames renderizados** — não ticks da UI: um frame que não foi renderizado não
   convergiu nada, e contar ticks tornaria o N dependente da carga da máquina.
6. **Capturar o frame seguinte.**
7. **Gravar PNG + manifesto.**

## O que a sessão fixa além do reset

O reset fixa o estado **inicial dos acumuladores**. Isso não basta: centenas de frames depois, a
captura sai de um estado que o reset não controla. Enquanto a sessão corre:

- **Câmera travada** — `UpdateCamera` ignora input e `SetCameraPose` recusa teleporte (com aviso no
  log). A mesma pose alcançada por trajetórias diferentes aquece o cache de mundo com células
  diferentes, e um duplo-clique no outliner é acidental por natureza. Recusar é melhor que
  cancelar: cancelar jogaria fora minutos de aquecimento.
- **Fase de animação canônica** — `ElapsedTime` vai para um valor fixo (`kCaptureElapsedSeconds`) e
  não avança; o valor real volta no fim. Nuvem, oceano, vento e ondulação saem dessa fase.
  *Congelar onde estava* não resolveria: fixaria uma fase **arbitrária**, a de quando o operador
  clicou, e duas capturas do mesmo bookmark disparadas com minutos de diferença sairiam com nuvens
  em posições diferentes. O zero é canônico por ser fixo, não por ser especial; o salto que ele
  provoca é inerte porque o reset logo em seguida derruba todo histórico que reprojetaria o estado
  velho.
- **Hora do dia declarada, não congelada** — a hora é a iluminação **autorada**, não um contador
  sem significado como o `ElapsedTime`; canonicalizá-la num valor fixo destruiria a capacidade de
  capturar uma cena de fim de tarde. Mas congelá-la onde estava também não serve: com o Time of Day
  correndo, duas capturas disparadas com minutos de diferença têm sóis diferentes, e um manifesto
  registrando as duas horas não torna as imagens comparáveis.

  Então ela vira **parâmetro da captura**, como a pose: `FCaptureRequest::PinTimeOfDayHours`, com um
  campo na UI (“Hora do dia fixada”, semeado uma vez com o relógio do mundo e depois estável). Duas
  capturas com a mesma hora declarada têm o mesmo sol por construção.

  Durante a sessão, hora e direção do sol são **reafirmadas a cada frame**, não apenas impedidas de
  avançar: o painel de TOD escreve direto na referência de `GetTimeOfDay()`, sem passar por setter,
  então não existe funil onde detectar a edição. Reescrever a cada frame faz “o sol não se move
  durante uma sessão” valer por construção, venha a escrita de onde vier — mesma política da câmera
  travada. No fim, o relógio do operador volta.
- **Molhadura no valor assentado** — `SettledWetness()`, o valor que a chuva atual alcançaria com
  tempo infinito. Aqui o passo fixo sozinho **não** basta: o τ é de 5 a 30 s e 128 frames cobrem
  2,1 s, então duas capturas só partiriam do mesmo ponto por coincidência. Partindo do assentado, a
  mesma chuva dá a mesma molhadura sempre.

O que **não** para: processos que *assentam* rápido (fade das sombras spot) continuam, com passo
fixo de `1/60 s`. O passo fixo também é o que o upscaler recebe em `DeltaTimeSec` — zero ali seria
um insumo que nenhum frame normal produz.

## O que cancela uma sessão

O contrato é "N frames consecutivos após **um** reset". Três classes de evento o quebram sem tocar
no capturador, e nenhuma pode ser recusada — o operador tem o direito de redimensionar a janela:

| Evento | Onde |
|---|---|
| Resize da janela | `Renderer::Resize` |
| Render scale / modo de qualidade do upscaler | `Renderer::ApplyRenderScale` |
| Carga de cena (inclusive aditiva e por linha de comando) | `Renderer::CommitCookedScene` |
| Qualquer knob que derrube acumulador | `FRenderSettings::Invalidate` |

Os dois primeiros existem porque **recriar recurso zera histórico por construção**, sem passar por
`Invalidate` — não há invalidação no funil a que se pendurar. `ApplyRenderScale` é o ponto onde os
dois caminhos que recriam alvos se encontram: o slider de render scale e, via `ApplyUpscalerScale`,
a troca de modo de qualidade do upscaler.

O terceiro é um gate no **funil**: todo knob que invalida história passa por `Invalidate`, então um
teste ali cobre os ~40 setters de uma vez, em vez de um gate por setter que o próximo knob
esqueceria. O próprio capturador passa por lá — no preset, no reset e na restauração —, e por isso
tudo que ele faz roda sob `CaptureSetupGuard`; sem essa exceção a sessão se cancelaria no ato de
começar, e a restauração descartaria um pedido novo enfileirado no mesmo frame.

O cancelamento alcança também o pedido **ainda pendente**. A janela entre o `Request` e o primeiro
frame é curta mas real, e uma cena trocada dentro dela faria a sessão começar na cena nova
carregando nome e bookmark da antiga.

O funil **não** é completo, e os limites ficam registrados: `TemporalMotion` e `NrdDirect` têm
chamadas diretas de `InvalidateHistory` no `Renderer` que não passam por ele.

> **Achado da revisão:** o oceano era o terceiro caso. Os setters de espectro do `FOceanFFT`
> (vento, ondas, swell, fetch, profundidade) derrubam o histórico temporal por conta própria —
> invariante interna da classe —, e por isso ninguém de fora ficava sabendo, inclusive uma captura
> em aquecimento. Agora os setters da fachada **declaram** o reset (`Invalidate(OceanTemporal)`) em
> vez de repeti-lo à mão; o `Renderer::SetUseWater` perdeu o laço que tinha.

**Se o operador mexer num knob durante a sessão**, o cancelamento não pode desfazer a escolha dele:
a restauração compara, campo a campo, o valor atual com o que o **preset** aplicou, e só devolve o
antigo onde os dois ainda batem. Sem isso, trocar o upscaler no meio de uma captura científica
cancelava a sessão e, no frame seguinte, o upscaler voltava sozinho para o valor anterior.

Cancelar é em voz alta (log + barra de status): uma captura sub-aquecida em silêncio é pior que
captura nenhuma, porque entra no A/B parecendo válida. Vale inclusive para o pedido cancelado
**antes de começar** — a UI já escreveu "aquecendo N frames" no clique, e sumir só com uma linha de
log deixaria esse texto na tela para sempre. Todo pedido aceito termina em sucesso ou falha
visível.

## Por que o reset é mais forte que o corte de câmera

`HistoryDomain::CameraCut` deliberadamente **não** derruba os caches de mundo — `DDGIAtlas`,
`ReGIR`, `RadianceCache`. O comentário no `HistoryDomain.h` explica: uma sonda irradia o mesmo
independentemente de onde a câmera esteja, e derrubá-las faria a GI reconvergir do zero a cada
duplo-clique no outliner.

Para captura isso se inverte. O estado inicial tem de ser **conhecido**, e não "o que sobrou do
trajeto até aqui": o radiance cache chegaria com células de onde a câmera passou, e o atlas do DDGI
com a convergência do caminho percorrido. Duas capturas da mesma pose dariam resultados diferentes
conforme o caminho até ela.

Daí um domínio próprio, nomeado pelo motivo — que é a política do arquivo.

> **Achado da revisão:** "todo acumulador" era uma afirmação falsa quando o domínio nasceu. O
> `kAllHistoryTargets` cobre o universo do *enum*, e dois acumuladores reais nunca tinham sido
> cadastrados nele: o temporal do `FSunShafts` e o histórico de displacement/foam da FFT do oceano.
> O único reset de cada um era interno (o passe adormecendo; o toggle da água). Ambos ganharam bit.
> O `static_assert` do arquivo protege o enum de crescer sem o contador acompanhar, mas **não**
> detecta um acumulador que nunca entrou — para esse a única defesa é a pergunta em toda revisão de
> passe novo: *isto sobrevive ao frame? então tem um bit no `EHistoryTarget`*.
>
> O `SunShafts` entrou também no `CameraCut` (temporal de tela, reprojeta por `PrevVP` — mesmo
> argumento do fog volumétrico, que já estava lá); o oceano **não**, porque é simulação de mundo e
> a onda é a mesma independentemente de onde a câmera está.

## Primeira rodada (smoke test)

Duas capturas consecutivas do mesmo slot, preset científico, N = 128, TOD e radiance cache
desligados. O protocolo se sustentou: `temporalSampleIndex == 128` nas duas, pose/FOV/resolução e
modos de render idênticos, PSNR de 66,4 dB, 99,99% dos pixels dentro de 1 nível e 169 pixels de
1.264.692 acima disso — ruído residual do estimador cru.

**E a régua achou um bug na primeira vez que foi usada**, que é o que ela existe para fazer. O
`SunDir` nascia com o literal autorado `(0,3 0,6 0,5)`, **não normalizado**; como toda escrita passa
pelo `SetSunDirection`, que normaliza, a restauração de estado no fim da primeira sessão convertia o
membro. A captura A gravava o vetor cru e a B o mesmo vetor unitário. A imagem quase não mudava —
quem consome já normaliza —, mas o manifesto divergia entre duas capturas idênticas, que é
exatamente o que ele não pode fazer. O membro passou a nascer unitário
(`Renderer::DefaultSunDirection`), e a invariante ficou declarada onde ele é.

Estas duas capturas **não são baseline**: a build era `-dirty`, o cache estava desligado (a
telemetria dele não foi exercitada) e o ReGIR não rodou por falta das condições efetivas. Servem
como prova de que o caminho de GPU funciona.

## A instrumentação do cache não é um observador neutro

Segunda rodada, com o radiance cache ativo e a instrumentação ligada. A telemetria se repetiu
**exatamente** entre duas capturas — 1.291.572 consultas, 1.004.003 hits (77,735%), 73.195 células
ocupadas, 2.200.350 amostras — com PSNR de 59,3 dB entre os PNGs.

Mas o mesmo par, medido contra o regime **sem** instrumentação, denunciou o que nenhum dos dois
regimes mostraria sozinho:

| | células | amostras |
|---|---|---|
| sem instrumentação | 73.218 | 2.200.003 |
| com instrumentação | 73.195 | 2.200.350 |

PSNR de 48,2 dB entre os regimes. A causa é o custo dos atômicos disputados por wave: eles mudam o
escalonamento e, com ele, **quais threads vencem as inserções** do cache. O efeito é minúsculo
(0,03% das células) e não é erro de ninguém — é o observador alterando o observado.

A consequência é de protocolo, não de código: a instrumentação é **parte da configuração**, não uma
janela para dentro dela. Por isso ela entra no manifesto (`cacheStatsEnabled`, efetivo como os
demais) e na etiqueta do arquivo (`rcUQS`), e alterná-la durante o aquecimento **cancela** a
captura — meio warm-up em cada regime não pertence a nenhuma das duas séries.

Ela **não** invalida o histórico do cache ao ser ligada: o conteúdo acumulado continua valendo, e
derrubá-lo tornaria a instrumentação inútil justamente para quem quer olhar um cache quente.

### São TRÊS regimes, e não dois (Fase 4)

A Fase 4 acrescentou o **detalhe** — misses por motivo, saúde da inserção e telemetria do produtor.
Ele não entrou junto do `S` justamente por causa da medida acima: a contagem de atômicos do regime
instrumentado é **congelada**, senão toda a série já tirada nele deixa de ser comparável com as
próximas. Então:

| regime | etiqueta | atômicos por wave | o que mede |
|---|---|---|---|
| limpo | — | 0 | imagem |
| instrumentado | `S` | 2, na consulta | ocupação, hit rate |
| detalhado | `Sd` | + até 6 na consulta, 4 na inserção, 7 no produtor | por que erra, e o que o produtor faz |

O terceiro mexe mais que o segundo, e num lugar novo: os atômicos de inserção ficam **dentro do
produtor**, onde decidem quem vence o CAS. Manifesto: `cacheStatsDetail`. Alterná-lo cancela a
captura, como o `S`.

### Regra de operação

- **Calibrar o N**: instrumentação ligada no sweep inteiro. O hit rate é um dos quatro sinais, e
  todos os pontos precisam do mesmo regime para a curva significar alguma coisa.
- **Comparar imagem**: instrumentação desligada, nos dois lados do A/B.
- **Diagnosticar o cache** (por que o hit rate é esse, o hash está saudável, o produtor faz o que
  se pediu): `Sd`. Série própria — não comparar com pontos tirados em `S`.

Nunca cruzar os três — e o nome do arquivo agora impede fazê-lo por distração.

## A sessão desliga o aquecimento automático do cache

Desde a Fase 4 o radiance cache tem estado próprio (`Resetting` → `Filling` → `Active`) e, em
produção, **a consulta de render só abre quando a tabela vale a pena** — e a borda `Filling →
Active` **derruba o histórico dos consumidores**, porque o terminador do raio secundário acabou de
mudar de fallback para cache.

Essa borda é incompatível com uma sessão de captura, e não por pouco. O passo 3 do protocolo zera
o cache; ele então enche durante o aquecimento e a transição cai por volta do frame 30 de 128 —
**dentro da janela de contagem**, derrubando acumulador no meio dela. É exatamente o que o funil de
invalidação existe para impedir, e o funil de fato cancelaria a sessão. Em toda captura.

Por isso a sessão roda com `AutoWarmup` **desligado**, nos dois presets, e devolve no fim. A regra
é a mesma do relógio do mundo, que o preset *gameplay* também canonicaliza apesar de não mexer em
knob nenhum: não é preferência de qualidade, é evento agendado dentro da janela de aquecimento.

O que isso significa para a imagem capturada: a consulta segue o toggle do operador do primeiro ao
último frame, e nos primeiros ela erra numa tabela fria. Inofensivo — o piso de confiança já impede
célula fria de encerrar caminho, e ninguém olha um frame de aquecimento. O manifesto grava
`cacheAutoWarmup: false`, então a captura declara o regime em que foi tirada.

⚠️ **Consequência para quem lê os números:** as capturas medem o cache em regime permanente, não a
transição. O custo do `Filling` (quantos frames o render passa sem cache depois de uma troca de
cena) **não é observável pela régua** — é medida de sessão viva, no painel.

## Calibração do N

> **Calibrado: N = 128.** É o default do capturador e da UI. O número deixou de ser palpite.

O N é **fixo para todo o A/B**, escolhido uma vez por calibração. Parada adaptativa por captura
daria N diferente entre configurações, e a diferença de N viraria viés: a configuração que converge
mais devagar seria medida com mais tempo de acumulação, que é precisamente a variável em teste.

### O sweep

Instrumentação ligada em todos os pontos (ver a regra de operação acima); dois disparos por N, para
que a coluna de repetibilidade signifique alguma coisa.

| N | ocupação | amostras/célula | repetibilidade (PSNR A×B) | hit rate |
|---|---|---|---|---|
| 32 | 70.005 | 18,31 | 50,62 dB | 77,85% |
| 64 | 73.099 | 22,98 | 54,38 dB | 73,76% |
| **128** | **73.195** | **30,06** | **59,29 dB** | 77,73% |
| 256 | 72.987 | 38,86 | 59,32 dB | 73,73% |

A **ocupação** termina em N=64 — de 64 para 256 ela nem cresce, oscila em torno de 73 mil células.
Se fosse o único sinal, o N teria sido escolhido em 64.

A **repetibilidade** conta outra história e é ela que decide: 50,6 → 54,4 → 59,3 dB de 32 a 128, e
então **59,29 → 59,32 dB** de 128 para 256. Três centésimos de dB pelo dobro da espera. É o platô, e
ele está em 128.

Isto é exatamente o que o parágrafo anterior deste arquivo previa: a tabela enche antes de as médias
assentarem, e parar pela ocupação daria um N cedo demais. Ficou medido em vez de argumentado — e a
distância entre 64 (ocupação pronta) e 128 (imagem estável) é o tamanho do erro que se teria
cometido.

O teto de 512 não precisou ser exercitado.

### Duas coisas que o sweep ensinou sobre a própria régua

**Hit rate instantâneo não serve para escolher N.** Ele oscila entre ~73,7% e ~77,8% sem tendência,
e a causa está no shader: uma célula só conta como hit enquanto `age < threshold`, e o limiar é
**escalonado por célula** — `RC_REFRESH_FRAME_MAX + (checksum & 7)`, ou seja, uma janela de 8 a 15
frames (`RadianceCache.hlsli`). O escalonamento existe para o refresh não virar pico periódico. A
consequência para a medição é que uma captura de **um frame** pega cada célula numa fase arbitrária
do ciclo de manutenção dela, e o número resultante diz mais sobre a fase que sobre a convergência.

Some-se a isso o que o próprio shader documenta: `RC_STAT_HITS` conta o **retorno antecipado** (o
custo economizado), não a cobertura da tabela. Cobertura seria `hasData`, e hoje não é reportada.

Duas saídas, quando isso importar — a segunda é melhor:

1. média de 16–32 frames em vez de amostra de um frame, o que embute o ciclo inteiro;
2. reportar `hasData` como contador próprio: cobertura não depende da fase do refresh, então
   mediria convergência sem precisar de média.

**Imagens de N diferentes não se comparam pixel a pixel.** Cada captura sai com `TSI = N`, portanto
com semente temporal diferente — o ruído é outro por construção. O dado válido do sweep é a
repetibilidade **dentro** do mesmo N, que é a coluna acima. Comparar 128 contra 256 pixel a pixel
mediria a semente.

## `TemporalSampleIndex`

`FrameIndex` global continua **monotônico** — frame slots, fences, lifetime e contadores absolutos
dependem disso. Um `TemporalSampleIndex` separado reinicia em zero no começo da captura, e só
jitter e RNGs o consomem. Fora do modo de captura o comportamento é idêntico (os dois avançam
juntos), então nada muda no uso normal.

Isso existe porque o jitter do upscaler é o maior contribuinte de diferença entre duas rodadas.
Deixá-lo fora da reprodutibilidade tornaria o A/B ruidoso justamente no eixo mais sensível; resetar
o `FrameIndex` global para consertar isso quebraria coisas que não têm nada a ver com amostragem.

### Auditoria dos usos de `FrameIndex` em `Renderer.cpp`

Sem números de linha de propósito — eles envelhecem a cada edição e um `grep TemporalSampleIndex`
acha os 15 sítios em um segundo. O que precisa durar é o **critério**.

**Migraram para `TemporalSampleIndex`** (15 sítios) — todos semente de sequência temporal: jitter
do upscaler (FSR/DLSS), jitter do TAA (Halton `% 8`), froxel do volumetric fog, ruído das nuvens,
`ShaftNoiseFrame` (`% 64`), ReGIR (sorteio de luzes da célula), DDGI (rotação das direções de
raio), reflexões, ReSTIR GI, ReSTIR DI, `Nrd.SetFrame` das duas instâncias RELAX,
`ShadowNoiseFrame` (`% 64`), AO e o passe de debug do DDGI.

O NRD não é caso especial: `cs.accumulationMode` já vira `CLEAR_AND_RESTART` quando `NeedsClear`,
que é o que o reset determinístico dispara — o índice reiniciado chega junto com a acumulação
reiniciada.

**Continuaram no `FrameIndex` absoluto:** `CommandQueue.FrameIndex()`, que apesar do nome é o
**frame slot** (0/1) e não tem relação; e `Time.z` do cbuffer.

### Posição do incremento

Os dois contadores avançam **depois do `CommandQueue.EndFrame`**, e a posição é o que faz o
contrato de warm-up valer.

O `++` vivia logo depois do `ResolveFrameView`. Como aquele já tinha consumido o índice para o
jitter, todo o resto do frame rodava com o valor **seguinte** — um frame amostrando em duas
sementes. Passava despercebido enquanto era só o `FrameIndex`, porque ninguém comparava os dois
grupos; deixa de passar aqui, porque após um reset o aquecimento usaria `0…N−1` no jitter e `1…N`
em todo o resto, e o frame capturado misturaria `N` com `N+1`. O contrato seria falso por
construção e a captura "determinística" dependeria de qual metade do frame se olhasse.

Com o avanço no fim, todo consumidor do frame lê o mesmo valor e o `0…N−1 / captura em N` sai da
definição.

> ⚠️ **Isto deslocou algumas sementes em −1** em relação ao comportamento anterior. É mudança de
> ruído, não de energia, e foi paga de propósito antes de existirem capturas de referência
> gravadas com a semântica errada. Um A/B contra builds anteriores a este commit não é bit a bit.

**Achado à parte:** `Time.z` do cbuffer carrega o `FrameIndex` e **nenhum shader o lê** — só
`Time.w` (gate do AODebug). Payload morto, removível num commit de limpeza; sem relação com a
captura.

## Presets

**Científico** — resolução nativa, sem upscaler. Para equivalência do estimador: elimina a maior
fonte de diferença entre rodadas e mede o sinal, não o reconstrutor.

> ⚠️ **Desligar o upscaler não basta.** `M.TAAActive = UseTAA && !M.UpscaleActive && ...`
> (`Renderer.cpp`): tirar o upscaler faz o `!M.UpscaleActive` virar verdadeiro e **ativa o TAA**,
> trocando o jitter do FSR pelo Halton em vez de eliminá-lo. O preset tem de zerar `UseTAA`
> também. Com os dois desligados o `JitterPx` fica no default 0 e `Projection == ProjUnjittered`
> — sem resíduo. (Hoje o `UseTAA` já nasce desligado em favor do FSR, mas isso é um default, não
> uma garantia do preset.)

> **Consequência assumida:** o denoiser é eixo do operador, não do preset — capturas cruas e
> denoisadas são as duas necessárias. Mas com **DLSS RR** os dois eixos são um só (o RR faz denoise
> *e* upscale num eval), então tirar o upscaler derruba o RR para NRD. O preset não esconde isso: o
> manifesto grava o estado **efetivo**, e é ele que vale na comparação.

**Gameplay** — upscaler e denoiser reais. Para validar o que o jogador vê, que é o único resultado
que importa no fim. Não muta nada: captura a engine como o operador a configurou.

Os dois são necessários e medem coisas diferentes. Uma regressão que só aparece no gameplay é uma
interação com o upscaler; uma que só aparece no científico é do estimador e o upscaler está
mascarando.

## Manifesto

Ao lado do PNG, com nome derivado do estado real da engine e não digitado à mão — erro humano na
terceira rodada é o que o PNG automático existe para eliminar. JSON plano, uma chave por linha,
mesma forma dos sidecars que o `SceneLoader` já lê, e legível num diff.

Cena, slot, preset, N e commit da build; resolução de saída e de render, render scale, upscaler +
qualidade, denoiser e TAA; os toggles do A/B (`useGI`, `ddgiReady`, `restirGI`, `restirDI`,
`regir`, `reflections`, `cacheUpdate`, `cacheQuery`, `giTerminatorOff`); ocupação do radiance cache
no instante do disparo; pose e FOV da câmera; direção do sol e hora do TOD.

Os toggles são os **efetivos**, e cada um vem de onde a decisão realmente é tomada — recompor a
condição por fora é o começo de uma divergência:

| Campo | Fonte |
|---|---|
| `restirGI`, `restirDI`, `reflections`, `taa` | `FFrameModes` do frame capturado |
| `regir` | o gate real, que também exige consumidor e `GILightCount > 0` |
| `cacheUpdate`, `cacheQuery` | o que o `ShaderParams` publicou **a um consumidor que rodou** |
| `cacheWarmup` | o estado do aquecimento global, que diz **por que** `cacheQuery` está falso |

O caso do cache mostra por que a distinção importa, em dois eixos. `ResetPending` é limpo pelo
resolve no meio do frame, então recompor `Enabled && Ready && !ResetPending` no fim daria "ativo"
para um frame que entregou flags **zeradas** aos traces — que é exatamente a captura com `N = 0`. E
os params são montados para os três consumidores (DDGI, ReSTIR GI, reflexões) mesmo quando o passe
não vai rodar, então o registro só conta quem de fato traçou; sem isso o manifesto afirmaria cache
ativo num frame em que ninguém o consultou.

As métricas do cache saem quando ele **atualizou ou consultou** — não só quando atualizou. A
configuração só-leitura (reflexões consultando com DDGI e ReSTIR GI desligados) é legítima no A/B, e
é justamente ali que ocupação e hit rate interessam.

Elas são as do frame disparado, e vêm de **duas** cópias porque as duas metades
ficam prontas em momentos opostos: `cacheQueries`/`cacheHits` são escritos pelos traces e zerados
pelo resolve (só a cópia pré-resolve os pega), enquanto `cacheOccupied`/`cacheValid`/`cacheSamples`
são escritos pela varredura *dentro* do resolve (só uma cópia pós-resolve os pega). Um frame de
defasagem é invisível no painel — por isso o caminho normal tem uma cópia só —, mas ocupação em
função do N é precisamente o que a calibração vai medir. Queries/hits ficam em zero sem a
instrumentação do cache ligada.

> **Limite conhecido de `cacheQueries`/`cacheHits`:** o trace tardio de reflexões roda **depois** do
> resolve, então as consultas dele caem no frame seguinte. A medida é, a rigor, "queries dos passes
> anteriores ao resolve deste frame + reflexões do frame anterior". Para a calibração isso não
> atrapalha — o viés é idêntico entre capturas da mesma configuração, que é como os números são
> usados —, mas não leia o campo como "todas as consultas deste frame".

Se o manifesto não puder ser gravado, a captura **falha e os dois arquivos são descartados**. Uma
imagem sozinha não é comparável — não se sabe de que configuração, de que N nem de que build ela
saiu — e comparável é a única coisa que ela deveria ser.

Os dois são escritos em `.tmp` e renomeados no fim, **manifesto primeiro, PNG por último**. Os dois
renames não são atômicos entre si, então a ordem escolhe qual é o único estado intermediário
possível: um manifesto sem imagem, que é obviamente incompleto, em vez de um PNG órfão, que parece
uma captura boa cujo manifesto alguém apagou. Falha no segundo rename desfaz o primeiro.

Quatro campos que parecem redundantes e não são:

- **`pinnedTimeOfDayRequested`** × **`pinnedTimeOfDayApplied`** × `timeOfDayHours`: o pedido, o que
  de fato foi fixado, e o que o mundo tinha. Os dois primeiros divergem quando o Time of Day está
  **desligado**: ali o sol é autorado à mão e não deriva da hora, então fixar a hora não faz nada, e
  gravar o pedido como se tivesse valido faria o manifesto afirmar um controle que não houve — duas
  capturas com o mesmo pin pedido poderiam ter sóis completamente diferentes. Negativo no aplicado =
  a sessão não fixou hora nenhuma. `timeOfDayEnabled` acompanha para o leitor saber por quê.

- **`temporalSampleIndex`** tem de ser **igual ao N**. É a prova, no arquivo, de que o contrato
  "aquece `0…N−1`, captura em `N`" valeu naquela rodada. Se divergir, o aquecimento foi
  interrompido (resize, troca de cena, um frame que morreu) e a captura não é comparável.
- **`ddgiReady`** é a *existência* do volume, não o `useGI` — é por existência que o fallback é
  escolhido (commit `a67eadd`), e é isso que precisa ficar registrado.
- **`cacheWarmup`** × `cacheQuery`: o segundo diz que a consulta estava fechada, o primeiro diz se
  foi o operador (`active` com a leitura desligada), um reset em curso (`resetting`, que acaba no
  frame seguinte) ou o aquecimento (`filling`, que leva dezenas). Numa captura o terceiro caso não
  deveria aparecer — a sessão desliga o aquecimento automático (ver a seção acima) —, e se
  aparecer, é defeito e não configuração.

`build` sai do `SMILE_BUILD_COMMIT`, carimbado **a cada build** e não no configure: um campo cuja
única função é ser confiável não pode apontar para o commit anterior depois de um rebuild. Sufixo
`-dirty` quando a árvore tinha alterações não commitadas — uma captura tirada no meio de uma edição
não pode se passar por uma tirada do commit limpo. **Arquivo não rastreado também conta**: um
`.hlsli` novo incluído por um shader existente entra na compilação sem produzir uma linha de diff
rastreada, e marcar limpo nesse estado poria no manifesto um commit em que a imagem não se
reproduz. O preço é um `-dirty` a mais quando há rascunho na árvore, e esse é o lado certo de errar.

## Futuro

Captura HDR pré-tonemap, para quando as diferenças do estimador ficarem pequenas demais para
sobreviver ao tonemap e à quantização de 8 bits do PNG.
