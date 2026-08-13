# SHaRC/WRC como GI primário, DDGI como fallback

> **Onde estamos:** Fases 0-3 fechadas e medidas. **Fase 4 com o código todo entregue e a medida
> principal feita** (11 commits): piso de confiança aprovado por A/B, capacidade e limiares de
> aquecimento definidos por medida, aquecimento global e visualizador no lugar, e o lifecycle
> auditado até a última porta lateral. Faltam **três gates de RUNTIME**, nenhum de código: a
> captura em 2¹⁷, o teste vivo de luz/ToD e o smoke de hot reload. O bloco `➜ FASE 4` mais abaixo
> tem os números, o que eles decidiram, e a fila do que resta.
>
> Leia este bloco ESTADO inteiro antes de continuar; ele existe para não re-derivar decisão. A
> seção "Estado de PARTIDA", lá embaixo, é retrato histórico e **não** descreve o código de hoje.

## ESTADO — 2026-08-13

**A régua está pronta e calibrada.** O `Docs/CAPTURE-PROTOCOL.md` é a referência; aqui fica só o
que muda o plano.

Capturador completo: domínio `DeterministicCapture` (o universo dos alvos, inclusive os caches de
mundo), `FFrameCapture` com aquecimento em frames **renderizados**, PNG pós-tonemap/pré-overlays,
manifesto derivado do estado **efetivo**, presets científico/gameplay, disparo pela UI ao lado dos
bookmarks, e o commit da build carimbado a cada *build*. Commits `49b2e88`, `1a16ba7`, `4ef2cf4`,
`3872f97`.

**N = 128, calibrado** por sweep de 32/64/128/256. A ocupação do cache termina em 64, mas a
repetibilidade da imagem só estabiliza em 128 (50,6 → 54,4 → 59,3 dB), e 256 rende 0,03 dB pelo
dobro da espera. Escolher pela ocupação teria dado metade do necessário — que é precisamente o erro
que o plano previa ao exigir o delta do sinal final na lista de sinais.

Três achados que a régua produziu antes de medir qualquer estimador, e que valem para o resto da
série:

- `SunDir` nascia **não normalizado**, e a restauração de estado o convertia: o manifesto divergia
  entre duas capturas idênticas. Corrigido na origem.
- Dois acumuladores reais (`SunShafts`, FFT do oceano) nunca tinham entrado no `EHistoryTarget`, e
  "reset de tudo" era falso.
- A **instrumentação do cache não é neutra**: os atômicos mudam o escalonamento das waves e, com
  ele, quais threads vencem as inserções (73.218 × 73.195 células, 48 dB entre os regimes). Ela é
  parte da configuração — entra no manifesto e no nome do arquivo, e alterná-la cancela a captura.

**Fase 0 fechada**: as quatro baselines foram tiradas no commit `ed7b543` limpo, slot 0, TOD 10:00
fixada, N = 128, instrumentação desligada. Elas são o registro do "antes" e **devem envelhecer** —
é para isso que existem. Controles novos podem ser tirados a qualquer momento, mas não as
substituem.

**Fase 2 fechada** — commit `e70ade7` (#4 da lista), validado contra as baselines. `ShadeSurfaceHit`
decomposto em `PathTracingCommon.hlsli`, `FRCQueryResult` com status de miss, política isolada no
wrapper. **Uma parte foi explicitamente adiada**: throughput/PDF/lobo do `FPathState` e a
amostragem de BSDF nascem na Fase 3 junto com o produtor delas — ver o bloco na Fase 2, que marca
item a item. Não ler como "Fase 2 inteira concluída".

Evidência de que o refactor não muda o estimador, antes mesmo do A/B visual: o DXIL de Release
(`-O3`, sem debug info) dos sete shaders afetados tem **contagens idênticas** de `traceRayInline`,
`rawBufferLoad`, `sample`, atômicos e de toda a aritmética. A única diferença é `−1 phi / +1 select
/ −1 zext / −1 and` por call site — um booleano reencodado, porque `cacheN` e `hitN` eram duas
expressões iguais em lados opostos do retorno antecipado e agora são um valor só. **Esta técnica
vale para toda a série**: em refactor que promete não mudar imagem, comparar o histograma de
`dx.op` do DXIL de Release custa minutos e responde antes de acender a GPU.

---

### Fase 3 — fechada em transporte e estabilidade; default V1 por medida

**Medido na Bistro exterior, 3060 Ti, preset gameplay com FSR.** O par V1×V4 é comparável porque o
resto do frame ficou igual; os absolutos pertencem a essa configuração.

| | V1 | V4 |
|---|---|---|
| updater | 3,75 ms | 7,73 ms (2,06×) |
| frame | 7,45 ms | 11,36 ms |
| resolve | 0,06 ms | 0,06 ms |
| células (N=256) | 25.165 | 50.718 |
| amostras/célula | 39,00 | 38,65 |

**O default passou para `UpdateMaxVertices = 1`.** Os 3,98 ms extras do V4 compram +0,45% de
luminância média e 44,79 dB entre as duas imagens (88,4% dos pixels dentro de 1 nível, 0,56% acima
de 10). É ganho real e pequeno demais para dobrar o custo do passe nesta GPU.

**O número que explica o porquê, e que decide o próximo passo:** V4 acumula **exatamente 2,00×** as
amostras de V1. Como V1 grava 1 vértice por caminho por construção, **V4 grava em média dois**, de
quatro possíveis — os vértices 3 e 4 quase não são alcançados (o caminho escapa para o céu ou cai na
elegibilidade). Cinco raios para gravar dois vértices. Por isso **V2 é a medida seguinte** e não um
palpite: ele corta exatamente na média. Não dá para afirmar que V2 = V4 — isso depende da cauda da
distribuição, não da média —, mas a média em 2,00 diz que a cauda é fina.

**O que o V4 compra é cobertura, não convergência.** Dobra as superfícies no cache (50,7k × 25,2k
células) mantendo as mesmas amostras por célula (38,65 × 39,00). Ele espalha o caminho, não adensa
a mesma célula.

⚠️ **A ressalva de escopo importa mais que o número.** A Bistro exterior é uma cena em que o caminho
escapa cedo — é *por isso* que a média dá 2,00. Em interior o caminho bate mais, a média sobe na
direção de 4, e V4 passa a custar mais **e entregar mais**. O default está calibrado para uma classe
de cena; reabrir com medida de interior antes de tratá-lo como geral.

**Limitação do instrumento, descoberta aqui.** O sweep entre N diferentes **não** mede PSNR de
convergência: cada N termina num `TemporalSampleIndex` diferente, logo num frame estocástico
diferente do ReSTIR GI, e é esse ruído que domina N64×N128×N256. Ocupação e energia continuam
válidas através do sweep (não dependem do frame sorteado) e é por elas que a estabilidade está
provada. Para uma curva **visual** por N seria preciso ou capturar com NRD ligado, ou pinar o índice
de amostragem do frame capturado — e a segunda opção quebra o contrato atual do manifesto ("o tsi
tem de bater com o N do aquecimento"), então seria mudança declarada do capturador, não ajuste.

**Async saiu de "talvez" para requisito** — mas não resgata o V4. Com sobreposição perfeita o piso
é `max(updater, resto)`: V1 iria a ~3,75 ms (esconde os 3,7 ms do resto), V4 continuaria em 7,73 ms.
E sobreposição perfeita não existe aqui: o RayQuery disputa SM e RT cores com os passes gráficos.
Fica como commit próprio, na Fase 7, com as dependências já conhecidas (G-buffer antes, resolve
depois, e a fila compute já ocupada pelo DDGI nessa mesma janela).

Fila do que vem: **Fase 4 (confiança e estados) → Fase 5 (SHaRC primário)**. Fora da linha crítica e
sem bloquear: medir **V2**, e **async** como commit próprio. Mesmo 3,75 ms do V1 ainda é caro num
frame de 7,45 — mas async e semântica de confiança ao mesmo tempo criariam duas classes de bug
simultâneas, e a semântica vem primeiro.

---

### ➜ FASE 4 — medida feita; o código todo entrou; falta a confirmação

Onze commits: `5bf1e48` (piso de confiança), `1c9035b` (miss e inserção), `bfb386c` (produtor),
`ed9f223` (separação de capacidade × contenção × teto × terminal), `318417b` (capacidade 2¹⁷),
`ca1f9a9` (aquecimento global), `f624d7b` (visualizador), `9e64d64` (invalidação na borda + o reset
que não pode ser perdido), `bdd383e` (o latch é a mudança de consulta; o tick sobe para o topo do
frame), `3e4ab07` (o reload de shader também fecha a consulta) e `19ac2ea` (botão de reset e reload
pelo funil). Todos compilam em Debug e Release, `ctest` 3/3.

**A medida do item 1 foi feita** — é a seção a seguir, e é dela que saíram os dois defaults novos.
O que resta para fechar a fase é uma captura de confirmação na capacidade nova e o gate dinâmico
(luz/ToD), que a régua determinística não cobre. Ver "O que falta" no fim do bloco.

**A auditoria da matemática do resolve (item 1) foi feita antes, lendo o código, e passa.** Em
`RadianceCacheResolve.cs.hlsl:95-101` a mistura é `(prevRadiance·prevSamples + newSum) / total` com
`samples = min(total, 64)`; como `prevSamples` vem de `Resolved`, que só foi escrito por esse mesmo
`min`, ele nunca passa de 64. A média corrente vira EMA de constante 1/64, que é o
`maxAccumulatedFrames` da SHaRC. **Correto — não mexer.**

#### O que entrou (não re-derivar)

**Piso de confiança.** O `RC_QueryInner` rejeitava só `sampleNum == 0`; uma célula de UMA amostra
encerrava um path como se estivesse convergida. Agora há `RC_QUERY_WARMING` e um piso vindo do CB
(`RadianceCacheLodCapFlags.w`, que estava livre — **nenhuma mudança de layout de cbuffer**, e é por
isso que o campo foi para lá). Default 4, faixa 1..16, e **`1` reproduz exatamente o regime
anterior** — é esse o outro braço do A/B. Quatro decisões dentro dele:

- Vale para os **dois lados**, consulta de render e terminal do updater, pelo mesmo argumento da
  política de backface. No terminal pesa mais: ali a amostra ruim não é consumida uma vez, é
  **gravada** na próxima célula da cadeia.
- Invalida a tabela (`ResetOnce` + `Dom::RayVisibility`), entra no manifesto (`cacheMinSamples`) e
  na etiqueta (`C<n>`). É a terceira vez que a regra "invalidação + manifesto + etiqueta" se aplica
  a um knob novo, e a primeira em que ela não foi esquecida.
- Os **seis estados de célula** do plano saem dos status da query sem buffer novo; o sexto
  (despejada) não é observável por construção — o resolve troca a chave por tombstone no mesmo
  passe, e a busca seguinte cai em `NO_ENTRY`.
- O visualizador desempacotava os params **a dedo**; passou a usar a macro
  (`RC_UNPACK_PARAMS_INTO`). Sem isso ele pintaria de verde exatamente a célula que a consulta
  recusa. O modo "cobertura" ganhou azul = aquecendo.

**Telemetria completa, num TERCEIRO REGIME.** `RC_FLAG_STATS_DETAIL` existe separado porque a
contagem de atômicos do `RC_FLAG_STATS` é **congelada**: ela já é não-neutra, e a série medida nela
só continua comparável enquanto esse número não muda. O detalhe se declara no manifesto
(`cacheStatsDetail`) e na etiqueta (`d` minúsculo, que qualifica o `S`), e cancela captura em curso.

Os cinco grupos têm **gates diferentes**, e é isso que mantém cada contador com uma população só:

| grupo | gate | população |
|---|---|---|
| tabela/resolve (5) | resolve | células da tabela depois do update |
| resumo da consulta (2) | `STATS` | raios de render que consultam o cache |
| miss por motivo (6) | `STATS` **e** `DETAIL` | raios de render — o updater não tem `STATS` |
| inserção (7) | só `DETAIL` | quem **escreve** na tabela — com produtor dedicado, só o updater |
| produtor (10) | só `DETAIL` | caminhos do updater |

Sondagens são contadas no `RC_Insert`, não no `RC_Find`: as duas percorrem a **mesma** cadeia, e
medir na inserção poupa atômicos no caminho quente (4% dos pixels contra todo raio secundário).
A métrica de cadeia usa somente a primeira varredura; disputas de CAS e novas varreduras ficam em
`retries`, para contenção não se disfarçar de tabela cheia. `PROBEMAX` e `PATH_DEPTH` usam
`InterlockedMax` e o resolve **precisa** zerá-los — senão o painel mostraria o pior caso da sessão
como se fosse o do frame.

Os 30 contadores vão para o manifesto além do painel: dois gates de saída são exatamente esses
números, e gate que só existe em painel volátil não é verificável depois.

**Correção de honestidade, de brinde:** o visualizador se registrava como consumidor
(`ConsumerRuns` default), então abrir a janela de debug bastava para o manifesto declarar
`cacheQuery: true` num frame sem nenhum trace de render consultando.

#### A MEDIDA — A/B do piso, 2026-08-13

Bistro exterior, slot 0, ToD 10:00 fixada, preset científico, N = 128, regime `Sd`, V1. Único knob
diferente entre as duas capturas: `cacheMinSamples` 1 → 4. Arquivos
`…rcUQSdC1D1V1R50T_20260813-182931` e `…rcUQSdC4D1V1R50T_20260813-182942`.

| | C1 | C4 |
|---|---|---|
| consultas | 1.291.552 | 1.291.552 |
| acerto | 68,198% | **67,936%** |
| miss aquecendo | 0 | 13.628 (1,055%) |
| miss refresh | 20.406 | 10.157 (0,786%) |
| células (ocupadas = com amostra) | 25.176 | 25.176 |
| confiáveis | 25.176 | **20.340 (80,79%)** |

**O piso custou 0,262 pp de acerto, e a conta fecha EXATAMENTE.** Dos 13.628 misses `aquecendo`,
10.249 eram células que já iam pedir refresh de qualquer jeito (é a queda de `refresh`, de 20.406
para 10.157: a query testa aquecendo antes de stale, então o motivo mudou de nome, não de
resultado). Sobram **3.379** — e 880.809 − 877.430 = **3.379**, o número exato de acertos perdidos.
O piso só custa a célula que estava *fresca* e abaixo dele; o resto ele tira de quem ia refrescar.
A mesma aritmética fecha do outro lado: o terminal do updater trocou 39 `cache` por 39 `miss`.

Isso é, de brinde, a validação do instrumento: 30 contadores de três populações diferentes fecham
entre si sem sobra.

**Imagem:** PSNR 53,35 dB entre C1 e C4, variação média praticamente nula. O piso não é uma
mudança visual — é uma mudança de *procedência* do valor. **`MinSamples = 4` aprovado como default.**

Os outros dois blocos que a mesma captura fechou:

- **Saúde do hash:** `insertFull` = 0, `contended` = 0, `retries` = 0, cadeia média **1,0026**
  (máx. 3) em 45.067 tentativas. Descartadas no teto de 64: 551 (1,223%) — updates aceitos 98,78%.
  ⚠️ Com 2,40% de ocupação **nada disso podia dar errado**: o gate de 0,1% não teria como falhar
  nem com um hash ruim. É por isso que a capacidade mudou (abaixo) e a medida precisa ser repetida.
- **Gate de saída da Fase 3, enfim medido:** 50.554 caminhos contra 50.588 esperados para 4% de
  1573×804 (99,93% — a diferença são pixels de céu, que retornam antes). A soma dos **sete**
  terminais bate exatamente com `cachePaths`. Terminal em **cache 51,63%**, céu 19,70%, miss 26,88%,
  lobo 1,79%, morto 0 (política de backface desligada). Mais da metade dos caminhos termina no
  próprio cache: **a realimentação temporal está viva**, e não é hipótese.

**O maior miss não é do cache, é da geometria:** `segmento curto` sozinho responde por **21,75%**
das consultas, e `sem chave` por 8,47%. Somados, 30% das consultas nunca vão ser atendidas por esta
tabela nesta configuração de célula. Isso é insumo direto da **Fase 5**: é o piso do que o fallback
vai continuar tendo de responder, e nenhum ajuste de capacidade ou de aquecimento o move.

**Ressalva registrada:** o manifesto anota `regir: false` com `regirRequested: true`, porque a cena
tem `giPunctualLightCount: 0`. Vale igual nas duas capturas, então não contamina o A/B.

#### O que a medida decidiu

**Capacidade: 2²⁰ → 2¹⁷** (`318417b`). A tabela terminou com 2,40% de ocupação. Não é
descuido: a estimativa de 2²⁰ foi feita quando o produtor eram os **hits do render** e todo raio
secundário inseria; o passe dedicado escreve por 4% dos pixels e derrubou a população em quase duas
ordens de grandeza.

2¹⁷ é o melhor **compromisso** contra a faixa de 20-70%, e não um encaixe — vale ser exato, porque
a primeira redação deste bloco dizia "a única potência em que os dois cabem" e isso é falso:

| | V1 | V4 | |
|---|---|---|---|
| 2¹⁶ | 38,4% | 77,4% | V4 estoura o teto por 7 pp |
| **2¹⁷** | **19,2%** | **38,7%** | V1 rente ao piso, por 0,8 pp |
| 2¹⁸ | 9,6% | 19,4% | os **dois** abaixo do piso |

**A tabela é a prova; a razão entre V1 e V4 só explica por que errar é possível.** Para os dois
caberem seria preciso V1 ∈ [20%, 35%] — o piso da faixa embaixo, e o teto dividido por dois em
cima. Essa janela é **1,75× larga** e potências de dois consecutivas andam **2×**, ou seja mais que
a janela: dá para pular por cima dela inteira. Nada garante que isso aconteça — em outra cena a
janela pegaria —, e nesta acontece, por pouco: 2¹⁷ cai 0,8 pp abaixo dela e 2¹⁶, 3,4 pp acima.

2¹⁷ é o único em que nenhum lado erra por mais de um ponto — e erra para o lado barato: ficar sob o
piso desperdiça memória, estourar o teto perde amostra. VRAM do cache: 36 MiB → 4,5 MiB. ⚠️ Medida de UMA pose estática; câmera andando e interior sobem a ocupação, e quem
denuncia é `insertFull`.

**Aquecimento global** (`ca1f9a9` + `9e64d64`) — o item estrutural que faltava. `Resetting` →
`Filling` → `Active`, com o knob `AutoWarmup` nascendo ligado e desligado reproduzindo exatamente o
regime anterior. Seis decisões que não se re-derivam:

- **`Filling` fecha a consulta do RENDER, não a do updater.** O terminal do produtor continua lendo
  o `Resolved`; é ele que enche o cache com multi-bounce em vez de só o primeiro bounce. Fechar os
  dois faria `Filling` produzir um conteúdo que `Active` teria de reconverter.
- **A borda `Filling → Active` derruba o histórico dos CONSUMIDORES.** Ela é, para eles, a mesma
  coisa que o operador abrir a leitura — o terminador do raio secundário muda —, e o toggle manual
  sempre invalidou. Sem isso, o que o ReSTIR GI e o atlas do DDGI (histerese 0,99: centenas de
  updates de memória) somaram durante o `Filling` continuaria a valer misturado com o cache. Era o
  único caminho da engine que trocava o terminador sem limpar quem somou o anterior. A máscara
  ("consumidores sim, tabela não") é função única, compartilhada pelos três eventos que são o mesmo.
- **O que dispara a invalidação é a mudança EFETIVA da consulta (`AutoWarmup && QueryEnabled`), e
  não a transição de estado.** A máquina anda sempre — inclusive com o automático desligado, para o
  estado estar certo quando o knob voltar —, e ali ela não estava fechando nada: invalidar seria
  derrubar histórico sem que nada tivesse mudado para consumidor nenhum. Numa sessão de captura
  seria pior: ela desliga o automático justamente para a borda não cancelá-la, e a borda a
  cancelaria assim mesmo.
- **O tick roda no TOPO do frame**, logo depois do `CollectStats` que o alimenta e antes de
  qualquer consumidor publicar cbuffer. A névoa volumétrica resolve o `UseHistory` dela ainda no
  `ResolveFrameLighting`: notificada do bloco de GI, ela já teria decidido reprojetar a história
  que o reset acabou de invalidar. A borda não pode depender de quem publica primeiro.
- **A consulta FECHANDO também é troca de terminador**, e o único caminho que a fecha sozinha é o
  reload de shader — ele reseta a tabela (a semântica da chave pode ter mudado) sem passar por
  setter nenhum. O latch cobre os dois sentidos.
- **Tudo que fecha ou abre a consulta passa pelo FUNIL.** Foi a última classe de defeito desta
  fase, e ela apareceu três vezes seguidas em lugares diferentes: a borda do aquecimento, o reload
  de shader e o botão de resetar a tabela (que chamava `ResetOnce` cru — fechava a consulta sem
  invalidar ninguém e sem cancelar captura). O `ReloadShaders` cancela captura **por conta
  própria**, e não de carona no latch do cache: trocar pipeline invalida a sessão porque *os
  shaders* mudaram, não porque um passe resetou algo — com `cacheQuery: false` a carona não
  chegava.
- **O estado sai do EVENTO, não de amostrar `ResetPending`.** `ResetOnce` é chamado de fora do
  frame — knob da UI, carga de cena — e portanto também *depois* do tick; nesse caso o resolve
  limparia o pending sem ninguém observar e o frame seguinte encontraria `Active` sobre uma tabela
  recém-zerada. O `SetupForScene` escrevia a flag a dedo pela mesma porta lateral.
- **O gate lê a varredura da tabela (`Confident`/`HasSamples`), não o hit rate.** O hit rate só
  existe com a instrumentação ligada, que é regime de MEDIÇÃO e não roda em produção — um gate
  preso nela ficaria em `Filling` para sempre no único modo em que ele importa.
- **Os primeiros frames de `Filling` ignoram o readback de propósito.** Ele está `kFramesInFlight`
  frames atrás e ainda descreve a tabela do regime anterior — cheia e confiável —, então sem a
  carência o gate pularia para `Active` no primeiro frame depois de um reset.
- Limiares: **70%** das células com amostra confiáveis (o regime permanente medido é 80,79%, e o
  gate fica abaixo porque essa fração tem um teto < 1 que depende do churn da cena), com teto de
  **96 frames** = 1,5× o `kStaleFrameMax`, ou seja mais de uma volta completa de despejo. **Tabela
  com conteúdo é pré-requisito dos dois caminhos**: o teto cobre a cena que enche devagar, não a
  que não enche — com o produtor parado ele recriaria, sem prazo para acabar, exatamente o custo de
  consultar tabela vazia que o estado existe para evitar.

**O capturador anda junto.** A borda passa pelo funil de invalidação, que cancela captura em curso;
como o reset determinístico zera o cache no início de toda sessão, ela cairia por volta do frame 30
de 128 **em toda captura**. A sessão passa a rodar com `AutoWarmup` desligado, nos dois presets,
como já acontece com o relógio do mundo — e é a condição do latch acima que faz esse desligamento
de fato proteger a sessão. Ver `Docs/CAPTURE-PROTOCOL.md`. Duas consequências a registrar:

- **O que a régua deixa de medir é a POLÍTICA, não a convergência fria.** A convergência fria é
  justamente o que o sweep de N mede — `N = 32`/`64` são o cache a meio caminho, `N = 0` é ele
  vazio. O que nenhuma captura observa é o *bloqueio automático* em `Filling`: quantos frames o
  render passa sem cache depois de uma troca de cena, e como a imagem salta quando a consulta abre.
- **`cacheWarmup: "filling"` numa captura é legítimo** — a máquina anda, o portão é que está aberto.

**Visualizador** (`f624d7b`): modos `Age` (rampa quebrada no limiar de refresh, que é por célula) e
`Confidence` (N contra o **piso**, saturando acima dele — é onde se vê a frente de aquecimento
andar). `Fallback source` continua fora até a Fase 5, quando existir escolha de fallback por hit.

#### O que falta para FECHAR a Fase 4

**Nada é de código — os três são gates de RUNTIME**, e nenhum deles tem como ser respondido lendo
o repositório.

1. **Captura de confirmação em 2¹⁷** — `Sd` / V1 / C4 / N=128, mesma pose. Verificar: ocupação na
   faixa, `insertFull` < 0,1%, contenção residual, cadeia saudável, imagem equivalente à de 2²⁰ e
   **o `AutoWarmup` de volta ligado no fim da sessão** (o manifesto sai com `cacheAutoWarmup:
   false` por construção; quem responde pela restauração é o painel, depois). É a primeira vez que
   o gate de capacidade tem como falhar: em 2²⁰, com 2,40% de ocupação, ele passaria mesmo com um
   hash ruim.
2. **Teste vivo de luz/ToD**, com o modo `Age` e a telemetria abertos: "a convergência responde a
   luz acesa/apagada e ToD sem congelar por dezenas de segundos". A régua determinística **não
   cobre isto** — ela é de pose e tempo fixos, e este gate é dinâmico por definição. O instrumento
   existe (miss `refresh`, `cacheEvicted`, o modo `Age`); o protocolo, não.
3. **Smoke de hot reload**, inclusive **durante** uma captura: os pipelines trocam, o cache reseta,
   os consumidores esquecem e a sessão cancela com a mensagem certa. São quatro efeitos de dois
   commits recentes (`3e4ab07`, `19ac2ea`) que só se veem rodando.

Teleport/troca de cena não entra na lista: **satisfeito por construção**, e vale registrar por quê
— a chave é de MUNDO, então teleportar não mostra radiância do lugar anterior, mostra ausência de
célula no lugar novo. Troca de cena arma `ResetPending` no `SetupForScene`.

⚠️ **As baselines de imagem NÃO servem para a rodada de telemetria.** Elas foram tiradas com a
instrumentação desligada, e o regime `d` mexe no escalonamento também do **produtor** (os atômicos
de inserção mudam quem vence o CAS). Telemetria e imagem continuam em séries separadas, como já
valia para o `S` — só que agora há três regimes, não dois.

### Histórico — implementação dos commits #5 e #6

**O laço multi-bounce entrou** (commit #6). O caminho anda até `UpdateMaxVertices` vértices
sombreados (default 4, custo = V+1 raios) e a radiância volta em ordem reversa,
`L_i = local_i + throughput_i · L_(i+1)`, com `RC_Update` em cada vértice elegível.

Quatro coisas que a implementação decidiu e que não devem ser re-derivadas:

- **As duas realimentações coexistem e não se confundem.** A do FRAME é o laço; a do TEMPO é o
  terminal lendo `Resolved` enquanto a escrita vai para `Accum`, e ela **já convergia sozinha** com
  um vértice. O laço não cria o multi-bounce — encurta a latência e tira o viés do truncamento.
  `UpdateMaxVertices = 1` reproduz o commit #5 exatamente, e é esse o A/B que decide se o laço se
  paga.
- **Segmento morto termina o caminho em PRETO, não descarta os vértices já andados.** O kill diz
  "dali para frente não vem luz" — uma afirmação sobre o *segmento*. A direta e o emissivo que os
  vértices anteriores já mediram continuam válidos. Em `bounce == 0` isso equivale a desistir,
  porque `count` fica em zero.
- **Elegibilidade usa a MESMA função da query** (`RC_ConeCoversCell`). O gate de *segmento curto* da
  query **não** se aplica ao produtor: lá ele evita auto-referência (ler a própria célula), e o
  produtor não lê nada — só escreve o que aquele ponto emite.
- **O array de vértices fica em registrador**, verificado: `getelementptr` = 0 no DXIL, ou seja,
  nenhum acesso indexado a memória. É o que os dois `[unroll]` compram, e é por isso que
  `RCU_MAX_VERTS` é constante de compilação e o knob só clampa dentro dela.

Uma armadilha que a extração do gate de cone revelou e que vale para o resto da série: escrever
`if (r < 0) return true` no lugar de `if (r >= 0) { ... }` troca `fcmp ult` por `fcmp olt` no DXIL —
**as duas formas divergem sob NaN**. A inversão "mais limpa" mudava o comportamento de um caso que
não deveria acontecer, e a comparação de DXIL pegou. Transcrito na forma original, os três shaders
de reflexão voltaram a sair com o mesmo conjunto de instruções.

**O bias de shadow ray não serve para raio de transporte** — e essa lição já tinha sido aprendida
duas vezes nesta base antes de eu reintroduzi-la. O `PT_ShadowRayOrigin` aplica o
`HitShadowRayBias`, que vale **0,20 m**: num raio de bounce isso desloca a origem em 20 cm, atravessa
parede fina — e o vazamento vai parar numa célula do cache, servido por dezenas de frames — e ainda
move a origem para longe do ponto cuja radiância se está medindo. O `ReSTIRGITrace` ("o normal-bias
de 0.2 aqui contaminava a medida") e os dois traces de reflexão ("o bias 0.2 na origem deslocava o
reflexo de contato e inflava o hitT") já usavam offset só-numérico. Os segmentos do updater agora
usam `OffsetRayWB` com a normal de face orientada para a direção de saída; o shadow ray mantém o
bias dele, onde o 0,20 é anti-acne calibrado e nenhum segmento depende da origem.

### Estado anterior — commit #5

**O produtor dedicado existe.** `Shaders/GI/RadianceCacheUpdate.cs.hlsl` + `FRadianceCache::
RecordUpdate`, gravado depois do `RecordGBuffer` e antes da espera do DDGI assíncrono. Com ele
ativo o CPU **para de armar o bit de update dos traces de render** — o cache deixou de aprender de
carona um sinal cujo terminador era o DDGI.

O que o passe faz hoje: seleciona ~4% dos pixels por permutação de período 25 sobre o tile 5×5
(sem buracos — cada posição dispara uma vez a cada 25 frames), traça do G-buffer até **v0**, sombreia
v0 com os blocos da Fase 2, amostra o BSDF, traça mais um raio e termina no **cache resolvido**, no
céu, ou em zero. Nunca em DDGI.

Cinco decisões que o commit #6 herda:

- **A política de backface existe no updater, mas o toggle nasce DESLIGADO — e essa é uma decisão
  pendente, não um esquecimento.** O toggle é o mesmo do ReSTIR GI (`FReSTIRGI::BackfacePolicy`) e
  o updater o recebe de fora, não como knob próprio: o cache alimenta o ReSTIR GI, e duas políticas
  de geometria fariam a mesma superfície ter duas radiâncias. **Na configuração padrão o bloco não
  roda**, então a exposição continua no pipeline. Ligar o default muda a imagem do render, e o plano
  proíbe fechar isso por "parece melhor". O que já está entregue é a capacidade de medir: a política
  existe e o manifesto carrega `giBackfacePolicy`.

  O argumento que mantém o default OFF no render não se transporta inteiro: lá o custo é um
  `HitIsBackface` por raio contra uma diferença medida como invisível; aqui o custo é ~25× menor
  (4% dos pixels) e a consequência dura mais — o render consome um hit ruim uma vez, o cache o
  **grava** numa célula de mundo e o serve por até 64 frames. **A medida pode terminar com defaults
  diferentes nos dois lados**, e aí a separação do toggle teria motivo medido em vez de gosto.

  Ação em v0 e no segundo segmento é deliberadamente diferente: caminho morto em v0 **não grava
  nada** — zero afirmaria "deste ponto não sai luz" e a média carregaria a afirmação. No segundo
  segmento o kill vale **zero**, porque ali é oclusão legítima de v0.

- **O vértice do G-buffer NÃO entra no cache.** Ele é só origem. Gravá-lo exigiria um segundo
  caminho de material a partir do G-buffer, que é a cópia divergente que a Fase 2 existiu para
  impedir. O vértice gravado é o primeiro hit — a mesma população que as consultas acertam.
- **Multi-bounce no tempo, antes do multi-bounce no frame.** Escrita em `Accum`, leitura de
  `Resolved`: `L_novo = direta + f·L_velho` é uma iteração de ponto fixo entre frames que já
  converge para o transporte completo. O laço de 4 vértices encurta a latência, não cria o efeito.
- **A chave do terminal usa `PT_LoadHitSurface`, não `HitGeomNormal`.** A normal do cache tem de ser
  a mesma que o `ShadeSurfaceHit` usa para consultar (interpolada com facing); a de face divergiria
  perto da silhueta de malha suavizada e update e query montariam chaves diferentes para o mesmo
  ponto.
- **O passe de update não conta em `RC_STAT_QUERIES/HITS`.** Aqueles contadores descrevem os raios
  de RENDER, e a instrumentação não é neutra (é o achado de `3872f97`). Somar as consultas do
  terminal mudaria a série e a medida ao mesmo tempo.

Evidência de que o render não mudou, pela técnica que a Fase 2 estabeleceu: o DXIL de Release dos
cinco traces que incluem o `PathTracingCommon.hlsli` tem **o mesmo conjunto de instruções**. Os três
shaders de reflexão saem idênticos até a numeração SSA; `ReSTIRGITrace` e `DDGITrace` diferem em
**8 nós `phi` reordenados dentro do próprio bloco** — e phi de um bloco é simultâneo, então a ordem
entre eles não tem semântica. Nada foi acrescentado, removido ou trocado. Os campos novos do
`FPathState` e o `PT_SampleBSDF` não têm chamador ali e o DCE os elimina.

**Falta da Fase 3** (commit #6, `gi: backpropagate multi-bounce radiance into WRC`): o laço de até
4 vértices, a backpropagation em ordem reversa, `RC_Update` por vértice elegível, o
`MinCacheableRoughness` (o gate de cone do lobo que CHEGA, que só passa a existir quando um vértice
pode ser alcançado por lobo estreito) e o `UpdateMaxBounces` de verdade — hoje o `UpdateParams.y`
viaja como 1 e o shader nem o lê, de propósito: knob que não controla nada é pior que knob ausente.

**Mesh lights não entram no updater, e isso é PARIDADE e não esquecimento.** O plano lista
"mesh lights/ReGIR" entre os insumos, mas o `PT_AddDirectLocal` — que o ReSTIR GI e o DDGI usam nos
hits deles — só trata puntuais e ReGIR; geometria emissiva contribui quando um raio a atinge, pelo
`PT_LoadHitEmissive`. Acrescentá-las só no updater faria a mesma superfície ter duas radiâncias
conforme o caminho, que é exatamente o que a Fase 2 fechou. Quando entrarem, entram no bloco
compartilhado e nos três consumidores de uma vez.

Não entrou e continua fora: `UseDDGIBootstrap`. Ele exige preencher a cauda de cascatas do cbuffer
do passe, e a struct `FDDGICascadeConstants` mora no `DDGI.h`, que **inclui** o `RadianceCache.h` —
tirá-la de lá é refactor, e refactor não se mistura com mudança de estimador no mesmo commit.

Duas coisas já decididas que continuam valendo:

- **O update nunca lê DDGI** como fonte normal. Hoje isso é propriedade do código, não gate: o
  `PT_SampleIndirectFallback` não tem chamador no passe, e o DXC o elimina.
- **`Accum` recebe o frame atual; `Resolved` é somente leitura** para os traces do mesmo frame. É a
  barreira contra auto-realimentação, e o motivo de o resolve continuar no fim.

Para medir, a régua já está pronta: baselines em `ed7b543`, N = 128, instrumentação desligada para
imagem e ligada para telemetria (nunca as duas séries misturadas). **O commit #5 ainda não foi
medido** — ele compila em Debug e Release e o DXIL prova que o render não mudou, mas o gate de saída
da Fase 3 (ocupação cresce só pelo passe dedicado, com DDGI desligado e query de render desligada)
é medição, e ela não foi feita.

## ESTADO — 2026-08-12

Bloco de retomada. Tudo abaixo já foi decidido e verificado no código; não re-derivar.

### O que já entrou

| Commit | Fase | O quê |
|---|---|---|
| `1a52ce9` | 1a | `InstanceGeo` sai do `FDDGI` → `FRaytracingScene`. Mais 3 correções de lifetime (dreno das duas filas no `BuildRaytracingScene`; `ComputeQueue.WaitIdle` nos dois `RefreshInstanceGeo`; `FReSTIRDI::RefreshMeshLightDescriptors`, que consertou tela branca ao religar o DI depois de duplicar objetos). |
| `14571d7` | 1b | Early return `if (!DDGI.IsReady()) return;` removido. `FGIFallbackBindings` + recursos neutros 1×1 zerados. `WantNrdIndirect()` solto do volume. Gate no shader via `FGIHitSampling::FallbackAvailable`. |
| `a67eadd` | 1b | Fallback escolhido por **existência** do volume (`DDGI.IsReady()`), não por `UseGI`. |
| `3361762` | régua | Bookmarks de câmera, 4 slots, sidecar `<cena>.cameras.json`. |
| `05a88f7` | régua | `TemporalSampleIndex` separado do `FrameIndex`; incremento movido para o fim do frame. |

**Fase 1 está fechada.** Fase 0 nunca teve capturas registradas — é o que a régua existe para
resolver, e por isso ela entrou antes do commit #4 em vez de depois.

### Conclusões que não devem ser re-derivadas

- **Fase 0 já estava quase toda em código.** As quatro configurações de A/B são alternáveis pela UI
  hoje, e a flag "DDGI no hit secundário" que o plano pede para acrescentar já existe:
  `FGIHitSampling::TerminatorOff` → bit 2 do `SkipModePacked()` → `termOff` no `HitShading.hlsli`,
  exposta como `giMeasureTerminatorOff`. O que falta da Fase 0 é **medição**, não código.
- **`HistoryDomain::CameraCut` NÃO derruba os caches de mundo** (`DDGIAtlas`, `ReGIR`,
  `RadianceCache`), de propósito. Correto para navegação, errado para captura — daí o domínio
  `DeterministicCapture` pendente.
- **O aquecimento é de centenas de frames**, não dezenas: histerese 0,99 no DDGI (~199 updates para
  o atlas de distância) e teto de 64 amostras no radiance cache.
- **Existência × habilitação.** O que é lido no setup fica *latched* na tabela de descritores; o que
  é lido por frame não. `UseGI` muda sem provocar setup, então não pode participar de escolha de
  descritor. Critério para qualquer campo novo no `FGIFallbackBindings`.
- **`BuildRaytracingScene()` tem de ser seguido de `SetupGIForScene()`** — a tabela de trace do DDGI
  carrega uma *cópia* do descritor do snapshot e não acompanha a realocação sozinha.
- **Preset científico:** desligar o upscaler **ativa** o TAA (`TAAActive = UseTAA && !UpscaleActive`),
  trocando jitter de FSR por Halton. Tem de zerar `UseTAA` também.
- `Time.z` do cbuffer carrega o `FrameIndex` e **nenhum shader o lê**. Payload morto, removível.

### O que falta, em ordem

1. ~~**Capturador**~~ — feito, exceto a **calibração do N** (instrumentação de convergência, teto
   de 512). Ver o bloco de estado no topo e o `Docs/CAPTURE-PROTOCOL.md`.
2. **Commit #4** — `gi: split hit shading from indirect fallback policy`. Quebra o
   `ShadeSurfaceHit`, introduz `FPathState` e `FRCQueryResult`. É o primeiro teste real da régua:
   promete não mudar imagem, então serve para validar o capturador.
3. **Fase 3 em diante** — o path tracer esparso, onde o estimador muda de verdade.

### Ressalva sobre comparação com builds antigas

O `05a88f7` moveu o incremento dos contadores para o fim do frame, o que desloca algumas sementes
em −1. Imagem contra builds anteriores **não é bit a bit**; o critério é "o ruído mudou, a energia
não".

---

## Objetivo

Transformar o `FRadianceCache` atual em um cache world-space de radiância no estilo SHaRC, alimentado por um path tracer esparso e usado como terminador primário do transporte indireto. O `FReSTIRGI` continua responsável pelo reúso espaço-temporal do ponto secundário. O DDGI deixa de definir o sinal principal e passa a ser consultado somente quando o cache não consegue responder com confiança, além de continuar atendendo consumidores que realmente precisam de irradiância volumétrica, como fog.

O estado final pretendido é:

```text
ReSTIR DI ------------------------------------------------------> direta

G-buffer -> ReSTIR GI / indirect path sample -> SHaRC/WRC -----> indireta
                                        miss / cache frio |
                                                          +----> DDGI fallback
                                                          +----> ambiente/zero se DDGI off

Sparse cache-update path tracer -> Accum -> Resolve -> SHaRC/WRC do próximo frame
```

Esta implementação deve evoluir o que já existe. Não importar uma implementação inteira de SHaRC nem substituir imediatamente o ReSTIR GI próprio pela RTXDI 3.0. O escopo padrão tem dez fases, de 0 a 9, e termina com o `FReSTIRGI` atual usando SHaRC/WRC como fonte primária e DDGI como fallback. ReSTIR PT é um épico futuro, opcional e dependente de autorização separada.

## Decisões de arquitetura

1. `FRadianceCache` continua sendo o dono da tabela, resolve, debug e telemetria. Ele ganha um produtor explícito de paths; não nasce um segundo cache concorrente.
2. O cache guarda radiância de saída world-space, não irradiância. Ele permanece não direcional. Superfícies especulares só podem consultá-lo quando o cone do raio cobrir ao menos um voxel; contribuições estreitas não devem alimentá-lo.
3. A atualização deixa de acontecer incidentalmente em todo `ShadeSurfaceHit`. Somente o passe dedicado de update escreve radiância no cache. Os traces de render consultam, mas não treinam o cache.
4. O passe de update nunca usa DDGI como fonte normal de radiância. Caso contrário, o cache apenas esconderia um sinal cuja origem continuaria sendo DDGI. Um modo de bootstrap com DDGI pode existir apenas como knob de diagnóstico e deve nascer desligado.
5. `Accum` recebe o frame atual; `Resolved` é somente leitura para os traces do mesmo frame. O resolve continua no fim e publica o resultado para o frame seguinte. Essa separação é a barreira contra auto-realimentação.
6. O hash de 32 bits + checksum + linear probing atual é mantido inicialmente. Ele entrega o comportamento necessário sem exigir `InterlockedCompareExchange64` e custa 36 MiB em `2^20`, contra aproximadamente 167 MB da configuração do Cyberpunk.
7. DDGI é fallback por hit e por capacidade do hardware, não outro “look” artístico. Materiais, luzes, intensidade e exposição são comuns aos dois caminhos.
8. ReSTIR DI e os dois domínios de denoising permanecem separados. Direta continua em `NrdDirect`; GI/reflexos continuam no `Nrd` indireto.
9. RTXDI ReSTIR PT fica fora do caminho crítico e do critério de conclusão. O cache deve funcionar com o `FReSTIRGI` atual; o apêndice de ReSTIR PT só é executado se houver autorização e hardware-alvo compatível.

## Estado de PARTIDA (como o código estava quando este plano foi escrito)

> ⚠️ **Isto NÃO é o estado atual** — o título dizia "Estado atual" e envelheceu mal. É o retrato do
> ponto zero, mantido porque as fases foram escritas contra ele. O que vale hoje está no bloco
> **ESTADO** no topo do arquivo. Os cinco primeiros itens abaixo **já foram resolvidos**: as Fases 1
> e 3 removeram o early return, soltaram o `InstanceGeo` do DDGI e trocaram o produtor do cache.

- `Renderer::SetupForScene` já monta `RadianceCache` independentemente da AABB.
- ~~O setup de resize ainda contém `if (!DDGI.IsReady()) return;`~~ — removido na Fase 1 (`14571d7`).
- ~~`FReSTIRGI::SetupForResize` exige atlas/ProbeData do DDGI~~ — passou a receber
  `FGIFallbackBindings`, que sabe não existir (Fase 1).
- ~~O snapshot `InstanceGeo` é exposto por `FDDGI::InstanceSRV()`~~ — mudou-se para
  `FRaytracingScene` (Fase 1, `1a52ce9`).
- ~~`ShadeSurfaceHit` grava o resultado com `RC_Update`, então o cache aprende um sinal derivado do
  DDGI~~ — **este era o problema central da série, e a Fase 3 o fechou**: quem escreve é o passe
  dedicado, e os traces de render só consultam.
- O cache atual já possui LOD por distância, normal por octante, proteção por comprimento do segmento, gate de cone especular, média temporal, refresh, tombstones, evicção, visualizador e readback de métricas.
- O `FReSTIRGI` já possui trace inicial, reuso temporal, reuso espacial, Jacobiano, limite de idade configurável, boiling filter, pack para NRD e histórico de superfície confiável.
- O RTXDI local é 3.0 e contém ReSTIR PT, mas o SmileEngine não liga o runtime; atualmente apenas replica/adapta partes dos algoritmos nos shaders próprios.
- O worktree contém alterações locais relevantes em DDGI, `HitShading`, `ReSTIRGITrace`, fog, editor e outros arquivos. O implementador deve preservar tudo, nunca usar reset/checkout destrutivo e revisar o diff existente antes de tocar em arquivos sobrepostos.

## Conclusões anteriores que este plano deve preservar

Esta seção existe para que uma compactação de contexto ou troca de implementador não apague decisões já tomadas durante a leitura das três referências.

### idTech8 / DOOM: WRC eficiente, mas não SHaRC de paths

- O WRC da idTech8 é um hash intermediário de shading em um pipeline híbrido, não o mesmo estimator do SHaRC do Cyberpunk.
- As probes traçam visibilidade e produzem hit packets compactos de 128 bits. O sistema deduplica/hash os hits antes do shading, forma listas de células ativas e sombreia por indirect dispatch/material; a apresentação cita aproximadamente 20 mil células sombreadas por frame.
- O WRC usa células-base de 25 cm com LOD, orçamento de aproximadamente 14 MiB e reaproveitamento temporal.
- O final gather não volta a sombrear o material. A hierarquia é, conceitualmente, screen-space, depois WRC e, em miss, irradiance volumes.
- A principal lição para a Smile não é copiar o estimator da idTech, mas copiar scheduling, compactação, deduplicação antes do shading, work lists por material e disciplina de memória.

### Assassin's Creed Shadows: DDGI excepcional, porém adequado como fallback

- O sistema usa cinco cascatas de `16 x 16 x 8`, total de 10.240 probes e cobertura de cerca de 512 m, com scroll toroidal e spacing de 2 a 32 m.
- Cada probe mantém um G-buffer persistente. Traçar/atualizar visibilidade e relight são operações separadas; somente uma fração das probes recebe trace completo por frame, enquanto o relight é mais frequente.
- Metadados de luminância permitem reagir rapidamente a Time of Day mesmo em probes não selecionadas para trace completo.
- As probes publicam radiância direcional e irradiância difusa separadamente. O multi-bounce lê a irradiância do frame anterior.
- O final gather difuso usa primeiro screen trace e depois BVH. Isso reforça que o caminho mais barato deve ser tentado antes de um trace completo quando a validação for confiável.
- A própria apresentação registra os limites de distribuição uniforme e leaking e aponta importance sampling, surface probes e estruturas esparsas como direções futuras. Por isso, aqui o DDGI permanece como cobertura ampla/fog/cache frio, não como sinal primário de superfície.

### Cyberpunk 2077: referência principal para a arquitetura escolhida

- ReSTIR GI e SHaRC são complementares. ReSTIR GI reutiliza o ponto/path secundário; SHaRC fornece radiância world-space para aumentar a qualidade dos bounces e terminar caminhos.
- O update do SHaRC é independente: cerca de 4% dos pixels, até quatro bounces, contra o caminho de render mais curto. Ele pode terminar consultando o cache do frame anterior e faz backpropagation.
- A consulta usa posição, normal, LOD logarítmico e regras de comprimento do segmento/tamanho do voxel e cone especular.
- A configuração publicada usa `2^22` entradas e aproximadamente 167 MB. A Smile deliberadamente começa em `2^20` e aproximadamente 36 MiB.
- O ReSTIR GI prático do jogo limita o histórico a oito frames. Para specular, reduz o raio espacial com roughness e desliga o spatial abaixo de `0.1`.
- Direta e indireta permanecem em domínios de denoising separados porque seus perfis de variância são radicalmente diferentes.
- O jogo combina a escolha de lobo difuso/especular em um trace inicial e corrige o PDF da mistura. Isso pode virar uma otimização futura do pipeline atual sem exigir ReSTIR PT completo, mas não é requisito para colocar SHaRC como primário.

## Regras para o implementador

- Cada fase termina em estado compilável e executável.
- Não misturar refactor estrutural, mudança de estimator e tuning visual no mesmo commit.
- Não apagar o caminho DDGI existente enquanto os gates finais não tiverem passado.
- Novos recursos de GPU entram no `VramTracker`/categoria GI e recebem nome de debug.
- Todo recurso lido por descriptor table deve receber descriptor válido mesmo quando o fallback estiver desligado; o shader é quem fecha a leitura por flag.
- Mudança de layout de cbuffer exige `static_assert` de tamanho/offset no C++ e comentário de contrato no HLSL.
- Mudanças que alterem o significado do histórico invalidam os domínios corretos: cache, ReSTIR GI, NRD/RR e TAA conforme o consumidor.
- Antes de otimizar, capturar baseline de imagem, custo, VRAM e métricas. “Parece melhor” não fecha fase.
- Não integrar ReSTIR PT durante o escopo padrão. Duas mudanças estatísticas grandes simultâneas tornam qualquer regressão impossível de localizar; o apêndice futuro exige autorização própria.

## Fase 0 — Congelar baseline e criar a matriz A/B

### Objetivo

Documentar o comportamento atual e criar modos que permitam provar de onde vem cada contribuição.

### Trabalho

- Registrar o estado inicial do worktree com `git status --short` e salvar o diff dos arquivos que a fase tocar. Não reverter alterações do usuário.
- Usar uma câmera determinística e capturar ao menos:
  - uma área predominantemente indireta;
  - uma parede fina com lados claro/escuro;
  - um emissivo forte;
  - uma sequência de roughness de 0 a 1;
  - uma cena com sol/Time of Day variando;
  - um teleport de câmera;
  - objeto e luz em movimento.
- Registrar GPU timings do frame e, separadamente:
  - ReSTIR GI temporal + trace;
  - ReSTIR GI espacial + resolve;
  - radiance cache resolve;
  - DDGI trace/update/relocate/classify;
  - NRD indireto;
  - ReSTIR DI e NRD direto como controles.
- Registrar VRAM de DDGI, ReSTIR GI, NRD, reflexos e radiance cache.
- Capturar quatro baselines:
  1. DDGI principal, cache desligado;
  2. cache atual atualizando, query desligada;
  3. cache atual atualizando e consultando;
  4. ReSTIR GI desligado, DDGI puro.
- Acrescentar, se não houver equivalente, uma flag de diagnóstico “DDGI no hit secundário” que desliga somente o gather dentro de `ShadeSurfaceHit`, sem desligar o DDGI do deferred/fog. O bit já parcialmente existe em `SkipModePacked`; torná-lo um modo explícito e nomeado.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/RenderSettings.h`
- `Engine/Source/Graphics/RenderSettings.cpp`
- `Editor/Source/RenderSettingsBridge.cpp`
- `Editor/Qml/SettingsWindow.qml`
- `Engine/Source/Graphics/Renderer.cpp`
- `Shaders/GI/HitShading.hlsli`

### Gate de saída

- As quatro configurações podem ser alternadas sem restart.
- O profiler e a UI não apresentam métricas velhas de um passe desligado.
- Existe uma captura reproduzível antes de qualquer mudança de estimator.
- Build de shaders e do editor passa.

## Fase 1 — Desacoplar a infraestrutura de RT do DDGI

### Objetivo

Permitir que ReSTIR GI, reflexões, ReGIR e temporal motion inicializem com TLAS e materiais válidos mesmo quando o volume DDGI não existir.

### Trabalho

- Mover a propriedade/publicação do snapshot `InstanceGeo` para `FRaytracingScene` ou para uma pequena estrutura comum `FRTSceneBindings`.
- `FDDGI`, `FReSTIRGI`, `FReSTIRDI`, `FReflections`, `FTemporalMotionVectors`, mesh lights e o futuro cache updater passam a receber o mesmo `InstanceSRV` comum.
- Separar os pré-requisitos do bloco de resize do renderer:
  - reflexões precisam de TLAS, instâncias, G-buffer, depth, velocity, ambiente e recursos de fallback válidos;
  - ReSTIR GI precisa de TLAS, instâncias, G-buffer, depth, velocity, ambiente e fallback opcional;
  - NRD indireto depende de ReSTIR GI/reflexos e do modo de denoiser, não de `DDGI.IsReady()`;
  - somente o volume DDGI e seus debug passes dependem de `DDGI.IsReady()`.
- Remover o early return global após `SetupNrdDirect`. Substituí-lo por setup condicional de cada passe.
- Alterar `WantNrdIndirect()` para depender do produtor indireto ativo, não do DDGI.
- Criar bindings neutros válidos para fallback ausente, preferencialmente recursos 1x1/1-elemento zerados pertencentes ao renderer. Não copiar `kInvalidSlot` para descriptor tables.
- Introduzir um contrato explícito, por exemplo:

```cpp
struct FGIFallbackBindings {
    u32 IrradianceAtlasSRV;
    u32 DistanceAtlasSRV;
    u32 ProbeDataSRV;
    FDDGICascadeConstants Cascades;
    bool Available;
};
```

- O nome pode mudar, mas o conceito deve impedir que a classe `FReSTIRGI` volte a depender diretamente de `FDDGI`.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/RaytracingScene.h`
- `Engine/Source/Graphics/RaytracingScene.cpp`
- `Engine/Include/Smile/Graphics/DDGI.h`
- `Engine/Source/Graphics/DDGI.cpp`
- `Engine/Include/Smile/Graphics/ReSTIRGI.h`
- `Engine/Source/Graphics/ReSTIRGI.cpp`
- `Engine/Include/Smile/Graphics/ReSTIRDI.h`
- `Engine/Source/Graphics/ReSTIRDI.cpp`
- `Engine/Include/Smile/Graphics/Reflections.h`
- `Engine/Source/Graphics/Reflections.cpp`
- `Engine/Source/Graphics/Renderer.cpp`

### Gate de saída

- Com DDGI desabilitado/não pronto, ReSTIR DI continua funcionando.
- ReSTIR GI e reflexões chegam a `IsReady()` com descriptors válidos.
- ReSTIR GI produz direto no hit + emissivo + ambiente/zero no terminal, sem access violation e sem ler atlas DDGI inválido.
- D3D12 debug layer não reporta descriptor, state ou lifetime errors.
- A imagem com DDGI habilitado permanece equivalente ao baseline; esta fase não muda o estimator.

## Fase 2 — Refatorar o shading de hit sem mudar a imagem

### Objetivo

Separar carregamento de superfície, iluminação local, transporte indireto e política de cache. O path tracer de update precisa reutilizar exatamente o mesmo material/shading sem herdar a decisão “sempre sample DDGI”.

### Trabalho

- Quebrar `ShadeSurfaceHit` em blocos menores, mantendo um wrapper compatível para os consumidores atuais:
  - classificação/facing e offset geométrico;
  - carregamento de material e texturas;
  - direto do sol;
  - direto de luzes locais/ReGIR e shadow rays;
  - emissivo;
  - avaliação/amostragem do BSDF;
  - fallback indireto DDGI;
  - composição da radiância de saída.
- Introduzir um pequeno `FPathState` HLSL com, no mínimo:
  - origem/direção; ✅ **feito**
  - throughput; ⏳ **adiado para a Fase 3**
  - depth; ✅ **feito**
  - roughness/cone do segmento; ✅ **feito**
  - PDF e lobo escolhido; ⏳ **adiado para a Fase 3**
  - flags de modo: render, cache update, replay futuro. ✅ render e cache update; ⏳ replay na Fase 3.

> **Parte da Fase 2 explicitamente PENDENTE** (commit #4, `PathTracingCommon.hlsli`). Throughput,
> PDF e lobo escolhido não entraram porque nenhum produtor os preenche ainda: quem os calcula é o
> laço de bounces do updater, que só existe na Fase 3. Campo sem produtor nasce com valor
> inventado, e o primeiro consumidor a lê-lo herdaria a invenção sem saber.
>
> Pelo mesmo motivo **não há amostragem de BSDF reutilizável** ainda — o render avalia a BRDF para
> uma direção conhecida (`BRDF_Direct`), não *amostra* um lobo; a amostragem nasce com quem precisa
> dela, que é o updater.
>
> A consequência prática: quem abrir a Fase 3 completa o `FPathState` **antes** de escrever o laço,
> e não depois. Isso não é dívida técnica — é a ordem certa —, mas não pode passar por "Fase 2
> concluída" numa leitura rápida.
- A API de consulta ao cache deve devolver um resultado estruturado em vez de apenas `bool`:

```hlsl
struct FRCQueryResult {
    float3 Radiance;
    float  CellSize;
    uint   SampleCount;
    uint   Age;
    uint   Status;
};
```

- Manter as regras atuais de segmento maior que voxel e cone especular maior que voxel.
- Remover o `RC_Update` do wrapper compartilhado somente quando o passe dedicado da Fase 3 existir. Até lá, protegê-lo por uma flag de produtor legado para manter a equivalência.
- Não duplicar BRDF entre o path updater e ReSTIR GI. Funções comuns devem viver em um header como `PathShading.hlsli`/`PathTracingCommon.hlsli`.

### Arquivos prováveis

- `Shaders/GI/HitShading.hlsli`
- `Shaders/GI/RadianceCache.hlsli`
- novo `Shaders/GI/PathTracingCommon.hlsli`
- `Shaders/GI/ReSTIRGITrace.cs.hlsl`
- shaders de reflexo que incluem `HitShading.hlsli`
- `Shaders/GI/DDGITrace.cs.hlsl`

### Gate de saída

- Shader output das configurações baseline permanece visualmente e numericamente equivalente dentro do ruído temporal esperado.
- Nenhum consumidor possui uma cópia divergente de material, emissivo, shadow bias ou luz local.
- Todos os shaders que incluem o contrato compilam em SM 6.6.

## Fase 3 — Criar o path tracer esparso de atualização do cache

### Objetivo

Dar ao cache uma fonte própria de radiância multi-bounce que não dependa dos hits produzidos por DDGI, ReSTIR GI ou reflexões.

### Trabalho

- Acrescentar ao `FRadianceCache` um pipeline `RadianceCacheUpdate` e um método `RecordUpdate`.
- Criar `Shaders/GI/RadianceCacheUpdate.cs.hlsl`.
- Usar pixels de G-buffer como origens dos paths. O primeiro estágio recebe depth, normal/material, `InvViewProj`, TLAS, instâncias, sky, luzes, mesh lights/ReGIR e os buffers do cache.
- Não gravar esse updater dentro de `PrepareIndirectLighting()`: hoje essa função roda antes de `RecordGBuffer()`, portanto depth/material do frame ainda não existem no estado necessário.
- O primeiro ponto de integração correto é imediatamente após `RecordGBuffer(Ctx)` e antes de `RecordSceneLighting(Ctx)`. Preferencialmente, gravar o update antes da espera do DDGI assíncrono: ele não consome DDGI e seu trabalho na fila direta pode sobrepor o compute já em voo. Restaurar depth/G-buffer aos estados que o bloco de espera e `RecordSceneLighting` esperam.
- Como passo inicial mais simples, o update pode viver no começo de `RecordSceneLighting`, antes de `ReSTIRGI.RecordTrace`; essa alternativa sacrifica overlap, mas mantém a dependência correta.
- Selecionar aproximadamente 4% dos pixels com uma sequência progressiva determinística, por exemplo uma permutação de período 25. Evitar um sorteio independente que deixe buracos por muitos frames.
- Começar com dispatch full resolution + early-out pela máscara. Só compactar depois da correção estar comprovada.
- Traçar até quatro bounces no update, com um raio por bounce e next-event estimation para sol/luz selecionada quando aplicável.
- Escolher o lobo a partir da energia difusa/especular, com PDF da mistura. Não alimentar o cache a partir de um lobo estreito que ele não consegue representar.
- Registrar por vértice, para no máximo quatro vértices:
  - posição e normal geométrica;
  - radiância direta + emissiva local;
  - throughput/PDF para o próximo vértice;
  - elegibilidade para cache;
  - distância e roughness do segmento.
- No primeiro corte de correção, consultar `Resolved` somente no terminal do path de update. O terminal pode ser emissivo, ambiente, cache anterior ou zero. Não usar DDGI.
- Fazer backpropagation em ordem reversa:

```text
L_terminal = emissivo/sky/cache anterior/zero
L_i = direto_i + emissivo_i + throughput_i * L_(i+1)
RC_Update(pos_i, normal_i, L_i) para cada vértice elegível
```

- Se o loop for implementado inteiramente num único compute, o array local deve ter tamanho fixo e pressão de registradores medida. Se occupancy despencar, usar duas etapas com buffer compacto de paths; não otimizar por suposição.
- Nomear o scope do profiler como `Radiance cache (update)` separado do resolve.
- O passe deve rodar depois do G-buffer e antes dos consumidores de iluminação que consultam o cache. Ele consulta sempre `Resolved` do frame anterior; as escritas vão somente para `Accum`, e o resolve continua depois de todos os traces. Não é necessária uma barreira entre update e query sobre `Resolved`, apenas ordenar/visibilizar `Accum` antes do resolve.

### Novos parâmetros mínimos

- `UpdateFraction`, default `0.04`. ✅ **feito** — quantizado em 1/25 pela permutação do tile.
- `UpdateMaxBounces`, default `4`. ⏳ nasce com o laço (commit #6); hoje o campo viaja como 1 e o
  shader não o lê.
- `RenderMaxBounces`, inicialmente preserva o comportamento atual do ReSTIR GI.
- `MinCacheableRoughness` ou regra equivalente. ⏳ commit #6 — só faz sentido quando um vértice
  puder ser alcançado por lobo estreito; em v0 o raio que chega é sempre difuso.
- `UsePreviousCacheAtTerminal`, default ligado. ✅ **feito**.
- `UseDDGIBootstrap`, somente debug, default desligado. ❌ **não entrou** — depende de tirar o
  `FDDGICascadeConstants` do `DDGI.h` (ciclo de include), e isso é refactor próprio.
- `DedicatedUpdate` (não estava na lista): escolhe entre o produtor dedicado e os hits do render.
  Nasce LIGADO; o caminho antigo fica como controle de A/B. ✅ **feito**.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/RadianceCache.h`
- `Engine/Source/Graphics/RadianceCache.cpp`
- `Engine/Source/Graphics/Renderer.cpp`
- novo `Shaders/GI/RadianceCacheUpdate.cs.hlsl`
- `Shaders/GI/RadianceCache.hlsli`
- `Shaders/GI/PathTracingCommon.hlsli`
- `Shaders/CMakeLists.txt`

### Gate de saída

- Com DDGI desligado e query de render desligada, o cache acumula radiância válida apenas pelo update dedicado.
- Desligar todos os antigos consumidores de update não faz a ocupação parar de crescer.
- A taxa de paths e profundidade média batem com os parâmetros.
- Não há NaN/Inf, overflow do acumulador ou GPU hang em emissivos extremos.
- O custo do update aparece isolado no profiler.

## Fase 4 — Tornar o resolve e a confiança adequados ao produtor dedicado

### Objetivo

Publicar um cache estável, responsivo e mensurável antes de habilitá-lo como sinal principal.

### Trabalho

- Manter o limite de 64 amostras e a evicção após 64 frames como ponto inicial. ✅ **já era assim**
- Manter o refresh escalonado por checksum para evitar tempestades periódicas. ✅ **já era assim**
- Verificar matematicamente a média após atingir o teto. O peso histórico deve permanecer limitado; não deixar a célula congelar com um `prevSamples` efetivo maior que o armazenado. ✅ **auditado, correto — não mexer**
- Definir estados explícitos da célula: ✅ **saem dos status da query, sem buffer novo**
  - ausente; → `RC_QUERY_NO_ENTRY`
  - presente sem amostra; → `RC_QUERY_NO_SAMPLES`
  - aquecendo; → `RC_QUERY_WARMING` (novo)
  - confiável; → `RC_QUERY_HIT`
  - precisa refresh; → `RC_QUERY_STALE`
  - stale/evictada. → **não observável pela consulta**, e de propósito: o resolve troca a chave por
    tombstone no mesmo passe, então a busca seguinte cai em `NO_ENTRY`.
- A confiança inicial pode usar somente `sampleCount`, `age` e elegibilidade geométrica. Variância/segundo momento só entra se os testes mostrarem necessidade; não aumentar 16 MiB por buffer sem evidência. ✅ **piso por `sampleCount`; nenhum buffer novo**
- Criar aquecimento global do cache: ✅ **`ERadianceCacheWarmup`, sem buffer nem passe novo**
  - `Filling`: update ativo, queries de render ainda não fazem early-out; ✅ e o terminal do
    **updater** continua consultando — é ele que enche o cache com multi-bounce.
  - `Active`: cobertura/convergência mínimas atingidas ou janela de warm-up completada; ✅ 70% das
    células com amostra confiáveis, ou 96 frames — **sempre com tabela não vazia**, nos dois
    caminhos. A borda invalida os consumidores, como o toggle manual de leitura.
  - `Resetting`: mudança de chave, cena ou teleport; queries fechadas até o resolve de reset. ✅ já
    era o `ResetPending`; o estado só o **nomeia**, para o painel e o manifesto distinguirem
    "resetando" (um frame) de "enchendo" (dezenas).
- Preservar o controle manual update/query para A/B, mas o modo de produção deve ser automático.
  ✅ `AutoWarmup` nasce ligado; desligado, a consulta volta a seguir só o toggle de leitura.
- Expandir estatísticas: ✅ **30 contadores no total; diagnóstico ampliado sob `RC_FLAG_STATS_DETAIL`**
  - tentativas e falhas de inserção; ✅ `full` (capacidade) separado de `contended` (concorrência),
    com `retries` próprio.
  - probes percorridos por busca, média e máximo; ✅ medidos na INSERÇÃO — a busca da query
    percorre a mesma cadeia, e medir lá dobraria os atômicos do caminho quente. Só a primeira
    varredura entra em `probeSum`/`probeMax`; retries não inflam o comprimento da cadeia.
  - misses por chave, zero samples, refresh, segmento curto e cone estreito; ✅ + `aquecendo`
  - updates aceitos/descartados; ✅ aceitos = `insertTries − insertFull − contended − capped`
  - paths lançados e profundidade média/máxima; ✅
  - terminal por sky, emissivo, cache anterior ou limite de bounce. ✅ como `sky` / `cache` /
    `killed` (segmento morto) / `miss` / `noquery` / `lobe` / `other`. Emissivo não é um terminal
    separado neste produtor: ele entra no `local` do vértice pelo `PT_LoadHitEmissive`, não fecha
    o caminho.
- Expandir visualização com `Age`, `Confidence`, `Fallback source` e, se barato, `Path depth`.
  ✅ **`Age` e `Confidence` entraram**, e o modo "cobertura" já distinguia aquecendo (azul)
  respeitando o piso. `Age` quebra a rampa no limiar de refresh (que é por célula); `Confidence`
  mede N contra o **piso** e satura acima dele, que é o que o separa de `Converged` (N/64).
  `Fallback source` fica para a Fase 5, quando existir escolha de fallback por hit. `Path depth`
  não entrou: o produtor grava por vértice e a profundidade é do CAMINHO, não da célula — pintá-la
  na tela exigiria um campo novo na tabela, que é exatamente o "16 MiB sem evidência" que o item
  anterior proíbe.

### Gate de saída

- Em câmera estática, ocupação fica preferencialmente entre 20% e 70%; acima disso exige ajuste de célula/LOD/capacidade antes de seguir.
  ⏳ **19,2% projetados** para o default V1 em 2¹⁷ (medido: 25.176 células). Falta a captura na
  capacidade nova — a projeção é aritmética, não medida.
- Falha por bucket cheio deve ser residual e explicitamente reportada; alvo inicial menor que 0,1% das inserções, ideal menor que 0,01%.
  ⏳ **0,000% em 2²⁰**, mas com 2,40% de ocupação o gate não tinha como falhar. Repetir em 2¹⁷.
- A convergência responde a luz acesa/apagada e Time of Day sem congelar por dezenas de segundos.
  ⏳ **sem protocolo** — a régua determinística é de pose e tempo FIXOS, e este gate é dinâmico por
  definição. Instrumento pronto (miss `refresh`, `cacheEvicted`, modo `Age`); falta como medir.
- Teleport e troca de cena não mostram radiância do local anterior.
  ✅ **por construção**: a chave é de MUNDO. Teleportar não traz radiância do lugar anterior — traz
  ausência de célula no lugar novo, que o aquecimento global agora nomeia. Troca de cena arma
  `ResetPending` no `SetupForScene`.
- O cache pode aquecer com DDGI completamente desligado.
  ✅ propriedade do código desde a Fase 3: o `PT_SampleIndirectFallback` não tem chamador no passe
  de update e o DXC o elimina.

## Fase 5 — Usar SHaRC/WRC como terminador primário do ReSTIR GI

### Objetivo

Fazer o `FReSTIRGI` produzir seu `Lo` secundário a partir do cache independente e deixar o DDGI somente no ramo de miss.

### Trabalho

- No trace inicial de ReSTIR GI:
  1. traçar da superfície primária para o ponto secundário;
  2. classificar o hit e formar a chave;
  3. consultar o cache antes da avaliação cara de material quando o path for elegível;
  4. em hit confiável, usar a radiância do cache como `Lo` e encerrar;
  5. em miss, avaliar material, direto e emissivo do hit;
  6. somente no terminal do miss consultar o fallback DDGI, se disponível;
  7. sem DDGI, usar ambiente/zero de maneira explícita.
- O render path não escreve em `Accum`.
- O cache não pode retornar radiância direcional para mirror/narrow glossy. Manter o gate por cone e testar o sweep de roughness.
- Preservar a distância do primeiro hit para NRD. Um cache hit muda a fonte da radiância, não a geometria visível que guia o denoiser.
- Limitar a idade do reservoir de ReSTIR GI a oito frames no preset SHaRC, seguindo a solução prática do Cyberpunk. Não tornar isso literal global se um A/B demonstrar regressão; o knob já existe.
- Religar validação periódica somente se o limite de idade + atualização do cache não responderem a luz dinâmica. Evitar pagar dois mecanismos sem necessidade.
- Invalidar ReSTIR GI, NRD/RR e TAA na borda em que o produtor primário muda entre DDGI e SHaRC.
- Manter temporal e espacial do `FReSTIRGI` inalterados no primeiro commit desta fase. Ajustes de bias/resampling vêm em commit separado.

### Gate de saída

- Com DDGI desligado, ReSTIR GI continua iluminando áreas multi-bounce após o warm-up do cache.
- Com fallback DDGI habilitado, sua taxa de uso cai conforme o cache aquece e estabiliza; a telemetria prova isso.
- Com cache confiável, desligar o update do DDGI não altera significativamente regiões já cobertas, exceto consumidores volumétricos.
- Não há leitura de DDGI no cache-update path.
- Não há dupla contagem: cache hit não soma novamente DDGI no mesmo hit.
- ReSTIR spatial/temporal não cria fireflies novos nem ghosting de iluminação acima do baseline.

## Fase 6 — Formalizar DDGI como fallback e manter volumetria

### Objetivo

Transformar a prioridade em configuração explícita do renderer e reduzir o custo do DDGI sem removê-lo.

### Trabalho

- Introduzir modos de alto nível, com nomes equivalentes a:

```cpp
enum class EIndirectPrimary {
    ReSTIR_SHaRC,
    DDGI,
    Off
};

enum class EIndirectFallback {
    DDGI,
    Environment,
    Black
};
```

- `UseGI` não deve continuar significando “DDGI ligado”. Ele significa que o domínio indireto está ativo.
- O deferred usa a saída de ReSTIR GI quando `ReSTIR_SHaRC` está ativo. O atlas DDGI não é somado como outra GI principal.
- Fog/volumetric scattering pode continuar lendo irradiância DDGI. Documentar essa exceção: é um consumidor volumétrico, não o estimador principal de superfície.
- Manter o DDGI aquecido quando ele for o fallback selecionado. Só depois medir redução de orçamento:
  - menos probes atualizadas por frame;
  - menor frequência de relight/update;
  - cascatas distantes/coarser;
  - prioridade para regiões de miss do cache, se houver um mapeamento barato.
- Se o custo do fallback continuar alto, avaliar o desenho do AC Shadows em change set próprio:
  - G-buffer persistente por probe;
  - trace/refresh de visibilidade separado do relight;
  - relight mais frequente que o trace completo;
  - metadado de escala de luminância para Time of Day em probes não selecionadas.
- Não expandir o DDGI da Smile para radiância direcional só para copiar o AC Shadows. Fazer isso apenas se um consumidor real justificar o custo; para fallback difuso/fog, irradiância continua sendo o produto adequado.
- Não introduzir fallback artístico diferente. Intensidade do DDGI fallback deve representar a mesma iluminação, sem multiplicador usado para mascarar diferenças.
- UI deve mostrar claramente:
  - primário;
  - fallback;
  - estado do cache: filling/active/resetting;
  - porcentagem de queries atendidas pelo cache;
  - porcentagem atendida por DDGI/ambiente;
  - custo de update/resolve e VRAM.
- Preservar “DDGI primary” como modo de rollback até o fechamento final.

### Gate de saída

- O modo padrão pode ser `ReSTIR_SHaRC + DDGI fallback` sem ordem implícita em flags desconectadas.
- O modo DDGI primary reproduz o baseline para rollback.
- Desligar o volume DDGI não impede setup, resize, denoise ou execução do ReSTIR GI.
- Fog continua estável em todos os modos; quando DDGI está ausente, usa fallback explícito em vez de descriptor inválido.

## Fase 7 — Qualidade, scheduling e otimizações inspiradas na idTech8/AC Shadows

### Objetivo

Reduzir custo e memória depois que o estimator estiver correto.

### Trabalho

- Substituir gradualmente o full-screen early-out do updater por geração/compactação de work items.
- Medir uma arquitetura de hit packets:
  - trace de visibilidade produz hit compacto, usando 128 bits como referência de orçamento da idTech8, não como requisito cego;
  - hash/deduplicação identifica células ativas;
  - shading acontece uma vez por célula/material quando isso for estatisticamente correto;
  - indirect dispatch por material evita megashader divergente;
  - aproximadamente 20 mil células ativas/frame e 14 MiB são números de referência da idTech8, não metas automáticas para a Smile.
- Essa otimização da idTech8 não deve ser aplicada se quebrar backpropagation ou introduzir bias não medido. Ela é um segundo produtor possível, não requisito da correção SHaRC.
- Medir update em async compute somente após definir dependências de TLAS, G-buffer, ReGIR, cache resolve e consumidores. Não colocar em compute por intuição.
- Considerar time slicing por tiles e prioridade de pixels em disoclusão/miss, inspirado no relight seletivo do AC Shadows.
- Medir uma hierarquia de consulta/trace barata antes do BVH completo:
  - screen-space trace validado para localizar/reaproveitar o hit secundário;
  - SHaRC/WRC para radiância world-space;
  - DDGI somente em miss/cache frio;
  - ambiente/zero quando DDGI não existir.
- O screen-space path é otimização, não fonte de verdade: rejeitar por depth/normal/material/motion e cair para BVH quando houver dúvida. ReSTIR deve continuar recebendo um ponto secundário geometricamente válido.
- Medir presets de capacidade `2^18`, `2^19`, `2^20` e `2^21`. `2^22` só entra com justificativa de cena e VRAM.
- Medir base cell/LOD a partir do estado conhecido `0,50 m / 6 m`; não voltar ao preset que saturou 95,2% da tabela.
- Se a variância for necessária, avaliar buffer auxiliar de luminância/segundo momento ou encoding compacto. A decisão precisa trazer delta de qualidade e MiB.
- Avaliar atualização adaptativa:
  - mais paths em disoclusão, cache miss, alta idade ou luz dinâmica;
  - menos paths em células convergidas;
  - teto global rígido para custo previsível.

### Gate de saída

- Otimização individual tem A/B de custo e qualidade; nenhuma “melhoria” entra apenas por reduzir ms numa câmera.
- Meta inicial de custo incremental para update + resolve: no máximo 1,5 ms em 1440p na GPU-alvo escolhida pelo projeto, ou orçamento explicitamente redefinido com evidência.
- O frame não apresenta picos periódicos de refresh/evicção.
- VRAM padrão do cache permanece próxima dos 36 MiB atuais, salvo justificativa medida.

## Fase 8 — Hardening do ReSTIR GI e denoising

### Objetivo

Adequar o reúso e o denoiser ao novo perfil de ruído sem mascarar erros do cache.

### Trabalho

- Manter direta e indireta em instâncias RELAX separadas. O capítulo do Cyberpunk mostra que os perfis de variância são incompatíveis para tuning conjunto.
- Confirmar que o pack indireto mantém diffuse GI e reflections/specular nos canais corretos, com hit distances coerentes.
- Fazer sweep controlado de:
  - reservoir max age, começando em 8;
  - M cap;
  - boiling filter;
  - temporal bias correction;
  - spatial count/radius;
  - firefly clamp;
  - history length do RELAX.
- Não usar clamp de firefly para esconder energia errada do cache. Primeiro localizar célula, path e fonte de fallback via debug.
- Se futuramente o mesmo reservoir transportar specular indireto, reduzir o raio espacial com roughness e desligar spatial abaixo de roughness `0.1`, como no Cyberpunk.
- Sem adotar ReSTIR PT, medir opcionalmente a unificação do trace inicial difuso/especular no estilo Cyberpunk: escolher um lobo por probabilidade de energia, corrigir o PDF da mistura e alimentar os pipelines/guide buffers correspondentes. Manter atrás de toggle e somente prosseguir se reduzir trace/VRAM sem contaminar o cache não direcional.
- Testar DLSS RR separadamente de NRD e None. Mudança de produtor exige reset dos histories correspondentes.
- Garantir que motion/disocclusion invalidem reservoir sem necessariamente limpar o cache world-space; teleport/troca de cena limpam ambos.

### Gate de saída

- Sequências com Time of Day, emissivo animado e luz móvel não deixam lag visível prolongado.
- Disoclusão não espalha célula brilhante em blob pelo pre-pass do denoiser.
- NRD, RR e None não leem recursos liberados ou históricos de outro modo.
- O modo cru continua disponível para avaliar o estimator antes do denoiser.

## Apêndice opcional — RTXDI 3.0 ReSTIR PT (fora do escopo padrão)

### Objetivo

Substituir ou complementar o ReSTIR GI de um ponto por resampling de paths completos somente se houver autorização explícita, hardware-alvo adequado e ganho que justifique a complexidade. O Opus não deve executar este apêndice como continuação automática das fases 0–9.

### Por que é uma fase separada

O `FReSTIRGI` atual armazena e reconecta um ponto secundário. ReSTIR PT grava informação de paths, faz random replay/hybrid shift e exige integração profunda com o path tracer via `RTXDI_PathTracerContext` e callbacks `RAB_*`. Isso não é um upgrade local do reservoir existente.

### Trabalho

- Integrar primeiro apenas o runtime RTXDI ao CMake, sem alterar a imagem.
- Usar o Full Sample local como referência para `RAB_PathTrace`; não inventar o contrato a partir do header.
- Implementar callbacks usando o mesmo material, luz, emissivo, ambiente, offsets e masks do path updater validado nas fases anteriores.
- Inserir a consulta SHaRC como terminal permitido do `RAB_PathTrace`, preservando o registro correto dos bounces no `RTXDI_PathTracerContext`.
- Fazer a migração em degraus:
  1. initial sampling sem reúso, comparado ao path updater;
  2. temporal replay;
  3. spatial/hybrid shift;
  4. boiling/bias correction;
  5. guide buffers e denoising;
  6. performance e memória.
- Manter `FReSTIRGI` atrás de toggle até o ReSTIR PT superar seus gates.
- Tratar mirror/PSR e Russian roulette conforme a documentação do RTXDI; não terminar estocasticamente bounces de mirror que alimentam guide buffers incompatíveis.
- Não compartilhar layouts de reservoir por conveniência. Criar recursos próprios do ReSTIR PT e contabilizá-los em VRAM.

### Gate de saída

- Sem reuse, o path tracer RTXDI produz referência equivalente ao path tracer comum.
- Temporal e spatial podem ser ligados separadamente e cada um possui captura de regressão.
- O ganho de estabilidade/qualidade é visível no sinal cru, não apenas depois do denoiser.
- Custo e VRAM cabem no preset alvo.
- Só então decidir se `FReSTIRGI` vira fallback legado ou é removido em outro change set.

## Fase 9 — Testes automatizados, documentação e mudança de default

### Testes de CPU

- Criar `SmileRadianceCacheMathTests` sem dependência do runtime D3D12 quando possível.
- Espelhar/testar:
  - cálculo de LOD e cell size;
  - quantização de posição e octante da normal;
  - determinismo do hash/checksum;
  - probing com vazio, colisão e tombstone;
  - média temporal com teto de 64;
  - idade, refresh e evicção;
  - composição de confiança.
- Centralizar constantes compartilhadas ou adicionar assertions para impedir drift silencioso entre C++ e HLSL.

### Testes de GPU/captura

- Câmera estática por pelo menos 128 frames.
- Travelling contínuo para testar LOD e churn.
- Teleport.
- Parede fina.
- Roughness sweep.
- Emissivo liga/desliga.
- Sol muda de direção/intensidade.
- Objeto dinâmico grande atravessa região iluminada.
- DDGI indisponível desde o setup.
- DDGI desligado durante execução.
- Resize, mudança de render scale e hot reload de HLSL.
- NRD, DLSS RR e None.

### Critérios finais

- Zero D3D12 validation errors, device removal, NaN/Inf ou descriptor inválido.
- SHaRC/WRC produz multi-bounce sem DDGI.
- DDGI aparece apenas nos contadores de fallback/volumetria quando o primário é SHaRC.
- Cache hit não paga gather DDGI.
- Cache miss possui fallback determinístico e observável.
- Mudança de cena/teleport não vaza histórico.
- Direta permanece equivalente ao baseline ReSTIR DI.
- Indireta crua melhora ou mantém estabilidade em relação ao baseline dentro do orçamento definido.
- O modo DDGI primary continua funcional por pelo menos um ciclo de estabilização.
- Somente após todos os gates, mudar o default para `ReSTIR_SHaRC + DDGI fallback`.

## Sequência recomendada de commits

1. `gi: add reproducible SHaRC/DDGI A-B modes and counters`
2. `rt: move shared instance bindings out of DDGI ownership`
3. `gi: decouple ReSTIR GI and indirect NRD setup from DDGI readiness`
4. `gi: split hit shading from indirect fallback policy`
5. `gi: add sparse radiance-cache path update pass`
6. `gi: backpropagate multi-bounce radiance into WRC`
7. `gi: add cache confidence, warm-up state and miss telemetry`
8. `gi: make WRC the primary ReSTIR GI terminator`
9. `gi: formalize DDGI as surface fallback and volumetric provider`
10. `gi: tune ReSTIR/RELAX histories for SHaRC signal`
11. `gi: compact and schedule radiance-cache updates`
12. `tests: add radiance-cache math and regression coverage`
13. Apêndice opcional, somente se autorizado: `rtxdi: integrate ReSTIR PT runtime and initial sampling`

O implementador pode dividir mais, mas não deve fundir os commits 2–8 em um único change set.

## Comandos mínimos de validação por fase

```powershell
cmake --build build --config Debug --target Shaders
cmake --build build --config Debug --target SmileEditor
ctest --test-dir build -C Debug --output-on-failure
```

Quando a fase mexer apenas em shader, ainda compilar `SmileEditor` se houver mudança de cbuffer, descriptor table, enum ou settings bridge.

## Pacote que o Opus deve entregar para validação

Ao fechar cada fase, enviar:

- resumo do que mudou e do que deliberadamente não mudou;
- lista de arquivos tocados;
- `git diff --stat` e diff relevante, sem incluir/reverter alterações preexistentes do usuário;
- comandos executados e resultado;
- GPU timings antes/depois;
- VRAM antes/depois;
- valores dos novos contadores;
- capturas cruas e denoisadas nas cenas de regressão;
- limitações conhecidas;
- qual gate da fase ainda não foi provado.

Se um gate falhar, corrigir dentro da mesma fase. Não avançar acumulando dívida estatística ou descriptor/resource lifetime bugs.

## Referências de implementação

- `Fast as Hell — idTech8 Global Illumination`: hash world-space compacto, deduplicação antes do shading, work lists por material e forte controle de orçamento.
- `Raytracing the world of Assassin's Creed Shadows`: atualização temporal seletiva, separação entre dados persistentes e relight, DDGI robusto como cobertura ampla e suas limitações de leaking/distribuição uniforme.
- `The Evolution of the Real-Time Lighting Pipeline in Cyberpunk 2077`: combinação ReSTIR GI + SHaRC, update em aproximadamente 4% dos pixels com até quatro bounces, resolve temporal, terminação por voxel/cone e separação de denoising direto/indireto.
- `D:/Engines/RTXDI-main/Doc/RestirGI.md`
- `D:/Engines/RTXDI-main/Doc/RestirPT.md`
- `D:/Engines/RTXDI-main/Samples/FullSample`: referência obrigatória somente se o apêndice ReSTIR PT for autorizado.

