# Auditoria do DDGI e estado da invalidação

Estado em 12 de agosto de 2026. Este documento registra a revisão completa do DDGI cruzada
com o paper original (Majercik et al., JCGT 2019), o código-fonte local da Flax e do
RTXDI 3.0, mais o SDK RTXGI de memória — e as fases que saíram dela.

**Onde a coisa está:** as fases 1 a 5 fecharam invalidação, medição e o layout dos atlas;
a 6.1 e a 6.2a construíram o encanamento de cascatas sem mudar um pixel; o portão do debug
migrou as três ferramentas de diagnóstico; e a **6.2b‑i acendeu a segunda cascata, fixa** —
com os três achados da revisão dela corrigidos (entre eles um use-after-free de GPU no
rebuild pelo editor) e **A/B aprovado: menos vazamento de luz, por +0,83 ms de frame**.
Falta a 6.2b‑ii — a cascata fina seguindo a câmera, com scrolling toroidal. Ver
"O que falta".

| fase | commit | validação |
|---|---|---|
| 1–5 | `26b71a9`, `5d16cf2` | A/Bs + imagem idêntica no reempacotamento |
| 6.1 | `ad6d754` | imagem idêntica (`CascadeCount = 1`) |
| 6.2a | `9e6254e` | imagem idêntica, com os wrappers ativos |
| portão do debug | `4d378ad`, `eb3b9f0` | idem |
| 6.2b‑i | pendente de commit | **APROVADA** — menos vazamento de luz, por +0,83 ms de frame |

Escrito no fim de sessões longas, para a próxima começar do ponto certo.

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

No Bistro o resultado é exato, sem desperdício: `TilesPerRow = 69`, 8 bandas, grade
`69 × 64`. Atlas de distância `1104 × 1024` — **os mesmos 1.130.496 texels de antes da
reforma**, agora em proporção 1,08:1 em vez de 69:1.

**A banda tem de conter fileiras Z INTEIRAS**, e isso é condição da vizinhança, não
arredondamento: o plano é `x + z·CountX`, então uma largura que não seja múltiplo de
`CountX` corta uma fileira X no meio e joga `x` e `x+1` em bandas diferentes — a adjacência
pela qual o layout existe. A primeira versão do seletor procurava um *divisor* de
`CountX·CountZ` perto da raiz de `NumProbes` e caiu em `69 = 3·23` **por sorte**; `552` tem
divisores como 92 e 276 que não são múltiplos de 23, e ali a vizinhança se perderia de novo.
Ela também podia devolver mais de 1024 colunas e fazer o `gridFits` recusar um grid que
caberia com outra largura, abrindo o espaçamento sem necessidade.

O seletor agora enumera `zRowsPerBand` de 1 a `CountZ` e forma sempre
`TilesPerRow = CountX · zRowsPerBand`, escolhendo o candidato que **minimiza a maior
dimensão** — critério que empurra para o quadrado, penaliza o desperdício da banda
incompleta e é exatamente o que a checagem de limite olha. `CountZ` não passa de algumas
dezenas, então enumerar tudo não custa nada. No Bistro o vencedor é `zRowsPerBand = 3`, ou
seja o mesmo 69 — agora por construção.

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

### Fase 6.1 — encanamento de cascatas, com uma cascata

Pré-requisito de tudo que vem depois, e sem efeito visível: com `CascadeCount = 1` a imagem
tem de ficar **idêntica**.

**A decisão de arquitetura, e ela não é a da Flax.** Lá as 4 cascatas são todas centradas na
câmera e fora da última cai um `FallbackIrradiance` constante — desenho para mundo aberto.
Aqui a **cascata mais grossa é o volume de hoje**, que cobre a cena inteira e converge em
todo lugar; as finas entram por dentro. O fallback da fina é a grossa, e só quem sai da
grossa (o terreno) cai no ambiente hemisférico. Assim o comportamento atual é um subconjunto
exato do novo, em vez de ser substituído por ele.

- **Espaço de índice `(cascata, índice local)`.** O atlas e os buffers (`ProbeData`,
  `ProbeRayCount`, `ProbesTrace`) falam em índice GLOBAL
  (`cascata·porCascata + local`); a geometria — posição no mundo, gaiola, vizinhos — fala
  em LOCAL, com o `(GridMin, Spacing)` daquela cascata. Com uma cascata os dois coincidem,
  e é por isso que a fase não muda pixel nenhum.
- **As cascatas empilham em blocos de LINHAS do atlas.** Mesmo motivo que decidiu o layout
  em bandas: cada cascata mantém a própria grade intacta, então o bloco 2×2 de tiles
  adjacentes continua valendo dentro dela. Intercalar cascatas na largura ou no índice
  destruiria de novo a vizinhança que custou 0,45 ms. É onde a Flax também as empilha.
- **Contagem de sondas IGUAL em todas as cascatas** (como a Flax). Só o par
  `(origem, espaçamento)` varia, o que cabe num `Vec4` — e faz bandas, linhas por cascata e
  índice local ficarem todos deriváveis de `count` e `tilesPerRow`. **Nenhum campo novo de
  cbuffer em nenhum dos cinco consumidores do gather**: `DDGI_TileRowsPerCascade` sai de
  `tilesPerRow / count.x`, o que só é possível porque o `atlasGridFor` garante que
  `tilesPerRow` é múltiplo de `count.x`. A correção da fase anterior virou pré-requisito
  desta.
- **A seleção de cascata NÃO entrou.** Com uma cascata ela é código morto e intestável;
  entra na 6.2 junto com a segunda. Os cinco sítios de gather passam `0` explicitamente.

### Fase 6.2a — seleção e blend, com uma cascata

Última fase invisível: `CascadeCount = 1`, imagem idêntica, mas agora com os wrappers
ATIVOS — é isso que torna o teste um teste da seleção e não só do encanamento.

- **`DDGICascadeChoice {Primary, Next, PrimaryWeight}`** e `DDGI_SelectCascade`. Uma
  cascata devolve `{0,0,1}`; dentro de uma fina, `{fina, fina+1, peso}`; fora das finas,
  `{grossa, grossa, 1}`. `Next == Primary` é o sinal de "não há blend", e é o que faz os
  wrappers pularem o segundo gather.
- **Só as cascatas ANTERIORES à grossa desvanecem**, nas últimas 2,5 células (valor da
  Flax). A grossa é fallback INCONDICIONAL: a borda externa dela continua sendo do
  `DDGI_VolumeWeight`, que é configurável e desvanece ao longo de N células. Aplicar o
  fade de cascata nela substituiria aquele em silêncio, e a equivalência com o
  comportamento histórico morreria no instante em que o seletor fosse ligado.
- **A seleção usa a posição CRUA.** É a "gaiola pelo ponto cru" que a auditoria exigia
  junto com a cascata, e aqui ela deixa de ser detalhe: o bias escala com o espaçamento,
  então dois pixels vizinhos de uma parede rasante poderiam escolher CASCATAS diferentes
  por causa de um deslocamento artificial.
- **Dois wrappers, não um.** `SampleDDGIIrradianceCascaded` e `...ChebCascaded`: o fog não
  tem atlas de distância nem `ProbeData`, e obrigá-lo ao caminho do Chebyshev
  acrescentaria bindings sem lhe dar nada. Eles devolvem SÓ a irradiância do DDGI — o peso
  do volume e o fallback ficam no caller porque são diferentes em cada um (deferred e fog
  caem no ambiente hemisférico, o `HitShading` cai em preto).
- **O bias é recalculado por cascata DENTRO do wrapper**, e a assinatura pede
  `(V, escala, teto)` em vez de um `biasVec` pronto — o erro não é possível sem mudar a
  assinatura.
- **`FDDGICascadeConstants`**: um POD, preenchido por `FDDGI::CascadeConstants()`, copiado
  para os cinco cbuffers em vez de cinco loops independentes. Offsets presos por
  `static_assert` nos cinco.
- **`PrepareCascadePlacement` DECIDE, `UpdatePerFrame` PUBLICA.** A costura existe por
  ordenação: os três publicadores capturam `CascadeConstants()` cedo, e o `UpdatePerFrame`
  roda bem depois. Mover a cascata lá dentro deixaria os consumidores do frame amostrando
  com a origem anterior enquanto o atlas já teria sido atualizado com a nova.

### Portão do debug — fechado antes de acender a segunda cascata

O diagnóstico é o instrumento que distingue erro de seleção, de blend, de visibilidade e
de scrolling. Entrar no A/B mais difícil da série com ele defasado seria entrar sem
instrumento, e o comentário do próprio `DDGIDebugPoint` diz que ele roda "a MESMA função,
não uma cópia" justamente para não mentir.

- **Decomposição do índice, agora UMA.** `GetDebugProbeCoordValues` devolve `(cascata,
  índice local, coord, contagens, GridMin, Spacing)` e os cinco consumidores usam esse
  resultado. Antes cada um refazia a conta assumindo índice LOCAL, e o defeito se
  replicava: coord fora do grid, posição com a origem da cascata errada, e um stepping que
  reconstruía o índice sem a base da cascata — saltando para a cascata 0 no primeiro passo,
  sem dizer que tinha trocado.
- **`DDGIDebugPoint` virou duas páginas de oito** (primária e, no blend, a próxima), cada
  uma com o próprio `base`/`frac` e o próprio BIAS. A normalização é **por página**: cada
  grupo de oito divide pelo próprio somatório e só então entra na mistura. Somar os 16 num
  denominador único daria pesos que a imagem nunca usou. O fast path é o mesmo teste dos
  wrappers, e quando dispara a segunda página sai marcada como ignorada — os taps continuam
  publicados para o painel poder dizer "não participaram".
- **Duas escalas no heatmap de distância.** A global (overview) sai da cascata GROSSA: com
  o teto da fina, os tiles grossos saturam por completo; com o da grossa, os finos ficam
  escuros mas LEGÍVEIS — informação reduzida em vez de perdida. O tile inspecionado e as
  esferas do visualizador usam a régua da própria cascata.
- **Metadados da seleção**: o parser lia só o `X` da linha e os campos ficavam nos defaults
  `(0, 0, 1)` — indistinguível de "uma cascata, sem blend", ou seja estado plausível e
  falso. O resumo mostra `c0 72% + c1 28%`, ou `c1 100%` no fast path.

### Fase 6.2b‑i — a segunda cascata, FIXA

Primeira fase da série que muda a imagem. Uma variável nova só: a cascata existe, mas não
se move.

- Seletor **1 / 2** em Configurações → GI, que **recria o volume** na hora. Deixar para o
  próximo load faria o botão aceitar o clique sem fazer nada visível.
- A grossa é o volume de sempre, intocada. A fina sai dela por
  `kCascadeSpacingRatio = 4`: **8,02 → 2,0 m no Bistro**, dentro dos 1–2 m do paper. Cobre
  `46×16×48 m` (gaiola; os centros cobrem `44×14×46`).
- Ancorada no centro da AABB da cena e **snapada ao próprio espaçamento**. Sem o snap a
  grade nadaria sob a geometria e cada sonda trocaria de posição no mundo por uma fração de
  célula — o pior caso para um cache temporal. Com ele, só se move em células inteiras, que
  é o que o scrolling da 6.2b‑ii sabe compensar.

**Por que FIXA:** com a fina seguindo a câmera, um artefato na transição seria ambíguo
entre erro de seleção/blend e erro de scrolling.

#### O que a revisão da 6.2b‑i pegou (12/08/2026)

Três achados, e o primeiro era bloqueador do A/B — não do commit, do A/B: ele é um
use-after-free de GPU que se manifesta como corrupção aleatória, exatamente o tipo de
sintoma que envenenaria a leitura visual da fase.

- **O rebuild liberava recurso ainda em uso pela GPU.** `RebuildGIVolume` (o clique no
  seletor de cascatas) cai direto no `SetupGIForScene`, e ele começa soltando os recursos
  e os slots de descritor do volume anterior. O lock do `RendererHandle` serializa a CPU e
  **não diz nada sobre a GPU**: no editor a mutação vem da thread da GUI enquanto o último
  frame ainda está em voo, referenciando o atlas e o `ProbesTrace` que estão prestes a ser
  soltos e reusados. No load isso nunca apareceu porque ali a fila já está parada.
  Corrigido com a drenagem das **duas** filas no topo do `SetupGIForScene`, e a ordem
  importa: `CommandQueue.Flush()` **antes** de `ComputeQueue.WaitIdle()`, porque o update
  do DDGI é submetido na fila de compute com um `Wait` no fence da direta
  (`SubmitAfter`) — esperar a compute primeiro seria esperar por trabalho que ainda
  depende da outra fila. Ficou na função inteira e não no `RebuildGIVolume` porque a
  exposição é de todos os sete sistemas que ela realoca (`TemporalMotion`, `ReGIR`,
  `RadianceCache`, `MeshLights`, reflexões, passe de debug), não só do DDGI.
- **O estado regional sobrevivia ao rebuild.** O `SetupForScene` zerava
  `HysteresisResetPending` e `RelocateFramesLeft` e deixava passar `InvalidateFramesLeft_`,
  `InvalidateDistFramesLeft_`, a caixa e `ReclassifyPending_`. Como os dois contadores
  contam frames de **update**, a janela atravessava o rebuild inteira: o atlas recém-zerado
  passaria dezenas de frames com histerese reduzida numa região arbitrária da grade nova —
  a caixa está em coordenadas de um volume que não existe mais. Zerados todos no setup.
  Nada se perde: o reset one-shot é estritamente mais forte que qualquer invalidação
  regional pendente, porque substitui o volume INTEIRO em vez de uma caixa.
  - Efeito colateral que a limpeza obrigou a resolver: com `ReclassifyPending_` zerado,
    relocação OFF e raios adaptativos ON, ninguém mais escreveria o `ProbeRayCount` do
    volume novo e a contagem ficaria congelada no 64 do clear — o knob nasceria inerte.
    `RelocateFramesLeft` passou a ser `Relocation ? kRelocateConvergeFrames : (AdaptiveRays
    ? kReclassifyFrames : 0)`. É a mesma correção que a fase 4 fez nos setters, agora no
    nascimento do volume.
- **O texto do painel afirmava demais.** "A medição diz que cabe" apoiava-se nas ~9.920
  sondas do sweep, que foram medidas com UMA cascata, em regime e no layout anterior — o
  gather de duas tem outro padrão de acesso. Reescrito como expectativa, com o pedido de
  medição no próprio painel (ver abaixo).

### O A/B de custo da 6.2b‑i (12/08/2026) — e a lição que ele corrige

Bistro, Release, RTX 3060 Ti, render 1573×804, em regime (badge `ASYNC` presente, ou seja
`CanRunAsync()` verdadeiro e a relocação já convergida).

| | 1 cascata | 2 cascatas | delta |
|---|---|---|---|
| sondas | 4.416 | 8.832 | ×2 |
| raios/frame | ~283 mil | ~565 mil | ×2 |
| `DDGI (async)` | 2,64 ms | 5,14 ms | **×1,95** |
| `Espera do DDGI (async)` | 0,02 ms (piso) | 0,10 ms | +0,08 |
| **frame de GPU** | **7,65 ms** | **8,48 ms** | **+0,83 (+10,8%)** |
| FPS de GPU | 131 | 118 | −13 |
| Z‑prepass | 0,88 ms | 1,69 ms | +0,81 |
| G‑buffer | 0,87 ms | 1,08–1,21 ms | +0,2…+0,3 |
| Deferred lighting | 0,65 ms | 0,63–0,65 ms | ~0 |
| Reflexos (composite) | 0,66 ms | 0,61–0,68 ms | ~0 |
| Volumetric fog | 0,27 ms | 0,25 ms | ~0 |
| VRAM "GI e reflexos" | 312,9 MB | 322,4 MB | +9,5 MB |

**O gather de duas cascatas é de graça.** Deferred, reflexões e as duas névoas não se
mexeram — dentro do ruído. As duas páginas de oito taps, a seleção e o blend não aparecem
na conta. O fast path da 6.2a (`Next == Primary` pula o segundo gather) está fazendo o que
foi desenhado para fazer: só as células de borda pagam duas páginas.

**O update escala linear e nada mais**: 0,598 µs/sonda contra 0,582 — ×1,95 para ×2 sondas.
Todo o custo do sistema é o update, e ele é proporcional à contagem de sondas.

**⚠️ E aqui está a correção metodológica: a ESPERA não é o instrumento de orçamento.** Ela
saiu do piso (0,02 → 0,10 ms) mas explica **0,08 ms** de um custo de **0,83 ms**. O frame
pagou dez vezes mais do que a espera acusou.

Onde o resto foi parar: nos passes que dividem a GPU com o compute. A janela de overlap vai
do `SubmitSegmentAndContinue` até o wait — céu, sombras, **Z‑prepass** e **G‑buffer**
(`Renderer.cpp`, entre `RecordSkyAndClouds` e o bracket) — e são exatamente esses que
subiram, +0,81 e +0,2…+0,3 ms. O Z‑prepass **não lê o DDGI**: não há caminho pelo qual uma
mudança de GI o torne mais caro que não seja **contenção por SM e banda** com o compute
assíncrono.

Isso confirma, com número, o aviso que a fase 5 tinha escrito sem prova: *"escondido no
overlap quer dizer que a fila direta não espera — não que o trabalho seja grátis"*. E
**obriga a qualificar o sweep**: as "9.920 sondas escondidas" foram medidas com a ESPERA
como critério, e a espera fica perto do piso enquanto o frame paga. O sweep provou o que
enunciou — que a fila direta não estola — e isso é **menos** do que "cabe". O critério de
orçamento passa a ser o **frame de GPU**; a espera vira o instrumento secundário, que
detecta só o caso em que o compute nem cabe na janela.

**Veredito da 6.2b‑i: APROVADA (12/08/2026).** O A/B visual no Bistro reduziu o vazamento
de luz que ainda restava — que é literalmente a causa-raiz que abriu esta auditoria (grid de
8 m contra os 1–2 m que o paper pede). Os 0,83 ms compram a correção do defeito que motivou
a série inteira, e o sistema segue folgado para 60 fps. A cascata fica.

⚠️ **Confounder honesto, e ele importa**: os dois passes FORA da janela também se mexeram
(Motion temporal +0,12, GTAO −0,26 ms), o que põe o ruído entre capturas em ±0,2 ms. O
+0,81 do Z‑prepass é 4× isso, então contenção é de longe a melhor explicação — mas o
Z‑terreno sozinho fez 0,66 → 1,14 ms, e custo de prepass de terreno depende de quanto
terreno a câmera enxerga. Separar por completo exige um par de capturas da **mesma câmera**.
O que não depende disso: o ×1,95 do update, o gather de graça e os +9,5 MB.

### Fase 6.2b‑ii — a cascata fina segue a câmera (scrolling toroidal)

Implementada, Debug e Release verdes, e **executada na Bistro em 18–19/08/2026**. A fina passou a ser ancorada na
câmera em vez do centro da cena, e é isso que torna o scrolling obrigatório: com o snap ao
espaçamento, a origem muda a cada 2 m andados, e sem scrolling todo o conteúdo guardado
passaria a representar outro ponto do mundo de uma vez.

**O invariante:** a sonda de coordenada geométrica `c` mora no slot `(c + scroll) mod count`,
com `scroll = origem_em_células mod count`. Como o slot é `(origem + c) mod count`, ele
depende só da célula ABSOLUTA do mundo — um ponto fixo do mundo nunca troca de slot enquanto
a janela desliza. Só as lâminas recém-expostas viram conteúdo novo.

**Três decisões, e as três são sobre onde o inteiro começa e termina.**

1. **O scroll é CARREGADO, não derivado.** Seria natural derivá-lo de `GridMin/spacing` (o
   padrão da casa, ver `tilesPerRow`) e custaria zero bytes. Não dá: a **grossa** tem
   `GridMin = AABBMin − 0.5·spacing`, deliberadamente não snapado, então a divisão não cai
   num inteiro. Derivar exigiria um float pousando exatamente num inteiro, e meio ULP ali
   desloca o atlas inteiro em uma sonda — em silêncio. Derivar vale quando a conta é inteira
   dos dois lados; aqui não é. Entrou como `ScrollOffset[4]` no POD (80 → 144 bytes).
2. **A origem é guardada em CÉLULAS e o `GridMin` é derivado dela**, não o contrário. É o que
   faz o `scroll` sair de aritmética inteira ponta a ponta, com resto forçado positivo (cena
   em coordenadas negativas daria slot negativo).
3. **A lâmina nova sai de uma subtração inteira.** `DDGI_NewlyExposed(c, delta, count)`: a
   sonda ocupava, na janela anterior, a coordenada `c + delta`; se aquilo caía fora de
   `[0, count)`, o slot guardava outro ponto do mundo. ⚠️ A primeira versão comparava as duas
   gaiolas em coordenadas de mundo, em **float**, e comparava nas BORDAS — o mesmo erro que a
   decisão (1) tinha acabado de evitar, reintroduzido a dois campos de distância. Teleporte
   não precisa de caminho próprio: com `|delta| >= count` nenhuma coordenada sobrevive ao
   teste e a cascata inteira se declara nova.

**A limpeza é dos QUATRO estados, e não só dos dois atlas.** Histerese 0 na irradiância e nos
momentos não basta — o slot reciclado ainda carrega o **offset de relocação** do lugar antigo,
a **marca de inativa** e a **contagem de raios**. E limpar só os atlas seria *pior* que não
limpar: o trace partiria da origem errada e o update tomaria esse resultado INTEIRO (histerese
0, sem histórico para diluir). Por isso o mesmo teste roda nos quatro passes — trace (origem
sem offset e 64 raios), update e updateDist (histerese 0), relocate (`prev` zerado, sem herdar
`w < 0`).

**Relocação one-shot na lâmina, e o motivo é de informação e não de qualidade.** O `delta` só é
diferente de zero no update em que a lâmina estreia — só nele dá para saber QUAIS sondas são
novas. O lerp de 0,25 é amortecimento de um laço de realimentação (a sonda move, o trace
seguinte sai da posição nova, o alvo se corrige) e a sonda recém-exposta não tem esse frame
seguinte. Ou chega ao lugar certo nesta passada, ou fica a um quarto do caminho para sempre.
O clamp em `maxOff` já limita o salto.

**`MiscParams3.w` — a passada de scroll não pode reclassificar o grid.** O `.z` (o passe roda)
ganhou um par: `.w` diz que o agendamento veio SÓ da rolagem. Aí o relocate retorna cedo fora
da lâmina — e o early-return fica **antes** da escrita do `ProbeRayCount`, porque reclassificar
as sondas velhas ali as remediria a partir do trace DECIMADO delas, que é exatamente a catraca
fechada na fase 4, reaberta uma célula por vez enquanto a câmera anda. Pelo mesmo motivo o
trace só força os 64 raios globalmente quando `z && !w`; na lâmina, força sempre —
`newlyExposed` vale **independentemente** de `scrollOnly`, porque durante os 180 frames
iniciais uma rolagem chega com `z=1, w=0` e a sonda nova continua não podendo herdar nada.

**O desbloqueio que a fase obrigou, e que vale sozinho: a relocação virou async-legal.** O
scroll agenda relocação a cada célula cruzada, e `CanRunAsync()` devolvia falso enquanto
houvesse relocação — ou seja, o DDGI cairia no caminho síncrono exatamente enquanto a câmera
anda. A causa era uma transição: o relocate promovia `ProbeData` de `kAtlasRead` (que contém
PIXEL) para UAV dentro do `RecordUpdate`, ilegal em fila de compute. Movendo a saída para o
`TransitionForUpdate` — que roda na DIRETA — o relocate só precisa de `NON_PIXEL → UAV`.
Conferido que nada entre o submit e o wait lê `ProbeData` (ali só há escritas de cbuffer de
reflexões e ReSTIR). **De quebra, os 180 updates de convergência inicial passaram a rodar
escondidos**, o que apaga a ressalva que o protocolo de medição repete desde a fase 4.

**Equivalência preservada:** para a grossa, `Scroll = {0,0,0}` e `delta = 0` sempre, então
`DDGI_StorageCoord`/`GeometricCoord` são identidade e o endereçamento dela é bit a bit o de
antes do scrolling existir.

#### O segundo tempo da lâmina, e por que ele é obrigatório

A primeira versão tinha o `canMark` intacto (`RelocateFramesLeft > 1`), e concluí que isso era
"verificado e seguro" porque nenhuma marca órfã seria escrita numa passada de scroll isolada.
Estava certo sobre a órfã e **errado sobre a conclusão** — a mesma condição que evita a órfã
estava dizendo que *não existe o frame de correção*, e é justamente ele que faltava.

**O defeito:** na estreia da lâmina a ordem dentro do `RecordUpdate` é trace → update →
updateDist → **relocate**. A sonda nova traça do VÉRTICE (offset zerado, correto) e os dois
atlas são integrados dali com histerese 0 — e só então o relocate publica o offset one-shot. Ao
fim do frame, `ProbeData` descreve uma posição e os atlas descrevem outra. Sem um segundo
update, a histerese normal preserva a incoerência: no atlas de DISTÂNCIA, com 0,99, ela dura
~199 updates, com o Chebyshev medindo distâncias de uma origem contra momentos de outra.

**A correção não é um mecanismo novo — é alcançar o que já existe.** A marca de
"recém-relocada" (`w >= 1` → histerese 0 no update seguinte) existe exatamente para "a sonda
moveu, o atlas dela é do lugar antigo", e a relocação normal a usa todo dia. A lâmina não a
alcançava porque `canMark` exigia um frame agendado depois. Então:

- a estreia AGENDA o update seguinte (`ScrollFollowUpPending`), e com ele garantido a marca
  passa a ser escrita (`canMark` ganhou `|| Scrolled`);
- no update seguinte, o relocate continua restrito — agora a `newlyExposed || w >= 1` —, os
  dois atlas são reintegrados a partir da posição relocada, e a marca é demovida;
- o trace força os 64 raios também em `w >= 1`: o relocate vai reclassificar essa sonda a
  partir DESTE trace, e deixá-la decimada alimentaria o classificador com a própria decimação —
  a catraca da fase 4, pela porta do segundo tempo.

As duas lâminas convivem se uma estreia durante um follow-up: são identificadas por critérios
diferentes (`delta` e a marca `w >= 1`), então o predicado cobre as duas sem estado extra.

**E o follow-up NÃO move a sonda de novo** — duas correções sobre a primeira tentativa, e as duas
vieram do mesmo mal-entendido de que "o follow-up é só mais uma passada de relocação":

- **Ele não pode relocar.** Este update existe para fechar a divergência que a estreia abriu (os
  atlas integrados no vértice, o offset publicado depois). Nele o trace já sai da posição
  one-shot e os dois atlas estão sendo reintegrados dela; mover outra vez recriaria a MESMA
  divergência um update adiante, e sem nenhum frame agendado para fechá-la. Ele ainda varre o
  trace — contagem de raios e classificação ativo/inativo são recalculadas —, só o deslocamento
  fica parado.
- **`canMark` sai de `ScrollDebut`, não de `ScrollWork`.** Ligá-lo no follow-up permitiria à
  ÚLTIMA passada agendada escrever uma marca `w >= 1` que ninguém mais apaga (bastava um
  `bigJump`): órfã, ou seja histerese 0 eterna naquela sonda — exatamente o defeito que o `> 1`
  original existia para evitar. Só a estreia agenda um update seguinte, então só nela existe o
  frame que demove.

Daí os dois predicados separados no CPU: `ScrollDebut` (há lâmina nova neste update) e
`ScrollWork` (o passe tem trabalho de scroll, estreia ou follow-up). Fundi-los foi o defeito.
`ScrollFollowUpPending` também entrou no reset do `SetupForScene`, junto das janelas regionais:
um follow-up pendente descreve uma lâmina de um volume que não existe mais.

#### Primeira validação viva e custo (18–19/08/2026)

O harness MCP fixou Bistro exterior, câmera e 10:00, desativou instrumentação, confirmou duas
cascatas reais (`23×8×24`, 4.416 sondas cada) e moveu a câmera em passos absolutos. O percurso
de 6 m cruzou três células finas; em todas, `ΔgridMin/spacing` foi inteiro e exatamente igual ao
delta modular de `scrollCells`. A caminhada de desempenho percorreu 15 m sem camera cuts,
cruzou oito células finas e repetiu a mesma igualdade. A grossa não cruzou célula nesses
percursos, como esperado pelo espaçamento de 8,018 m e pela fase inicial da grade.

**Régua de custo:** Release, RTX 3060 Ti, 1573×804, `gameplay_rr`, 15 s de aquecimento + 60
amostras por braço, timestamps brutos, ciclo `1→2→1→2`, throttle de segundo plano desligado.

- uma cascata: frame mediano **23,87 ms**, p95 **24,78 ms**, DDGI async **2,80 ms**;
- duas cascatas: frame mediano **25,30 ms**, p95 **26,36 ms**, DDGI async **6,49 ms**;
- custo da segunda: **+1,43 ms / +5,99%** no frame mediano e **+1,58 ms / +6,40%** no p95;
- o compute cresceu **+3,69 ms**, mas parte continuou escondida; o wait mediano saiu do piso
  (**0,02 → 0,49 ms**), portanto duas cascatas já excedem parcialmente a janela de overlap;
- duas cascatas andando: **24,28 ms** mediano e **25,73 ms** p95. O DDGI async mediano ficou
  praticamente igual (**6,45 ms**); o p95 subiu 0,25 ms e o wait p95 0,56 ms. Não apareceu
  regressão sustentada de frame causada pelo scrolling, embora a cauda do wait registre as
  lâminas novas;
- o delta de `QueryVideoMemoryInfo` entre uma e duas cascatas foi só 0,56 MiB e não serve para
  atribuir residência: o allocator preserva heaps comprometidos entre os braços. VRAM por
  recurso precisa de instrumento próprio se virar gate.

A matriz de política, na mesma sessão, mediu `all_off` 22,18 ms, volume DDGI com superfície Off
24,53 ms, DDGI primário 25,30 ms, SHaRC+Black 28,59 ms e SHaRC+DDGI 29,48 ms. O relatório
canônico é `build/bin/Release/Captures/Benchmarks/gi-cascades-2026-08-19T03-14-37-425Z.json`.
O log persistente do mesmo PID não contém erro D3D12/DXGI, device removal nem erro de validação.

**Ainda falta para fechar toda a 6.2b‑ii:** diagonal, movimento vertical, teleporte maior que a
grade, objeto cruzando a borda da fina e uma execução explicitamente com o debug layer ligado.
O caminho axial lento e o custo parado/em movimento estão aprovados.

#### A/B dos raios adaptativos (19/08/2026)

Concluído no mesmo Bistro, GPU, resolução e regime da régua acima, agora mantendo **duas
cascatas e política DDGI fixa**. Foram dois ciclos independentes `OFF→ON→OFF→ON`: câmera parada
e caminhada contínua de 15 m. Cada braço confirmou pelo MCP o toggle efetivo, `MinRays=16`,
`MaxRays=64`, duas cascatas reais e oito células finas cruzadas nos percursos. A distribuição
efetiva por sonda continua GPU-only de propósito: fazer readback no meio da régua introduziria o
stall que o teste quer medir.

- parado, o DDGI async caiu de **6,318 para 6,087 ms**: **−0,231 ms / −3,65%**. O wait mediano
  caiu de 0,413 para 0,234 ms;
- no frame parado, a média foi só **24,473 → 24,420 ms**: **−0,052 ms / −0,21%**. O p95 ficou
  essencialmente empatado (25,174 → 25,195 ms), e os pares individuais deram −0,137 ms e
  +0,241 ms de economia: portanto **não há ganho de frame robusto com câmera parada**;
- andando, o DDGI async caiu de **6,530 para 6,190 ms**: **−0,340 ms / −5,21%**;
- no frame andando, os dois pares favoreceram o toggle: **24,032 → 23,650 ms** na mediana
  (**−0,383 ms / −1,59%**) e **25,731 → 25,006 ms** no p95
  (**−0,725 ms / −2,82%**). O wait mediano caiu 0,155 ms (−20,37%).

Conclusão: o toggle **faz trabalho real e reduz compute/contenção**, com retorno mais claro
durante scrolling, mas não devolve sozinho o custo da segunda cascata e não merece ser vendido
como ganho universal de frame. Continua sendo a alavanca de menor risco.

O gate visual veio em seguida: dois pares alternados full-64/adaptive, `scientific N=128`,
resolução nativa, sem TAA/upscaler, mesma pose/hora/política. O manifesto passou a registrar
`ddgiCascadeCount`, o toggle e os limites 16–64 para cada PNG ser auditável sem o relatório MCP.

- média dos dois full-64 contra média dos dois adaptive: **57,91 dB**, SSIM de luminância
  **0,99953**, erro absoluto médio **0,050 nível de 8 bits por canal**;
- viés médio de luminância: **−0,0065 / 255** — sem perda sistemática de energia;
- só **0,064%** dos pixels passaram de 5 níveis por canal na média dos pares e **0,011%**
  passaram de 10;
- a repetição adaptive entre si variou **mais** (52,46 dB) que a diferença média contra full-64.
  O mapa ampliado 8× repetiu o mesmo resíduo temporal em folhagem, luminárias e detalhes finos,
  sem faixa de cascata, vazamento, escurecimento coerente ou região nova atribuível ao toggle.

**Decisão: raios adaptativos 16–64 ficam ON por default.** O controle OFF permanece full-64
bit-idêntico para diagnóstico. Esta decisão é para o regime medido da Bistro; cenas de produção
novas ainda entram na regressão visual normal, não ganham uma promessa universal por decreto.

Relatório canônico:
`build/bin/Release/Captures/Benchmarks/gi-adaptive-rays-2026-08-19T03-27-42-901Z.json`.
O log persistente do editor PID 21124 passou sem erro D3D12/DXGI, device removal ou erro de
validação.

Par visual canônico:
`build/bin/Release/Captures/Validation/gi-adaptive-visual-2026-08-19T03-34-56-762Z.json`;
métricas e mapas:
`build/bin/Release/Captures/Validation/gi-adaptive-visual-2026-08-19T03-34-56-762Z-analysis/`.
O log do PID 22072 também passou limpo.

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

⚠️ **Releitura obrigatória (12/08/2026): "escondido" era menos do que parecia.** O A/B da
6.2b‑i mediu o frame de GPU pela primeira vez e mostrou que 8.832 sondas custam +0,83 ms de
FRAME com a espera em 0,10 ms — ou seja o critério deste sweep (espera no piso) não prova
que o grid é barato, só que a fila direta não estola. Ver "O A/B de custo da 6.2b‑i". O
sweep continua válido no que enunciou; a conclusão de orçamento que se tirava dele, não.

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

**A decisão foi tomada: cascata, e não hash.** O sweep mostrou que o tempo não era o
obstáculo (2,25× cabe escondido), a fase 5 tirou o obstáculo que era real (o layout do
atlas), e a 6.1/6.2a construíram o encanamento. A arquitetura escolhida está no bloco da
fase 6.1: **a grossa é o volume de hoje e as finas entram por dentro** — o oposto da Flax,
onde todas seguem a câmera e fora da última cai um fallback constante.

## O que falta

**A 6.2b‑i está fechada** — custo medido, visual aprovado (menos vazamento). Sobra dela um
item barato e não bloqueante:

- **Um ciclo `1 → 2 → 1 → 2`**, parado e após convergência, para o delta de frame virar
  número definitivo. Hoje ele carrega o confounder do Z‑prepass (ver a tabela). Muda a
  atribuição do custo, não o fato dele.

**A 6.2b‑ii já rodou no eixo X e teve o custo medido** (ver a seção dela). O gate axial passou;
resta completar, em ordem:

1. **Smoke com o debug layer ligado.** A mudança de estado do `ProbeData` para `NON_PIXEL` na
   fila direta é a linha mais arriscada da fase, e é a única cujo erro o compilador não pega —
   o debug layer pega, e na hora.
2. ✅ **Caminhada lenta cruzando células** — passou em 18–19/08/2026. O
   sintoma de scroll errado é um erro de UMA lâmina: invisível parado, rastro atrás da câmera
   em movimento.
3. **Diagonal** (as três lâminas de uma vez) e **movimento vertical** (o eixo Y tem `count`
   menor, então a lâmina é proporcionalmente maior).
4. **Teleporte** maior que a grade: tem de reconvergir sem flash, pelo caminho geral.
5. **Objeto atravessando a borda da fina**, que cruza scrolling com invalidação regional.
6. ✅ **Custo, de novo** — medido parado no ciclo `1→2→1→2` e andando por 15 m. A segunda
   cascata custou +1,43 ms de frame; o scrolling não adicionou regressão sustentada.

**Depois disso**, a ordem acordada continua: A/B dos raios adaptativos, compactação das
inativas, e o update escalonado por último.

Isso exige **scrolling toroidal**, e não como otimização: com snapping ao espaçamento a
origem muda a cada 2 m andados, e sem scrolling todo o conteúdo guardado passa a
representar outro ponto do mundo de uma vez — um flash por célula cruzada. O esquema é o da
Flax (`GetDDGIScrollingProbeIndex`): a sonda de coordenada local `c` guarda no slot
`(c + scroll) mod count`, e só as lâminas recém-expostas são limpas.

Checklist acordada:

- `ScrollOffset` (int3) e `ScrollClear` por cascata, no POD e nos cinco cbuffers;
- o wrap entra em **todo** endereçamento de sonda (gather, trace, updates, relocate, debug);
- limpar **só as lâminas novas**, nos quatro estados: irradiância, distância,
  relocação/classificação e contagem de raios;
- teleporte maior que uma dimensão da grade = reset completo **só da fina**;
- a grossa nunca desliga e continua atualizando — é fallback incondicional;
- testes: caminhada lenta cruzando células, diagonal, movimento vertical, e objeto
  atravessando a borda da fina.

**Depois da 6.2b‑ii**, e independentes entre si:

- **Remedir a banda e refazer o sweep.** O sweep de `kTargetMax` vale para o layout de
  antes do reempacotamento; e o blend muda o padrão de acesso do gather.
- **A correção de índice da gaiola de borda.** É erro de ÍNDICE e sobrevive a qualquer
  spacing, mas muda a contagem de sondas (logo, o custo) e a cobertura na face `AABBMax` —
  A/B e fase próprios. A escolha entre "uma sonda a mais por eixo" (margem simétrica, +22%
  de sondas no Bistro) e "recentrar" (custo zero, cobertura justa) depende dessa medição.
- **A/B do piso `+0.05`** no peso de backface: não existe na Flax nem no paper (conferido
  no fonte), e afrouxa o cull.
- **Composição de energia**: o `HitShading` usa `((1-F)·diffuse + F)·indirect` e o deferred
  soma `DiffuseColor·GI` sem o `(1-F)`. Incoerência interna, não erro físico.
- **Terceira cascata (4 m)**, só se a 6.2b revelar transição ou alcance insuficientes. Com o
  custo agora em FRAME e não em espera, a conta mudou: `3 × 4416 = 13.248` sondas seriam
  ~7,7 ms de `DDGI (async)` e, extrapolando o degrau medido, ~+1,7 ms de frame sobre uma
  cascata. Precisa de uma das alavancas abaixo ANTES, não depois.
- **Remedir com um ciclo `1 → 2 → 1 → 2`** antes de fixar o custo em 0,83 ms (ver o
  confounder do Z‑prepass na tabela do A/B).

### Devolver os 0,83 ms — as alavancas, e a ordem importa

Todo o custo é o UPDATE, e ele é linear na contagem de sondas (o gather saiu de graça na
medição). As alavancas conhecidas atacam a mesma variável, então a escolha entre elas é de
RISCO DE IMPLEMENTAÇÃO, não de retorno.

**Ordem acordada (12/08/2026), e o princípio dela: terminar o COMPORTAMENTO antes de
otimizar a distribuição de trabalho** — o scrolling da 6.2b‑ii ainda vai mudar quem atualiza
o quê e quando, então otimizar antes seria calibrar sobre um alvo que se move.

1. commitar a 6.2b‑i com os fixes da revisão;
2. **6.2b‑ii** — a fina móvel, com scrolling toroidal, validada;
3. A/B dos **raios adaptativos** — concluído em 19/08/2026; ganho de compute confirmado,
   principalmente em movimento, par visual aprovado e default ON;
4. **compactar só as sondas que a Smile JÁ classifica como inativas** (ver a ressalva
   abaixo — é menos do que "sonda que não precisa de raio");
5. avaliar **separadamente** uma política "longe de geometria";
6. **update escalonado**, já com estado temporal por cascata e medição de frame pacing.

1. **Raios adaptativos** — já implementado (fase 4) e A/B'd em 19/08/2026. Corta raios
   por sonda onde não há geometria perto, o que reduz compute **e contenção** em TODO frame,
   suavemente, sem tocar em estado nenhum. Era "devolve tempo, que é o que já sobra"; com o
   orçamento medido em frame, virou a primeira coisa a tentar. O par científico aprovou 16–64
   e o recurso ficou **ON por default**. Custo de implementação: zero.
2. **Sondas inativas ainda traçam** (a Flax compacta e usa dispatch indireto). Não custa
   variância nenhuma — a sonda inativa não contribui para o gather de todo jeito.

   ⚠️ **Mas "inativa" aqui é uma coisa só, e mais estreita do que parece.** No
   `DDGIRelocate.cs.hlsl`: `inactive = (backRatio > thresh) && (backfaceCount >= 6)` — ou
   seja **sonda ENGOLIDA por geometria**, detectada por maioria de raios batendo em
   backface. Sonda alta em espaço aberto tem `backRatio ≈ 0` e continua **ativa**; quem a
   trata são os raios adaptativos, que a levam ao `MinRays` pela proximidade
   (`prox = closestFront`). São dois mecanismos distintos e não intercambiáveis. Duas
   consequências:
   - a compactação rende o que a classificação atual já marca, e nada além disso. Ganho
     estimável direto do visualizador de sondas, antes de escrever código;
   - a classificação **só existe com relocação LIGADA**: com ela off o passe faz
     `ProbeData = 0` e retorna, então nenhuma sonda é marcada inativa.

   **A política "longe de geometria" da Flax é outra coisa e não deve entrar junto.** Lá ela
   desativa sonda distante de geometria pelo Global SDF *e* elimina partes da cascata grossa
   cobertas pelas finas. A segunda metade **conflita direto com a arquitetura escolhida
   aqui**: a grossa é fallback INCONDICIONAL e precisa estar convergida em todo lugar (é o
   oposto do desenho da Flax, onde fora da última cascata cai um constante). Se um dia
   entrar, entra como avaliação separada e sem a parte de furar a grossa.
3. **Update escalonado — fina todo frame, grossa a cada 2.** A aritmética fecha: com as duas
   cascatas na mesma contagem, `2,57 + 2,57` vira `2,57 + 1,285` ≈ **3,9 ms amortizados**, e
   a grossa é iluminação de baixa frequência espacial, que é o que tolera meia taxa. Mas é a
   mais cara das três, e **não por causa do dispatch**:

   ⚠️ **Todo o estado contado em FRAMES da `FDDGI` é global e passaria a divergir entre
   cascatas.** `RecordUpdate` decrementa `InvalidateFramesLeft_` e
   `InvalidateDistFramesLeft_` uma vez por chamada, consome o `HysteresisResetPending` uma
   vez, e o dispatch cobre `NumProbes` — todas as cascatas. Com a grossa em meia taxa:
   - a janela de invalidação fecharia com a grossa tendo recebido metade das misturas:
     `0,90¹⁷ = 17%` de resíduo em vez dos 3% para que a janela foi dimensionada — fantasma
     visível da iluminação antiga, e justamente na cascata que serve de fallback para tudo;
   - o reset one-shot do `SetupForScene` seria consumido pelo update da fina, e a grossa
     nasceria misturando 1% da estimativa nova com 99% de preto — **exatamente o bug da
     fase 1**, de volta por outra porta;
   - `RelocateFramesLeft` e a reclassificação têm o mesmo problema.

   Nada disso é difícil: é tornar os contadores por cascata. Mas é precisamente a classe de
   defeito que esta série inteira produziu (ver "Um padrão que atravessou a série"), então
   merece fase própria com os contadores como O TRABALHO, e não como detalhe de uma
   otimização. Segundo alerta: o retorno em FRAME não é o amortizado — nos frames em que a
   grossa atualiza, a contenção é a de hoje, então o ganho aparece como frames ALTERNADOS
   (~8,5 / ~7,7) e não como um frame liso de 8,0. A EMA do profiler mostra a média e
   esconde exatamente isso.

## Um padrão que atravessou a série inteira

Vale registrar porque previu quase todos os defeitos: **nenhum bug real foi erro de
matemática — todos foram estado duplicado que divergiu.** Cinco loops preenchendo o mesmo
bloco de cascatas, dois arrays de cascata coexistindo num cbuffer, cinco consumidores
refazendo a decomposição do índice, duas escalas de heatmap derivadas em separado, um
`RayParams` faltando numa declaração espelhada.

As defesas que ficaram são todas a mesma resposta: o POD único, os `static_assert` de
OFFSET (não só de tamanho — o de tamanho não pega campo acrescentado antes do bloco), o
`tilesPerRow` derivado da largura do atlas em vez de transportado, a largura da grade de
dispatch derivada de `numProbes`, e a decomposição do índice centralizada.

## Onde está o quê

| assunto | arquivo |
|---|---|
| gather, Chebyshev, bias, fade, detector, região | `Shaders/GI/DDGICommon.hlsli` |
| update da irradiância + detector | `Shaders/GI/DDGIUpdate.cs.hlsl` |
| update dos momentos + histerese própria | `Shaders/GI/DDGIUpdateDist.cs.hlsl` |
| terminador e o gate de medição | `Shaders/GI/HitShading.hlsli` |
| constantes, janelas derivadas, cbuffer | `Engine/Include/Smile/Graphics/GI/DDGI.h` |
| reset, invalidação, empacotamento | `Engine/Source/Graphics/GI/DDGI.cpp` |
| domínios de invalidação | `Engine/Include/Smile/Graphics/Renderer/HistoryDomain.h` |
| contrato do gather compartilhado + gate | `Engine/Include/Smile/Graphics/GI/GIHitSampling.h` |
| eventos do editor | `GizmoController.cpp`, `SceneOutlinerBridge.cpp`, `LightsBridge.cpp` |
| bracket de stall do wait | `Renderer.cpp`, no `if (GIComputeFence != 0)` |
| classificação de raios por sonda | `Shaders/GI/DDGIRelocate.cs.hlsl` |
| empacotamento dos atlas | `DDGI_TileOrigin` / `DDGI_TilesPerRow` (`DDGICommon.hlsli`) + dimensões no `SetupForScene` |
| empacotamento do `ProbesTrace` | `DDGI_TraceTexel` (`DDGICommon.hlsli`) + `kTraceProbesPerRow` (`DDGI.h`) |
| seleção e blend de cascata | `DDGI_SelectCascade` + os dois wrappers (`DDGICommon.hlsli`) |
| estado por cascata (POD dos 5 cbuffers) | `FDDGICascadeConstants` + `CascadeConstants()` (`DDGI.h`) |
| colocação das cascatas no frame | `FDDGI::PrepareCascadePlacement` (`DDGI.cpp`) |
| drenagem das filas antes de realocar | topo de `Renderer::SetupGIForScene` (`Renderer.cpp`) |
| decomposição do índice no painel | `GetDebugProbeCoordValues` (`ViewportWidget.cpp`) |
| diagnóstico pontual (2 páginas de 8) | `Shaders/GI/DDGIDebugPoint.cs.hlsl` |
