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
| Domínio `DeterministicCapture` no `HistoryDomain` | Pendente |
| Contador de aquecimento por frame **renderizado** | Pendente |
| Instrumentação de convergência (calibrar o N) | Pendente |
| PNG + manifesto | Pendente |
| Presets científico/gameplay | Pendente |

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

## Calibração do N

O N é **fixo para todo o A/B**, escolhido uma vez por calibração. Parada adaptativa por captura
daria N diferente entre configurações, e a diferença de N viraria viés: a configuração que converge
mais devagar seria medida com mais tempo de acumulação, que é precisamente a variável em teste.

Calibrar com teto de 512 frames, medindo:

- delta do atlas DDGI entre frames;
- ocupação e amostras/célula do radiance cache;
- hit rate do cache;
- **delta temporal do sinal GI final**.

O último não é redundante: ocupação pode estabilizar enquanto a radiância ainda converge — a tabela
enche antes de as médias assentarem. Usar só ocupação daria um N cedo demais.

Ordem de grandeza esperada: **centenas** de frames, não dezenas. A histerese do DDGI é 0,99 (o
`HistoryDomain.h` cita ~199 updates para o atlas de distância) e o radiance cache tem teto de 64
amostras por célula.

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

**Gameplay** — upscaler e denoiser reais. Para validar o que o jogador vê, que é o único resultado
que importa no fim.

Os dois são necessários e medem coisas diferentes. Uma regressão que só aparece no gameplay é uma
interação com o upscaler; uma que só aparece no científico é do estimador e o upscaler está
mascarando.

## Manifesto

Ao lado do PNG, com nome derivado do estado real da engine e não digitado à mão — erro humano na
terceira rodada é o que o PNG automático existe para eliminar:

cena, slot, toggles de A/B (cache escrita/leitura, ReSTIR GI, DDGI), resolução, render scale,
upscaler, denoiser, N do aquecimento, e commit da build.

## Futuro

Captura HDR pré-tonemap, para quando as diferenças do estimador ficarem pequenas demais para
sobreviver ao tonemap e à quantização de 8 bits do PNG.
