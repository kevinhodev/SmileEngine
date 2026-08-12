# Auditoria do DDGI e estado da invalidação

Estado em 11 de agosto de 2026. Este documento registra a revisão completa do DDGI
cruzada com o paper original (Majercik et al., JCGT 2019), o código-fonte local da Flax
e do RTXDI 3.0, mais o SDK RTXGI de memória. Registra o que as fases 1 a 4 corrigiram, o
resultado dos A/Bs, e a fila do que ficou — incluindo a decisão estratégica em aberto.

Escrito no fim de uma sessão longa, para a próxima começar do ponto certo.

## Veredito

O núcleo do algoritmo está correto e é fiel ao paper: direções Fibonacci rotacionadas
por frame E por probe, atlas octaédrico com borda de 1px e fold, momentos de distância,
Chebyshev com os pisos defensivos, relocação, e realimentação de múltiplos quiques. A
convenção de energia (`E/π`) é coerente com o resto da engine. O desenho
`DDGI → RadianceCache → ReSTIR GI` também faz sentido, com o DDGI como terminador
estável.

**Os defeitos estavam todos na periferia:** inicialização, invalidação de histórico e
distribuição espacial. Nenhum no ray tracing nem na matemática do gather.

E uma causa-raiz atravessa quase tudo: **o espaçamento sai da AABB da cena inteira**
(`spacing = maxExt/23`), o que dá 8,02 m no Bistro. Dela derivam, em cadeia:

- o bias de auto-sombra valia 1,20 m (`0.75 · spacing · 0.2`) e precisou de um teto
  absoluto de 0,40 m, que o RTXGI moderno resolve com bias em metros;
- a gaiola incompleta na borda positiva do eixo dominante (abaixo);
- a precisão dos momentos em `R16G16_FLOAT`: o erro relativo da variância é
  `~M²·3·2⁻¹¹`, ou seja o Chebyshev fica cego a oclusores mais próximos que ~2–5% da
  distância média — 25–30 cm a 8 m de spacing, espessura de parede;
- nenhuma separação entre cômodos (o paper pede "at least one full cage per room-like
  space" e recomenda 1–2 m para escala humana; a Flax usa 1 m na cascata 0);
- 64 raios espalhados por uma célula de 8 m.

## O que foi feito

### Fase 1 — resposta temporal

- **Reset one-shot no `SetupForScene`** (`DDGI.cpp`). Os três recursos são zerados ali e
  o primeiro update precisa SUBSTITUIR, não misturar. Sem isso o volume convergia a
  `0.99ⁿ` a partir do preto: ~300 updates, 5 s a 60 fps, com a cena nascendo sem
  indireto e clareando sozinha. A relocação NÃO cobria o caso: ela só marca
  `wasInactive || bigJump`, e pós-setup o `ProbeData` está zerado (w = 0, não é
  "inativo") e a sonda em ar livre não se move.
- **Teto de 0,98 na histerese estável.** O paper (§4.4) usa α entre 0,85 e 0,98 e a Flax
  grampeia em 0,98. `SetHysteresis` clampa em `[0, 0.98]`.
- **Histerese própria do atlas de distância (0,99).** Ver "constantes" abaixo.
- **Detector adaptativo por sonda** (`DDGI_AdaptiveHysteresis`, `DDGICommon.hlsli`): o
  update mede quanto a sonda mudou e derruba a histerese dela sozinho, como rede da
  invalidação por evento. Redução por SONDA (36 texels, `InterlockedAdd` em
  groupshared), banda morta no numerador, resposta limitada a 0,90 em sonda enterrada,
  e só REDUZ. **Default OFF** — ver A/Bs.

### Fase 2 — encanamento espacial

- **Padding cumulativo corrigido.** `InvalidateRegion` guardava a caixa JÁ padded, unia
  com a próxima e padava de novo: a região crescia um spacing por lado a cada chamada.
  Agora a união é crua e a folga entra uma vez no envio (`UpdatePerFrame`). É isso que
  torna seguro chamar por frame durante um gesto.
- **Eventos ligados**, todos com união da caixa antiga e da nova (duas chamadas — o
  `FDDGI` une):
  - gizmo: mover objeto e mover luz;
  - outliner: o olho (mesh e pasta; a instância sai mesmo da TLAS,
    `RaytracingScene.cpp` pula `!Visible`);
  - luz: cor, intensidade, raio, cone, direção, on/off, posição por campo, `RTWeight`,
    e as ações de lista (add, remove, duplicate, toggle da linha, `placeAtCamera`).
- **`SetCastShadows` NÃO invalida, e é de propósito**: a montagem do `FGPULightGI` não
  lê esse campo — o hit do indireto sempre dispara shadow ray.
- **`HistoryDomain::SceneContent`** (novo domínio): visibilidade de objeto e propriedade
  de luz derrubam reservoirs, ReGIR, reflexões, NRD direto e cache de radiância — mas
  **não** o `DDGIAtlas` (quem chama invalida por região) nem `TemporalMotion`/`HiZ`
  (indexados por índice, e a lista não andou). Coalescido via `MarkSceneContentDirty`.
- **`SetRTWeight` mudou de caminho**: era `MarkIndirectLightingDirty` → `SkyRadiance` →
  que carrega `DDGIAtlas` → reset global do atlas a cada tique do slider. Agora é
  região + `SceneContent`, que alcança mais caches e não pisca.
- Mover objeto NÃO chama `MarkSceneContentDirty`: é gesto contínuo e os históricos de
  tela reprojetam por motion vector. Derrubar reservoir por frame seria reset permanente
  durante o arraste.

### Fase 3 — coerência entre os dois atlas

- **`DDGI_RegionalHysteresis`** (`DDGICommon.hlsli`), compartilhado pelos dois passes de
  update, com a **posição relocada** da sonda. Se um testasse a caixa pelo vértice do
  grid e o outro pela relocada, a sonda que a relocação move — a encostada em parede,
  justamente a que mais importa — poderia entrar num atlas e ficar de fora do outro.
- **O atlas de distância passou a consumir a caixa**, com histerese e **janela
  próprias** (dois contadores; a caixa é uma só, e a união persiste enquanto qualquer
  uma das duas janelas estiver aberta). Antes disso a iluminação respondia e a
  visibilidade não — luz nova pesada por visibilidade de uma cena que não existe mais.
- O cbuffer do `DDGIUpdateDist` passou a ser declarado até o fim (`MiscParams3`).

### Fase 4 — a medição da folga, e os knobs que não faziam nada

Nenhum item desta fase muda a imagem na configuração padrão. Ela existe para tirar a
decisão estratégica do campo da opinião e para limpar o que estava morto.

- **Bracket de stall no wait** (`Renderer.cpp`, no `if (GIComputeFence != 0)`). Par de
  timestamps na fila DIRETA em volta do `SubmitSegmentAndContinue` + `GpuWait` — o
  primeiro fecha o segmento do G-buffer, o segundo só executa depois que a fence liberou,
  e os dois estão na MESMA base de tempo. Aparece no profiler como
  **"Espera do DDGI (async)"**. Um escopo do `FGpuProfiler` atravessa a fronteira de
  segmento sem problema: a lista é a mesma (o `SubmitSegmentAndContinue` fecha, executa e
  reabre), o query heap é do device, e o `Resolve` do fim do frame pega os dois índices.
- **`AdaptiveRays` deixou de ser inerte.** Duas correções, porque eram dois defeitos:
  - `TriggerReclassify` perdeu a guarda `Relocation &&`. Quem escreve o `ProbeRayCount` é
    o passe de relocação, então com a relocação desligada os três setters não chegavam a
    lugar nenhum;
  - o `DDGIRelocate.cs.hlsl` deixou de sair cedo quando a relocação está desligada. As
    duas responsabilidades moram nele por CONVENIÊNCIA (as duas querem a mesma varredura
    dos 64 hits), não por dependência: agora ele classifica sempre e só o deslocamento
    depende do toggle.
  - `SetMaxRays`/`SetMinRays` clampam em `kRaysPerProbe` **e arredondam para baixo na
    potência de 2**. Aceitar 256 era ficção (o trace tem um thread por raio e decima com
    `stride = 64/rayCount`, que só REDUZ); e como a divisão é inteira, só potência de 2 é
    exata — pedir 48 dava stride 1 e traçava os 64. `MinRays > MaxRays` é resolvido no
    empacotamento (`min` dos dois), porque invertidos o `clamp` do HLSL devolve o MAX em
    silêncio.
  - **Reclassificação pós-edição**, coalescida no fechamento da janela de invalidação e
    **restrita a mudança de GEOMETRIA** (`EGIRegionChange`). A classificação (sonda
    inativa e contagem de raios) congela quando a relocação converge, então uma edição de
    cena a deixa velha: parede removida = sonda que fica com a contagem de espaço fechado;
    objeto novo = sonda engolida que ninguém mais marca inativa — caso que o comentário da
    sonda enterrada no `DDGIUpdate` já lamentava. Duas decisões de desenho, e as duas
    vieram de erros da primeira versão:
    - `InvalidateRegion` só MARCA; quem dispara é o `RecordUpdate` quando as duas janelas
      fecham. Disparar na hora prenderia o update no caminho síncrono durante todo um
      arraste de gizmo (que invalida por frame) e seguraria o trace nos 64 raios junto.
    - Luz não reclassifica. A primeira versão marcava em TODA invalidação, então mexer na
      intensidade de uma luz agendava 6 frames de reclassificação global, síncronos e com
      64 raios, ~69 frames depois de soltar o slider — um hitch atrasado o bastante para
      ninguém ligar ao que acabou de fazer, e para nada: nenhuma sonda mudou de lugar nem
      passou a enxergar coisa diferente. `EGIRegionChange` não tem default de propósito:
      "radiométrico" faria o call site esquecido reintroduzir a classificação velha em
      silêncio, "geometria" faria ele pagar o hitch. Obrigar a declarar é a única opção
      que não escolhe um dos dois erros por omissão.
  - Exposto em Configurações → GI ("Raios adaptativos"), domínio `GIAccumulation` — sem o
    clear, o lado ligado do A/B começaria com sondas convergidas a 64 raios. Sem isso o
    knob continuaria intestável, que é a mesma coisa que morto.
  - Com o toggle OFF o comportamento é bit a bit o de antes: `EffMax = EffMin =
    kRaysPerProbe` faz o `DDGI_DesiredRays` clampar em 64 e a classificação virar
    identidade.
  - ⚠️ **Catraca do classificador, achada e fechada na mesma fase.** O classificador mede
    a proximidade pelo hit mais próximo do trace do frame; se a sonda já estivesse
    decimada, esse mínimo sairia de uma amostragem angular mais grossa e viria viesado
    PARA LONGE — a sonda cairia mais um degrau, e outro. Como a relocação classifica 180
    frames SEGUIDOS no começo da cena, o grid inteiro escorregaria até `MinRays` e o A/B
    mediria o escorregamento em vez do knob. Fechado com `MiscParams3.z`: nos frames em
    que o passe de classificação roda, o trace ignora a contagem e manda os 64. O
    classificador decide sempre sobre o conjunto cheio.
- **O teto do grid deixou de ser um número por eixo.** `kMaxPerAxis` era 32 e nunca mordia
  (o spacing sai de `maxExt/(kTargetMax-1)`, então nenhum eixo passa de 24) — era por não
  morder que podia mentir. A primeira tentativa foi um `static_assert` de `kMaxPerAxis³`,
  e ela estava ERRADA: supõe cena cúbica, reprovaria grid que cabe folgado, e teria
  travado o próprio sweep de `kTargetMax` em 25. O que valeu foi `gridFits`, uma checagem
  em RUNTIME dos três produtos reais (ver a tabela em "Custo"), que **abre o espaçamento
  até caber** e loga. Antes disso, subir `kTargetMax` criava textura inválida em silêncio.
- **`SetIntensity` ganhou piso positivo.** Zero é SENTINELA no cbuffer, não valor — o
  Renderer manda `DDGIParams.x = 0` para dizer "GI desligado" e os cinco consumidores leem
  `(x > 0) ? x : 1.0`. Sem o piso, um slider em 0 viraria intensidade 1.
- **O comentário do piso `+0.05` parou de mentir.** Conferido no fonte da Flax
  (`DDGI.hlsl:214`): lá o backface é `Square(dot(...) * 0.5 + 0.5)` puro. Os pisos que SÃO
  da Flax (Chebyshev 0.05, peso final 1e-6, trilinear 0.001, curva de crush) estão
  identificados como tais. O somando continua no código — o A/B dele segue na fila.

### Fase 5 — reempacotamento dos atlas (o teto de 1024 colunas)

**O problema:** os dois atlas eram uma FILEIRA de tiles — coluna = `x + z·countX`, linha = `y`
— então a LARGURA crescia com `countX·countZ`. No atlas de distância (stride 14+2) isso
estourava o limite de dimensão de Texture2D com 1024 colunas de sonda. O Bistro já usava
552: **uma segunda cascata não caberia.** O `ProbesTrace` tinha o mesmo vício em outra
dimensão — uma linha por sonda, ou seja `NumProbes` era a ALTURA e o grid parava em 16.384.

**A correção:** grade 2D em vez de fileira.

- `DDGI_TileOrigin` passou a enrolar o índice em `tilesPerRow` colunas. O índice agora é o
  próprio `DDGI_ProbeLinear`, o mesmo dos buffers — antes o atlas tinha ordem própria (X,
  depois Z, com Y nas linhas) porque a fileira única exigia X e Z juntos na largura, e
  existia uma função só para traduzir entre as duas ordens. `AtlasTileFromProbe` virou
  identidade.
- `DDGI_TraceTexel` faz o mesmo no `ProbesTrace`, com `kTraceProbesPerRow = 256` fixo —
  `256 × 64 raios = 16384` texels é exatamente a largura máxima, então é o maior valor que
  cabe e não há o que calibrar.

**`tilesPerRow` não é campo de cbuffer, e isso é a decisão de desenho da fase.** Ele é
recuperado da LARGURA (`DDGI_TilesPerRow(atlasW, tile)`), que todo consumidor do gather já
carrega ao lado do tamanho do tile. A largura É `tilesPerRow·(tile+2)` por construção no
`SetupForScene`, então a divisão inteira é exata dos dois lados e **não existe estado
duplicado para dessincronizar** — o que importa porque um `tilesPerRow` errado não falha,
desalinha o atlas inteiro em silêncio. Pelo mesmo motivo as quatro dimensões saem das
mesmas duas variáveis num lugar só, e não de quatro expressões independentes como antes.
Os dois atlas compartilham o `tilesPerRow` (o índice do tile é o da sonda), então derivar
de qualquer um dá o mesmo número.

**Números do Bistro** (`4416` sondas), grade `67 × 66`:

| atlas | antes | depois | texels |
|---|---|---|---|
| irradiância | 4416×64 | 536×528 | +0,14% |
| distância | 8832×128 (69:1) | 1072×1056 (≈1:1) | +0,14% |
| ProbesTrace | 64×4416 | 16384×18 | +4,3% |

Memória praticamente igual; o que mudou foi a FORMA.

**O teto, com os números certos.** Uma primeira versão desta seção anunciou "~1 milhão
contra ~24 mil", e os dois estavam errados:

| | teto de sondas | quem morde |
|---|---|---|
| antes da fase 5 | 16.384 absoluto; **~9.920 no Bistro** | `ProbesTrace` (altura = `NumProbes`) no absoluto; o atlas de distância (`CountX·CountZ ≤ 1024`) antes disso, na prática |
| fase 5 só com os atlas | **65.535** | o `Dispatch`, não os recursos |
| com o dispatch 2D | **~1.048.576** | atlas de distância (`tilesPerRow` e linhas ≤ 1024) |

As "~24 mil" saíam de `1024 colunas × 24` de `CountY` e ignoravam que o `ProbesTrace`
travava em 16.384 antes disso.

**A linha do meio é a que quase passou em branco:** três passes são *uma sonda por grupo*
(`Dispatch(NumProbes,1,1)` em trace, update e updateDist) e o D3D12 para em 65.535 grupos
por dimensão. Os recursos comportariam 1 milhão, o `gridFits` aprovaria, as texturas seriam
criadas — e só o `Dispatch` ficaria inválido, em runtime e longe da causa. Corrigido com
grade 2D de grupos (`DDGI_ProbeFromGroup`), com a largura da grade derivada de `numProbes`
por aritmética inteira, exatamente como o `tilesPerRow` sai da largura do atlas e pelo mesmo
motivo. Ela é escolhida para dividir `numProbes` quase exato — no Bistro, `884 × 5 = 4420`
grupos para 4416 sondas, quatro de sobra. O `gridFits` passou a checar o dispatch junto com
os recursos: teto anunciado sem quem o sustente foi o defeito, e a checagem é onde ele não
volta.

**Como validar: a imagem tem de ficar IDÊNTICA.** Isto é re-endereçamento puro — nenhum
valor de irradiância, visibilidade ou peso mudou. Se algo estiver mal endereçado o GI vai
visivelmente para o lixo (preto ou salpicado), não sutilmente. O que precisa de medida é
outra coisa: **banda**.

### A regressão de banda, e o que ela ensinou (11/08/2026)

A primeira versão do reempacotamento enrolava o **índice linear da sonda**
(`DDGI_ProbeLinear`) — mais simples, e unificava a ordem do atlas com a dos buffers a ponto
de `AtlasTileFromProbe` virar identidade. O sweep depois dela:

| `kTargetMax` | sondas | espera |
|---|---|---|
| 24 | 4.416 | ~0,02 ms |
| 28 | 6.804 | ~0,02 ms |
| 30 | 8.700 | ~0,03 ms |
| 32 | 9.920 | **0,45 ms** |

Antes da fase 5 as mesmas 9.920 sondas ficavam no piso. **A reforma custou ~0,4 ms**, e a
causa é geométrica: o gather lê OITO sondas vizinhas, e no layout de fileira as quatro de
`(x..x+1, y..y+1)` caíam num **bloco 2×2 de tiles adjacentes** — vizinhas na horizontal *e*
na vertical. Enrolar o índice linear mantinha só o par em `x` junto e jogava `y` a `countX`
tiles de distância. A propriedade não era acidente do layout antigo; era o que fazia o
gather ser barato.

**Correção: o plano `(x,z)` volta para as colunas e o `y` para as linhas, com o plano
enrolado em BANDAS** quando não cabe numa linha. Com uma banda é bit a bit o layout
histórico; com várias, o teto continua sendo o produto (~1 milhão). A vizinhança 2×2 volta
inteira.

No Bistro o resultado é exato, sem desperdício: `TilesPerRow = 69` (o `SetupForScene`
prefere um divisor de `CountX·CountZ = 552` perto da raiz de `NumProbes`), 8 bandas, grade
`69 × 64`. Atlas de distância `1104 × 1024` — **os mesmos 1.130.496 texels de antes da
reforma**, agora em proporção 1,08:1 em vez de 69:1.

Preço: `AtlasTileFromProbe` deixa de ser identidade e ganha uma inversa
(`ProbeFromAtlasTile`), as duas no `FDDGI` para o editor e o renderer não duplicarem a
conta. Vizinhança 2×2 vale duas funções.

**Resultado (11/08/2026): com o layout em bandas, `kTargetMax = 32` — as mesmas 9.920
sondas que custavam 0,45 ms — voltou a oscilar entre 0,02 e 0,03 ms, ou seja ao piso.** A
regressão era inteiramente da ordem dos tiles. O `ProbesTrace` está inocente e
`kTraceProbesPerRow` fica em 256 sem A/B.

**Segunda lição, sobre `kTraceProbesPerRow`:** este documento afirmou que 256 "é o maior
valor que cabe, então não há o que calibrar". Isso **confunde capacidade com desempenho** —
`256×64 = 16384` enche a largura máxima, mas isso é um argumento sobre o que cabe, não
sobre o que é rápido. Continua sendo um knob (64 = largura 4096, 128, 256), agora sem
motivo conhecido para mexer. O `static_assert` era `==` e proibia justamente esse A/B;
virou `<=`.

**O que a fase 5 entrega, fechado:** teto de ~24 mil (na prática 9.920 no Bistro) para
~1 milhão de sondas, **sem custo de tempo nem de memória** — o atlas de distância tem
exatamente os mesmos 1.130.496 texels, e 9.920 sondas seguem escondidas no overlap como
antes da reforma.

⚠️ **O número que nunca foi capturado é o FRAME DE GPU.** "Escondido no overlap" quer dizer
que a fila direta não espera — não que o trabalho seja grátis: o compute assíncrono divide
SMs e banda com o raster. `kTargetMax = 32` está *provado* caber na janela de overlap;
se vale gastar (2,25× o atlas em VRAM, ~2,25× o compute) é outra decisão, e ela precisa do
frame de GPU, não da espera. Default segue em 24 até a cascata, que é o caminho certo para
chegar a 1–2 m de espaçamento.

Conferir também no visualizador: o clique no atlas para selecionar sonda
(`ViewportWidget`) e o tile ampliado (`Renderer`) endereçavam pela ordem velha e foram
migrados junto; o `DebugView.ps` já derivava tudo da largura da textura e não precisou
mudar.

**Roteiro de validação:**

1. `kTargetMax = 24`, conferir a imagem — tem de ser idêntica.
2. Abrir os dois atlas no visualizador e clicar em sondas do começo, do meio e do fim da
   grade (o fim é onde a linha incompleta mora).
3. `kTargetMax = 33` — agora tem de passar **sem** o aviso do `gridFits`, porque o teto
   que o disparava era o do layout antigo.
4. Refazer o sweep, anotando `DDGI (async)`, a espera e o frame de GPU.

## Gate de medição

`FGIHitSampling::TerminatorOff` — zera o termo indireto do `ShadeSurfaceHit` com todo o
resto rodando (volume traçado, sondas relocando, atlas atualizando). Empacotado no bit 2
de `GIDistParams.w`, ao lado do `skipMode`, porque esse float já chega por contrato de
NOME aos cinco shaders que incluem o `HitShading` — custou um bool, um accessor, três
sites de empacotamento e duas linhas de shader, em vez de um float4 novo em cinco
cbuffers. Com o gate ligado o `volW` vai a zero antes do gather, então o custo do tap
também sai e a medição é honesta no profiler.

Toggle em Configurações → GI ("Cortar o terminador"). **Não é knob de qualidade.**

## Constantes, e por que cada uma

O critério do par (histerese, janela) é UM: quanto do histórico pode sobrar quando a
janela fecha (`kInvalidateResidual = 3%`). Fixado o residual, a histerese vira um knob
de VARIÂNCIA. A janela é **derivada** por `DDGIFramesForResidual` — trocar a histerese
ajusta as duas, e um `static_assert` prende a janela em [4, 128].

O número de amostras efetivas de uma EMA é `(1+h)/(1-h)`, e a amplitude de flicker vai
com `1/√N`.

| onde | histerese | janela | amostras efetivas |
|---|---|---|---|
| irradiância, base | 0,98 | — | 99 |
| irradiância, regional | 0,90 | 34 | 19 |
| distância, base | 0,99 | — | 199 |
| distância, regional | 0,95 | 69 | 39 |

**Por que a distância é sempre mais alta:** com o expoente 50 do `DDGIUpdateDist` o lobo
de cada texel tem 9,51° a meio peso (`0.5^(1/50) = cos 9,51`), ou 0,69% da esfera. Com
64 raios isso dá **~0,44 raio esperado por texel por frame** — o texel guarda, na
prática, a distância do raio que por acaso caiu mais perto, e as direções giram todo
frame. Lá a mistura temporal não é atraso, é a RECONSTRUÇÃO do estimador. Descer junto
com a irradiância cortaria pela metade as amostras que formam os momentos do Chebyshev,
cuja precisão já é o gargalo.

O par regional da irradiância era **0,75 / 12 e foi REPROVADO em A/B**: 7 amostras
efetivas não seguram a variância de um estimador de 64 raios, e mover um objeto fazia a
região dele piscar — com o detector ligado ou desligado.

## Resultados de A/B (11/08/2026)

1. **Mover objeto, detector OFF** — o flicker sumiu com o par 0,90/34 + fase 3. ✅
2. **Detector ON, recalibrado (0,50/0,80)** — o flicker sumiu. ✅ Segue default OFF
   porque a versão recalibrada não tem A/B próprio e o papel dele encolheu (ver fila).
3. **Gate do terminador, Bistro, ReSTIR GI OFF** — delta visual **grande**: a rua
   inteira em sombra passa de "crushed" a legível. O custo mal se moveu.

### Como ler o resultado 3 — as ressalvas importam

O que ele prova, na formulação estreita:

- **multi-bounce é essencial nesta cena**, e o DDGI atual cumpre esse papel;
- o **gather do terminador tem ótimo custo/benefício**: delta visual grande, custo
  marginal pequeno.

O que ele **não** prova:

- que só o DDGI possa cumprir o papel;
- que o sistema DDGI inteiro seja barato — o gate mantém os ~283 mil raios, o traversal,
  os shadow rays e os dois updates de atlas rodando. Ele só pula o gather de 8 sondas.

Ressalvas de método na captura: **reflexões estavam ON** (`UseReflections = true`), e o
gate também corta o DDGI nos hits de reflexão/espelho/água — o isolamento perfeito da
realimentação exigiria capturar com reflexões e água desligadas. O **radiance cache
estava OFF** (`RadianceCache::Enabled = false`, opt-in).

E uma tentação a evitar: o delta de 1,5–1,8× que a série `1/(1−ρ)` prevê **não é uma
predição válida aqui**. Aquele colapso do operador de transporte num escalar só vale em
cavidade difusa fechada e uniforme; o Bistro tem escape pelo céu, albedos diferentes e
oclusão. A coincidência numérica é sanity check fraco, e não prova nem desmente leak.

## Custo (PIX, 11/08/2026)

- Bloco de compute do DDGI: **2,117 ms** (7930,919 → 7933,036 ms), `API Queue 1`.
- Só o update do DDGI vai para a fila de compute (`Renderer.cpp`, `UseAsyncCompute &&
  DDGI.CanRunAsync()`); a fila direta espera por ele DEPOIS do G-buffer.
- Grid do Bistro: `23x8x24 = 4416 sondas, spacing 8,018 m` → ~283 mil raios/frame.

⚠️ O período do frame (~7,4 ms) **não** é a janela de overlap: o compute tem de terminar
antes do **wait pós-G-buffer**, não antes do fim do frame. A folga real é
`wait − fim do compute`, e é menor que a folga do período. Medir pelo período
superestima o orçamento.

O bracket de stall da fase 4 é o instrumento (linha **"Espera do DDGI (async)"** no
profiler). Ele não mede folga POSITIVA — quando o compute já acabou, o valor fica no piso
do instrumento e não diz por quanta margem. Mede o que decide: **suba a contagem de sondas
até o número subir de forma sustentada acima desse piso** (~0,02 ms no Bistro; ver o
protocolo do sweep abaixo). Enquanto estiver no piso, cabe mais sonda.

### Resultado da medição (11/08/2026) — o grid de hoje se esconde por inteiro

**Bistro, câmera parada, Release: "Espera do DDGI (async)" = 0,02 ms (0,2%).**

Leitura correta: **stall indistinguível do overhead.** 0,02 ms não é "0,02 ms de espera" —
é o custo mínimo de fechar o segmento, sinalizar e reenviar a fila, que existe mesmo com o
compute já terminado. O que se prova com isso é uma coisa só, e vale a pena enunciá-la
estreita:

> **`4416 sondas × 64 raios` cabem inteiramente escondidos** atrás de sombras + depth
> prepass + G-buffer, custando ~0 ms de tempo de frame.

⚠️ Uma versão anterior deste documento dizia que "~2,5× mais sondas caberia no orçamento
de tempo" **antes de medir** — aquilo era o teto de MEMÓRIA (a tabela abaixo) travestido
de conclusão de tempo. Ficou retratado; o sweep abaixo é que provou o número, e por outro
caminho.

### O sweep (11/08/2026) — 2,25× cabe, e o teto de tempo segue desconhecido

Valores reais do log de boot, não estimativas:

| `kTargetMax` | grid | sondas | espaçamento | espera |
|---|---|---|---|---|
| 24 (default) | 23×8×24 | 4416 | 8,018 m | no piso |
| 28 | 27×9×28 | 6804 | 6,830 m | no piso |
| 32 | 31×10×32 | **9920** | 5,949 m | no piso |
| 33 | 30×10×32 | 9600 | 6,051 m | no piso |

Em 33 o `gridFits` disparou, como previsto:

```
DDGI: espacamento de 5.763077 m nao cabe nos atlas; aberto para 6.051231 m
```

Com `CountZ = 33` o produto `CountX·CountZ` bateria em 1056 contra o teto de 1024 do atlas
de distância, e o guarda abriu o espaçamento — **33 entrega MENOS densidade que 32**. O
melhor ponto é 32.

**O que ficou provado:** `9920 × 64 ≈ 635 mil raios/frame`, **2,25× o grid original**,
seguem inteiramente escondidos no overlap — a espera não saiu do piso em nenhum degrau.

**O que continua desconhecido: o teto de TEMPO.** Ele não foi encontrado porque o limite
de layout do atlas chegou primeiro. Só dá para achá-lo depois da reforma do empacotamento
— e a reforma muda padrão de acesso e bandwidth, então **o sweep terá de ser refeito
depois dela**, não reaproveitado.

**Protocolo, para repetir**: `Adaptive Rays` OFF (64 raios fixos), mesma câmera e
resolução, Release, recarregar a cena a cada valor, não editar nada durante a medida,
esperar a relocação convergir mais ~30–40 frames. Anotar por ponto: sondas e espaçamento
**do log** (o `gridFits` pode ter aberto o espaçamento e a densidade pedida não ser a
entregue), `DDGI (async)`, `Espera do DDGI (async)` e o frame de GPU. Dois cuidados, que
são os jeitos fáceis de errar:

- **O piso não é zero, é ~0,02 ms.** O critério é subida SUSTENTADA acima dele — algo
  estabilizado em ~0,05 ms é sinal; oscilação entre 0,02 e 0,03 não é.
- **Espere ~30–40 frames por degrau.** O `FGpuProfiler` suaviza por EMA de 0,1
  (`GpuProfiler.cpp`) e mostra duas casas; ler antes disso mostra o meio da rampa.

### Mas o teto não é o tempo — é a DIMENSÃO DE TEXTURA (e é mais apertado do que a auditoria dizia)

Ao conferir quanto dá para adensar, apareceu um limite estrutural que a entrada "limite
latente" desta auditoria descrevia pela metade. Três recursos escalam com o grid, e cada
um bate num limite diferente de 16384:

| recurso | dimensão que cresce | restrição | uso no Bistro hoje |
|---|---|---|---|
| `ProbesTrace` | altura = `NumProbes` | `NumProbes ≤ 16384` | 4416 |
| `IrradAtlas` | largura = `CountX·CountZ·(6+2)` | `CountX·CountZ ≤ 2048` | 552 |
| `DistAtlas` | largura = `CountX·CountZ·(14+2)` | **`CountX·CountZ ≤ 1024`** | 552 |

**Quem morde primeiro é o atlas de DISTÂNCIA, não o `ProbesTrace`.** Com tile de 14+2 ele
estoura com 1024 colunas de sonda — metade do que a irradiância aguenta, e o `ProbesTrace`
nem chega perto. A auditoria só tinha anotado o `ProbesTrace`.

O que isso significa em números, para o Bistro (`maxExt ≈ 184 m`, Z dominante):

- teto ≈ **`kTargetMax` 32–33**, ou seja **espaçamento ~5,8 m** contra os 8,0 m de hoje;
- ali o grid fica em ~31×11×33 = ~11,3 mil sondas (720 mil raios, ~2,5× o de hoje), e o
  `ProbesTrace` estaria em 11,3 mil de 16384 — sobrando.

**A conclusão que importa para a decisão estratégica:** mesmo levando o grid ao limite
absoluto de textura, o espaçamento vai de 8,0 para ~5,8 m. O paper pede 1–2 m para escala
humana e a Flax usa 1 m na cascata 0. **Um volume uniforme não chega lá no Bistro — falta
um fator de 3 a 6, e o limite não é tempo de GPU, é layout de atlas.** Adensar o volume
único está fora de questão como solução; ou a cascata fina (que cobre um subconjunto do
mundo, então 1 m cabe em poucas sondas), ou reformar o layout do atlas de distância
(empacotar em 2D em vez de uma fileira de colunas) antes de qualquer coisa.

Guarda em runtime (fase 4): `gridFits` verifica os três produtos reais e **abre o
espaçamento até caber**, com `LogWarning`. Não dava para checar isso em tempo de
compilação sem supor cena cúbica — `kTargetMax³` seria o pior caso e reprovaria grid que
cabe folgado. Antes disso, subir `kTargetMax` criava textura inválida em silêncio.

Alternativas, se algum dia a folga positiva importar:

1. **PIX/Nsight**, lendo a distância entre o fim do bloco de compute e o wait. Zero
   código, mas manual.
2. `GetClockCalibration` no profiler — correto e caro; só se quiser a timeline
   permanente das duas filas. É o que falta hoje: há `GetTimestampFrequency` por fila mas
   nenhuma calibração, então os ticks da direta e os da compute estão em bases de tempo
   não correlacionadas e subtrair um do outro não significa nada. É por isso que o
   bracket precisou ser dos DOIS lados na mesma fila.

## Fila do que ficou

### Da auditoria original, ainda aberto

- **Gaiola incompleta na borda positiva do eixo dominante.** `spacing = maxExt/23`,
  count 24, `gridMin = AABBMin − 0.5·spacing` → a última sonda cai em
  `AABBMax − 0.5·spacing`, e a meia-célula final fica fora da última gaiola com os taps
  duplicados por clamp. É erro de ÍNDICE, não de escala: **sobrevive a qualquer
  spacing**, inclusive a uma cascata. Correção: uma sonda a mais por eixo, ou recentrar
  o grid com meia célula real dos dois lados. O comentário do `DDGI_VolumeWeight` já
  documenta essa geometria — a consequência foi compensada no fade e esquecida na
  gaiola.
- **Gaiola calculada pelo ponto viesado.** `base` sai de `biasPos`; a Flax usa o ponto
  cru para a gaiola e o viesado só no alpha, e o `saturate` dela ABSORVE o transbordo
  continuamente enquanto recalcular o `floor` faz a gaiola SALTAR. O RTXGI faz como a
  Smile, então não é violação do algoritmo. Hoje a faixa afetada é ~5% da célula (bias
  capado em 0,40 m contra 8 m). **⚠️ Vira pré-requisito da cascata**: com spacing de 1 m
  o teto não morde (`0.75·1·0.2 = 0,15 m`) e a faixa passa a 15% da célula — três vezes
  maior em proporção.
- **Precisão dos momentos.** `R32G32_FLOAT` custa **+4,31 MiB** no Bistro (o atlas de
  distância é 8832×128 = 1.130.496 texels; o de irradiância inteiro são 2,16 MiB). Medir
  BANDWIDTH, não só VRAM: o gather são 8 taps bilineares por consulta em quatro
  consumidores, e 4,3 MiB fica na ordem do L2 de uma Ampere enquanto 8,6 não.
  **Guardar (média, variância) em vez dos dois momentos crus NÃO é a saída barata**: a
  variância da mistura temporal é `lerp(σ²) + h(1−h)(μ_P − μ_Q)²`, e lerpar variâncias
  perde o termo *between-group* — que é justamente o que dispara quando a superfície se
  move.
- **Composição de energia (A/B, não correção).** O `HitShading` usa
  `((1-F)·diffuse + F)·indirect` e o deferred soma `DiffuseColor·GI` sem o `(1-F)` —
  incoerência interna (o próprio IBL do arquivo aplica `KdIBL`), não erro físico: é a
  convenção da UE. (A metade latente disto — `SetIntensity` sem piso contra o sentinela
  0 — foi fechada na fase 4.)
- **Piso `+0.05` no peso de backface**: não existe na Flax (lá o backface é quadrado
  puro; os pisos aparecem depois) nem na formulação original, e afrouxa o cull. A
  atribuição falsa no comentário foi corrigida na fase 4; o A/B do valor segue aberto.
- **Detector adaptativo**: a correção definitiva do falso positivo é exigir
  PERSISTÊNCIA (a mesma mudança em N frames seguidos; ruído troca de sinal, mudança real
  não), e isso precisa de estado por sonda. A média dos 36 texels também pode perder
  mudança localizada (são ~8 texels totalmente alterados para cruzar 0,50); a
  alternativa é contagem acima de limiar ou média dos maiores N. Retido de propósito
  para não medir duas heurísticas de uma vez.
- **Sondas inativas continuam traçando.** A Flax compacta as ativas e usa dispatch
  indireto. É a outra alavanca de orçamento junto com os raios adaptativos — e é a que
  não custa variância nenhuma, porque a sonda inativa não contribui para o gather de todo
  jeito. Fica para depois da medição: se a folga for grande, nem precisa.

### A decisão estratégica em aberto

A pergunta não é "DDGI ou radiance cache". É **espaçamento adaptativo por cascata
(conhecido, provado na Flax, escopado) ou por hash (novo, nunca executado)**. As duas são
a mesma correção da mesma causa-raiz.

O que se sabe hoje:

- O **World Radiance Cache está implementado e nunca rodou** (um commit,
  `1d9a024`, e nada depois; `Enabled = false`). Promovê-lo a principal exige antes:
  dono neutro para o `InstanceGeo` (hoje pertence ao DDGI e ReSTIR/reflexões/mesh lights
  dependem dele), remover o early return de `DDGI.IsReady()` em `Renderer.cpp`,
  **raios de continuação** (sem eles o cache só aprende direto+emissivo — e a imagem do
  gate ligado mostra o tamanho dessa dívida energética), fallback de cold start,
  substituto para o DDGI na névoa, e invalidação regional própria.
- O RTXGI 2.0 de fato deixou o DDGI na linha v1 e passou a apresentar NRC/SHaRC. O
  RTXDI local é 3.0 e traz **ReSTIR PT**, sucessor natural do ReSTIR GI caseiro
  (`Doc/RestirPT.md`).
- Brixelizer GI foi recusado: duplicaria a TLAS com uma representação SDF paralela.
- A cascata, no molde da Flax, usa a MESMA contagem de sondas por cascata com spacing
  diferente — 1 → 2 cascatas ≈ dobra os raios do update. Não precisa ser assim na Smile:
  a cascata fina pode ter menos sondas. E o deferred **não** paga amostragem dupla: o
  Flax seleciona a cascata que contém o ponto e usa dithering por padrão
  (`DDGI_CASCADE_BLEND_SMOOTH = 0`).

**A medição foi feita e mudou a pergunta.** Ver "Resultado da medição" acima. Isso
reordena a fila:

0. **O sweep foi feito: 2,25× cabe sem sair do piso** (tabela na seção de medição). O
   `kTargetMax` está de volta em 24 — subir a densidade sem a reforma do atlas era trocar
   memória por 5,9 m de espaçamento, longe dos 1–2 m que o problema pede.
1. **O custo não é o que impede a cascata.** O que impedia era o atlas.
2. ✅ **Feito na fase 5: o layout dos dois atlas e do `ProbesTrace`.** O teto saiu de ~24
   mil sondas efetivas para ~1 milhão. **Falta validar** (imagem idêntica) e **remedir a
   banda** — a forma do atlas mudou de 69:1 para 1:1.
3. **Agora a cascata**, e a **gaiola pelo ponto cru tem de entrar JUNTO** com ela — com
   spacing de 1 m o teto do bias deixa de morder e a faixa afetada triplica.
4. **"Raios adaptativos"** segue como alavanca independente, ainda sem A/B: ele devolve
   tempo, e tempo é justamente o que já sobra. Mede-se se a cascata fina apertar o tempo.

**A correção de índice da gaiola de borda não espera pela cascata** — é erro de ÍNDICE e
sobrevive a qualquer spacing. Ela muda a contagem de sondas (logo, o custo) e a cobertura
na face `AABBMax`, então é A/B próprio e fase própria; a escolha entre "uma sonda a mais
por eixo" (margem simétrica, +22% de sondas no Bistro) e "recentrar" (custo zero,
cobertura justa) depende do resultado da medição acima.

## Onde está o quê

| assunto | arquivo |
|---|---|
| gather, Chebyshev, bias, fade, detector, região | `Shaders/GI/DDGICommon.hlsli` |
| update da irradiância + detector | `Shaders/GI/DDGIUpdate.cs.hlsl` |
| update dos momentos + histerese própria | `Shaders/GI/DDGIUpdateDist.cs.hlsl` |
| terminador e o gate de medição | `Shaders/GI/HitShading.hlsli` |
| constantes, janelas derivadas, cbuffer | `Engine/Include/Smile/Graphics/DDGI.h` |
| reset, invalidação, empacotamento | `Engine/Source/Graphics/DDGI.cpp` |
| domínios de invalidação | `Engine/Include/Smile/Graphics/HistoryDomain.h` |
| contrato do gather compartilhado + gate | `Engine/Include/Smile/Graphics/GIHitSampling.h` |
| eventos do editor | `GizmoController.cpp`, `SceneOutlinerBridge.cpp`, `LightsBridge.cpp` |
| bracket de stall do wait | `Renderer.cpp`, no `if (GIComputeFence != 0)` |
| classificação de raios por sonda | `Shaders/GI/DDGIRelocate.cs.hlsl` |
| empacotamento dos atlas | `DDGI_TileOrigin` / `DDGI_TilesPerRow` (`DDGICommon.hlsli`) + dimensões no `SetupForScene` |
| empacotamento do `ProbesTrace` | `DDGI_TraceTexel` (`DDGICommon.hlsli`) + `kTraceProbesPerRow` (`DDGI.h`) |
