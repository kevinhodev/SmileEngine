# Auditoria dos Opacity Micro-Maps

Estado em 31 de outubro de 2026, **segunda passada**. Revisão completa do caminho OMM da engine
cruzada com o SDK local (`D:/Engines/OMM`, versão 1.9.2 — header, `integration_guide.md` e o
**código-fonte** do baker), o header do Agility SDK 1.619.5, a spec funcional do DXR, a proposta
HLSL 0024 e o relato de produção do *Indiana Jones and the Great Circle* (NVIDIA/MachineGames), que
é hoje o único caso público de OMM em path tracing com números medidos.

A segunda passada **conferiu na fonte** o que a primeira tinha assumido pelo nome (o sampler, o
SRV), encontrou dados reais de bake num log de BistroExterior, e com eles corrigiu duas
prioridades e achou uma dependência entre o P0 e o P1 que a primeira não viu. O que mudou está
marcado com **[2ª]**.

## Veredito

**O caminho está correto, e correto no detalhe que mais quebra integrações de OMM: a
equivalência entre o que o baker classifica e o que o shader testa.** O guia da NVIDIA dedica três
seções a formas de errar isso, e a engine não cai em nenhuma.

Confere com a "Simple Standard Form (Type 1)", a única que o guia garante idêntica em 4-state:

| contrato | baker | shader | OK |
|---|---|---|---|
| mip | `mipCount = 1`, mip 0 ([OpacityMicromap.cpp:189](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L189)) | `SampleLevel(..., 0.0f)` ([RTAlphaTest.hlsli:39](Shaders/GI/RTAlphaTest.hlsli#L39)); SRV com `MostDetailedMip = 0` e sem `ResourceMinLODClamp` ([Texture.cpp:223](Engine/Source/Graphics/Resources/Texture.cpp#L223)) **[2ª]** | ✅ |
| filtro | `TextureFilterMode::Linear` | static sampler `s1`: `MIN_MAG_MIP_LINEAR`, `MaxAnisotropy = 1`, bias 0 ([ComputePipeline.cpp:137-154](Engine/Source/Graphics/Backend/D3D12/ComputePipeline.cpp#L137)) **[2ª]** | ✅ |
| endereçamento | `TextureAddressMode::Wrap` | `s1`: `ADDRESS_MODE_WRAP` nos três eixos (o `s0` é CLAMP — o nome bate com o registro certo) **[2ª]** | ✅ |
| fonte do alpha | BC **decodificado** pelo DirectXTex a partir dos bytes cozidos | o mesmo BC amostrado pela GPU | ✅ |
| comparação | `alpha > cutoff−1ULP` | `alpha >= cutoff` | ✅ |
| fator do material | pré-multiplicado no plano | `* mat.BaseColor.a` | ✅ |
| transformação de UV | nenhuma | nenhuma (o material não tem tiling/escala de UV) | ✅ |

O fluxo de dados da textura é o que o guia desenha como **correto** (decodificar o formato de
runtime, não bakear do source). O deslocamento de um ULP em
[OpacityMicromap.cpp:178](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L178) reconcilia o
`>` do baker com o `>=` do HLSL e é exato para todo float. Pré-multiplicar `BaseColorFactor.a` é
válido porque a bilinear é linear: `bilinear(α·k) = bilinear(α)·k`.

O lado D3D12 também: ordem OMM Array → barreira UAV global → BLAS
([RaytracingScene.cpp:690-705](Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L690)),
prebuild do BLAS **com** a linkage anexada (é o que faz o driver dimensionar o BLAS para OMM),
estado permanente `RAYTRACING_ACCELERATION_STRUCTURE` nos arrays, `PREFER_FAST_TRACE` no array
(−5% de `TraceMain` no Indiana Jones) e todos os `RayQuery` com `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`
(o pipeline flag não se aplica a inline ray tracing, então este é o gate certo). Por spec, a flag
de opacidade da geometria é *ignorada* em triângulos com OMM, e as ray flags de força só se aplicam
**depois** da classificação — não há caminho pagando alpha-test onde o micro-map já decidiu.

A procedência com hash de conteúdo (`OmmBakeInputs.h`) e o rebake transacional são mais rigor do
que a maioria das integrações tem.

**Os defeitos estão em política, não em correção:** o que se decide por passe, o que se faz no
cozimento, como se degrada e o que se mede. Nenhum achado abaixo produz pixel errado hoje.

## Dados reais **[2ª]**

A primeira passada não tinha números. Há um log de editor com BistroExterior em
`Docs/Audits/AC-Shadows-2026-10-03/editor-stderr.log` (1591 malhas, 132 materiais, 406 texturas):

| grandeza | valor |
|---|---|
| pares malha/material mascarados | **122** (45 "grandes" ≥4096 tri, 77 pequenos) |
| tempo de bake | **22,0 s** de uma carga de cena de **28,2 s** — 78% do load |
| fase serial dos 45 grandes, na main thread | ~17 s (3,9 s → 21,1 s do relógio do log) |
| fase paralela dos 77 pequenos, 8 workers | ~5,5 s (21,1 s → 26,7 s) — os workers ficam **parados** até a fase serial acabar |
| pares que caíram no fallback all-unknown | **10**, todos grandes |
| triângulos sem nenhum benefício de OMM | **134 222** — ~15% dos ~899 k triângulos mascarados |
| bakes reais / dado de array | 112 / 20,1 MB, 8775 micro-maps |
| bakes uniformes (só índices especiais) | 51 |
| OMM Arrays no build | 61 |
| maior malha mascarada | 1 615 992 triângulos → 48 micro-maps, 768 KB |

(Estes são os números de antes da divisão de workload; o efeito dela está no P1.)

Três coisas saem daqui que a primeira passada errou ou não viu: o bake é **a maior parte** do load
(reforça o P1 do Cooker), o fallback **não é raro** (um sexto da folhagem), e o desperdício dos
arrays committed é **pequeno** (61 arrays, não centenas).

---

## P0 — Nenhum passe usa `RAY_FLAG_FORCE_OMM_2_STATE` — **FEITO (knob, ligado por default)**

**O que entrou:** o knob `GIOmm2State` (`FRenderSettings`, toggle "OMM 2-state no GI" no menu de
visualização, `omm2State` no `gi_configure`/`gi_status` do pipe). O knob só **pede**; quem liga é
`Renderer::RefreshOmmGI2State`, e só com **zero** triângulos mascarados sem micro-map na cena.
A guarda é recontada no load de cena, no rebake do editor e quando o knob muda. Ligado, a flag
vai no argumento de runtime do `TraceRayInline` dos 12 traces de GI: o gather, o retrace e a
revalidação do ReSTIR GI, a visibilidade (Trace e Spatial), as sondas do DDGI, o update do
radiance cache e as três sombras no hit via `FHitShadeParams::OmmRayFlags`. Reflexos e ReSTIR DI
passam 0 e ficam em 4-state. O helper `SMILE_OmmRayFlags` mascara o valor que chega do CB, então
só o bit do 2-state atravessa. A troca invalida o GI pelo mesmo `OnGIHitSamplingChanged` do
`FoliageShadows`.

**A/B medido** (BistroExterior com sidecar, RTX 3060 Ti, preset `gameplay_rr`, sequência
intercalada off/on/off/on, 60 frames distintos por rodada, script
[omm-2state-ab.mjs](Tools/SmileMCP/scripts/omm-2state-ab.mjs)):

| passe | 4-state | 2-state | Δ | spread entre rodadas off |
|---|---|---|---|---|
| ReSTIR GI | 6,390 ms | 6,060 ms | **−0,33 ms (−5,2%)** | 0,010 ms |
| ↳ Temporal + trace secundário | 5,009 ms | 4,714 ms | −0,30 ms (−5,9%) | 0,010 ms |
| DDGI (async) | 1,374 ms | 1,271 ms | **−0,10 ms (−7,5%)** | 0,045 ms |
| Radiance cache (update) | 0,539 ms | 0,506 ms | −0,03 ms (−6,2%) | 0,041 ms (dentro do ruído) |
| ReSTIR DI (4-state, controle) | 2,972 ms | 3,022 ms | +0,05 ms | 0,080 ms (ruído) |
| **Frame (GPU)** | 24,27 ms | 23,71 ms | **−0,56 ms (−2,3%)** | 0,44 ms |

O ganho sustentado é de **~0,4 ms** nos passes de GI: o do ReSTIR GI está 30× acima do spread e o
do DDGI 2×. O do frame inteiro fica perto do ruído. O ReSTIR DI, que não muda de política, não
se moveu, o que serve de controle.

**Imagem:** mesma câmera, histórico zerado, 256 frames de aquecimento. Só 0,37% dos pixels mudam
mais de 2/255, o máximo é 34/255, e a luminância média varia 0,004%. O mapa de diferença
concentra tudo na folhagem (vasos, cestas penduradas, árvore), que é onde o 2-state age, sem
escurecimento visível do GI. Não houve captura de controle (off contra off), então parte disso pode
ser ruído do RT, mas a localização na folhagem indica que é o efeito.

**Correção desta auditoria: o −55% do Indiana Jones não era deste knob.** A tabela deles compara
**OMM desligado contra OMM ligado** (com 2-state no indireto). A maior parte do ganho é o próprio
OMM, que a SmileEngine já tinha e que o sidecar terminou de cobrir (0% UNKNOWN, 98% da área de UV
conhecida). Com o hardware já resolvendo quase tudo, sobra pouco alpha-test para o 2-state
eliminar. Os ~0,4 ms são o que faltava, não a fatia grande.

**A guarda, verificada no editor** ([omm-2state-guard.mjs](Tools/SmileMCP/scripts/omm-2state-guard.mjs),
Bistro **sem** `.somm`): a cena tem 113 154 triângulos sem micro-map, o pedido é registrado
(`requested: true`) e **recusado** (`active: false`), e o log avisa: "2-state no GI pedido, mas
113154 triangulos mascarados estao sem micro-map [...] Mantendo 4-state. Rode SmileOmmBaker na
cena". **Default: ligado** (decisão de 31/10/2026, depois do A/B). A guarda é o que torna isso
seguro: confirmado no editor, o Bistro com sidecar sobe com `2-state no GI LIGADO` no log, e
uma cena sem `.somm` atual fica em 4-state e avisa.

### Texto original desta seção

São **16 declarações** de `RayQuery` (a primeira passada contou 17; a 17ª era o parâmetro de
`PT_ResolveSelfIntersection`), todas com `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`, nenhuma forçando
2-state. **É a maior alavanca de performance que o formato 4-state oferece.**

Em 2-state forçado, o hardware ignora o estado Unknown e usa só o bit T/O: `UNKNOWN_OPAQUE` vira
opaco, `UNKNOWN_TRANSPARENT` vira transparente. Como a engine bakeia com
`unknownStatePromotion = ForceOpaque`
([OpacityMicromap.cpp:236](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L236)), *todo*
microtriângulo incerto resolve como opaco, e o `SMILE_RT_PROCEED` daqueles passes deixa de rodar:
`q.Proceed()` retorna falso de primeira, sem fetch de albedo.

É a divisão que o Indiana Jones fez:

- **sombra primária**: 4-state (conservador) — `SunTracing` −14%;
- **iluminação indireta**: force-2-state — "removeu os alpha tests de shader para os BLAS com OMM".
  `TraceMain` **−55%**, `SharcUpdate` **−55%**, Any-Hit em `TraceMain` de **17% para 3%**.

Candidatos, em ordem de ganho esperado:

| call site | o que é | veredito |
|---|---|---|
| [PathTracingCommon.hlsli:296](Shaders/GI/PathTracingCommon.hlsli#L296), [:341](Shaders/GI/PathTracingCommon.hlsli#L341), [:382](Shaders/GI/PathTracingCommon.hlsli#L382) | sombra de sol / luz local **no hit indireto** | o mais quente |
| [ReSTIRGIVisibility.hlsli:16](Shaders/GI/ReSTIRGIVisibility.hlsli#L16) | visibilidade do reuso | forte |
| [ReSTIRGITrace.cs.hlsl:153](Shaders/GI/ReSTIRGITrace.cs.hlsl#L153), [:336](Shaders/GI/ReSTIRGITrace.cs.hlsl#L336) | gather e revalidação | forte |
| [RadianceCacheUpdate.cs.hlsl:319](Shaders/GI/RadianceCacheUpdate.cs.hlsl#L319) | update do SHaRC | é o `SharcUpdate` dos −55% |
| [DDGITrace.cs.hlsl:122](Shaders/GI/DDGITrace.cs.hlsl#L122) | sondas | forte |
| `ReflectionTrace*`, `WaterReflectionTrace` | reflexo de espelho | **manter 4-state** |
| `ReSTIRDI*` | visibilidade da direta | **manter 4-state** |
| `TemporalSurface` | reprojeção | manter 4-state |

**[2ª] Pré-requisito que a primeira passada não viu: o fallback vira quad sólido.** O all-unknown
usa `FULLY_UNKNOWN_OPAQUE` em todo triângulo
([OpacityMicromap.cpp:135](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L135)). Em 4-state
isso é só lento — o alpha-test resolve. Em 2-state forçado, "ignorar o Unknown e usar o bit O"
transforma **cada um desses triângulos num opaco sólido**: os cards de folhagem viram retângulos
para o GI. No Bistro eram 10 malhas e 134 k triângulos, e como são as malhas grandes em UV, é
provável que sejam justamente as árvores. O Indiana Jones tropeçou na mesma classe ("o 2-state
forçado expôs modelos sem cobertura OMM"). **O P0 não pode entrar enquanto houver triângulo
mascarado sem micro-map.** A divisão de workload (P1, feita) baixou isso para 113 k, mas não
zerou — e o que sobra só sai com o bake offline. A ordem passa a ser: Cooker antes do 2-state.
Antes disso, um 2-state só é seguro em instância cuja malha tenha `WorkloadExcludedTriangles == 0`,
e essa granularidade não existe na ray flag (é por raio, não por instância).

**Atualização: o pré-requisito está cumprido no Bistro.** Com o sidecar atual são 0 triângulos
UNKNOWN inteiros. Mas o fallback de runtime continua existindo: uma cena sem `.somm`, ou com um
`.somm` desatualizado depois de um recook, volta a ter UNKNOWN. Então o P0 precisa de uma guarda
**por cena**: só ligar o 2-state quando a cobertura do load (que o log já soma) tiver
`FullyUnknown == 0`, e avisar alto quando não tiver. Não basta a flag de runtime.

**[2ª] Correção da implementação.** A primeira passada disse que, como as flags do `RayQuery<>`
são de tipo e `PT_ResolveSelfIntersection` recebe `RayQuery<RAY_FLAG_NONE, …>`, seria preciso
trocar o tipo por um `#define`. Não é. As flags efetivas são o OR do template com as do
`TraceRayInline`, e a proposta HLSL 0024 só exige que o template tenha `ALLOW_OPACITY_MICROMAPS` —
que os 16 já têm. Passar `RAY_FLAG_FORCE_OMM_2_STATE` **no argumento de runtime** do
`TraceRayInline` não toca no tipo, não quebra assinatura nenhuma, e vira um knob de cbuffer: A/B
sem recompilar.

Troca de qualidade a medir: a folhagem fica conservadoramente mais **grossa** para o indireto, o
que escurece um pouco o GI sob sebes. Se o A/B acusar, o eixo é
`unknownStatePromotion = Nearest`, que preserva cobertura ao custo de borda serrilhada (invisível
no indireto) — e que não muda nada no caminho 4-state.

## P1 — O fallback era tudo-ou-nada — **FEITO em parte, e o resto mudou de dono**

Antes: quando o `maxWorkloadSize = 2^30` estourava, `MakeAllUnknown` degradava a **malha inteira**
para `FULLY_UNKNOWN_OPAQUE` — benefício zero e, com o P0, quad sólido. No Bistro, 10 dos 45 pares
grandes.

**A escada que esta auditoria propunha estava errada, e o fonte do SDK mostra por quê.** O
workload **não depende do nível de subdivisão**: `ComputeWorkloadSize`
(`bake_cpu_impl.cpp:662-679`) soma a área da AABB de UV em texels do mip 0, uma vez por triângulo
de UV único. Baixar 8 → 6 → 4 devolveria o mesmo número e não tiraria malha nenhuma do fallback.
Também não há "desabilitar triângulo" pela API: `subdivisionLevels[i]` só é lido quando `<= 12`
(`bake_cpu_impl.cpp:544`), então o `kDisabledPrimitive = 0xE` interno é inalcançável por fora.

**O que entrou** ([OpacityMicromap.cpp](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp),
`SplitByWorkload`): no `WORKLOAD_TOO_BIG`, a engine mede os triângulos com a mesma conta do SDK
(mesma deduplicação), tira os mais caros até o resto caber no teto, bakeia **só o resto** com
`Force32BitIndices`, e costura o buffer: os excluídos com `FULLY_UNKNOWN_OPAQUE`, os bakeados no
lugar de origem. O orçamento de tempo é o mesmo teto. Os bakes que já cabiam ficam byte a byte
iguais (o caminho só existe depois do erro). Se nada couber, continua o all-unknown.
`FOpacityMicromapData::WorkloadExcludedTriangles` publica quantos ficaram de fora, e o log do
import soma a cena. Teste: 19 casos sintéticos (bakeados idênticos ao bake sem os excluídos, mais o
caso negativo em que nada cabe) e a checagem nos 122 pares do Bistro.

**O que ele rendeu no Bistro, medido:**

| | antes | depois |
|---|---|---|
| triângulos mascarados sem micro-map | 134 222 (14,9%) | **113 154 (12,6%)** |
| teste completo, Release | 39,9 s | 44,8 s (+12%) |
| teste completo, Debug | 233 s | **300 s** (+29%) — estoura o `TIMEOUT 300` do ctest |

Recupera 16% do que estava perdido (2,3% dos triângulos mascarados), ao custo de ~5 s de bake
por load em Release, e quebra o gate Debug do `Smile.OpacityMicromapBake.BistroExterior`. O
tempo extra é o bake da parte que agora cabe, limitado pelo mesmo teto de antes por malha.
**Decidido (Kevin, 2026-10-31): manter a divisão** e subir o `TIMEOUT` do teste para 450 s
([Tests/CMakeLists.txt](Tests/CMakeLists.txt)). Ela segue útil depois do Cooker, no rebake do
editor, que continua com teto. O motivo está no log novo, que dá
o múltiplo do teto por malha: as 10 estão entre **4,3× e 34×** acima dele. Não são "uns poucos
triângulos de UV enorme"; são malhas em que a **maioria** dos triângulos é cara (34 192 de 37 366
numa delas): cards grandes no atlas que somam várias vezes o teto. Subir o teto em runtime para
2^32 cobriria só as de ~4×; cobrir todas pede ~34× o trabalho, minutos de load. **O resto só se
resolve no bake offline, sem teto** — e isso puxa o P1 do Cooker para antes do P0 (ver lá).

Achado de passagem, não tratado: no SDK `aabb.x * aabb.y` é `int × int`
(`bake_cpu_impl.cpp:676`), e a AABB de um triângulo acima de 2^31 texels dá wrap (UB). Um triângulo
assim pode escapar do teto e fazer o bake demorar horas. A conta da engine, em `double`, não tem o
problema, mas só roda depois de o SDK acusar o estouro.

Ainda em aberto:

1. **`maxArrayDataSize`** não é usado (fica em `0xFFFFFFFF`). Está implementado
   (`bake_cpu_impl.cpp:1563-1660`) e faz downsample dos blocos até caber: é o teto de VRAM por
   micro-map que a NVIDIA adicionou no SDK 1.7 para substituir o "pular OMMs acima de 0,5 MB" deles
   (tinham um de 43 MB). No Bistro o maior é 2,7 MB, então hoje não morde — é salvaguarda.

## P1 — O bake é 78% do load de cena; ele pertence ao Cooker — **FEITO (sidecar `.somm`)**

**O que entrou**, com um desenho diferente do que esta seção propunha (ver o texto original abaixo):

- **`SmileOmmBaker`** ([Tools/OmmBaker](Tools/OmmBaker/main.cpp)): carrega a cena cozida pelo
  **mesmo** `LoadCookedSceneData` do runtime e bakeia **sem teto**, gravando `<cena>.somm`. É
  incremental: pares cujo bake no sidecar ainda bate com a cena são reaproveitados (`--force`
  refaz todos). Não é um passo do `SmileCooker` porque o Cooker não linka a engine: não tem OMM
  SDK, DirectXTex nem o loader de textura, e o bake tem de ver o alpha exatamente como a GPU.
  O [CookScenes.ps1](Tools/CookScenes.ps1) roda o baker depois de cada cena (`-NoOmm` pula).
- **Sidecar, e não bump de `kCookedVersion`.** O `.somm` se valida sozinho, par a par, por
  hash do conteúdo carregado (UVs+índices, pixels de alpha) e pelos bits de alpha/cutoff. O
  header guarda a versão do formato, a dos parâmetros do bake (`kOmmBakeSettingsVersion`) e a do
  SDK, mais um hash do payload inteiro. Não precisou recozinhar nada, não colide com os `.smesh`
  v9 antigos da árvore e não mexe no schema da baseline do `CookScenes.ps1`.
- **Formato próprio, e não `omm::Cpu::Serialize`.** O sidecar guarda o `FOpacityMicromapData` já
  na forma do D3D12. O load nem passa pelo SDK.
- **Uma só enumeração** ([OmmBakeCache.h](Engine/Include/Smile/Graphics/RayTracing/OmmBakeCache.h)):
  `CollectOmmBakeJobs` e o agendador `BakeOmmJobs` saíram do import e são chamados pelos dois
  lados, o que garante a mesma chave por construção.
- **O import** procura cada par no sidecar. Sidecar ausente, recusado inteiro ou com par
  desatualizado: o par vai pelo bake de runtime, com teto, como antes, e o log diz quantos e
  manda rodar o baker. Não é fallback para geometria sem OMM.

**Medido no BistroExterior:**

| | runtime com teto | sidecar offline |
|---|---|---|
| triângulos UNKNOWN inteiros | 113 154 (12,6%) | **0** |
| resolvidos inteiros pelo hardware | 56,4% | **69,0%** |
| com micro-map | 31,0% | 31,0% |
| dado de array | 20,1 MB | 20,1 MB (sidecar de 23 MB) |
| custo | ~22 s **a cada load** | 54 s **uma vez**, offline |
| ferramenta inteira com o sidecar pronto (load da cena + validação) | — | **0,6 s** |

Os triângulos que estouravam o teto eram **uniformes**: viraram índice especial resolvido pelo
hardware, sem micro-map. Por isso a cobertura subiu 12,6 pontos sem crescer o array.

O teste confere o sidecar real contra a cena: as 122 chaves batem, **os 112 pares que cabem
no teto são byte a byte iguais ao bake de runtime** (offline e runtime enxergam as mesmas
entradas), e os 10 antes divididos agora estão cobertos. Mais 14 casos sintéticos: ida e volta
exata, bytes determinísticos e as recusas (chave com um ULP de cutoff diferente, textura
diferente, par ausente, byte trocado, arquivo truncado, versão de parâmetros, arquivo
inexistente).

**O bake não é reprodutível entre Debug e Release.** O `omm-lib` compila com `/fp:fast` em todas
as configurações, e `/Od` contra `/O2` reassocia e contrai o ponto flutuante de formas diferentes.
Medido com o sidecar de Release e o teste em Debug: 12 dos 112 pares divergem, mas em **zero**
triângulos muda a classe (resolvido inteiro / micro-map / UNKNOWN), contagens, tamanho do array e
descritores são idênticos, e a área conhecida varia no máximo 1·10⁻⁴. A divergência fica nos
estados de microtriângulo na borda do contorno. Os dois resultados são micro-maps válidos, então
o sidecar não recusa um bake de outra configuração. O header grava a configuração de quem bakeou
(`BakerFlavor`, só informativo), e o teste exige identidade byte a byte na mesma configuração e
aplica essa tolerância medida (com teto 10× maior) entre configurações.

**Verificado no editor (Release, GPU, 31/10/2026)**, abrindo o BistroExterior pela linha de
comando, nas mesmas condições, com segundos de diferença entre as aberturas:

| | sem sidecar | **com sidecar** | sidecar corrompido |
|---|---|---|---|
| etapa de OMM | 28 698 ms | **27 ms** | 29 713 ms |
| `Commit scene GPU` | 29 658 ms | **661 ms** | — |
| **load da cena** | **30 220 ms** | **1 175 ms** | 31 169 ms |
| triângulos UNKNOWN | 13% | **0%** | 13% |

Com o sidecar, o load fica **~26× mais rápido**. O corrompido foi recusado inteiro com o motivo
("hash do payload não confere"), avisou e caiu no bake de runtime sem quebrar. Nas três aberturas
a cena renderizou 8 s sem erro no log: nenhum BLAS sem micro-map na TLAS e nenhum "OMM
desatualizado". A linha da TLAS com bytes de OMM rodou: `61 OMM Arrays (20.295 MB)`. **Ainda não
verificado:** o ramo de par desatualizado com o resto do sidecar válido. A contagem dele é a mesma
`Lookup` coberta pelo teste sintético, mas não houve um recook real para exercitá-la.

<details><summary>Texto original desta seção (antes da implementação)</summary>


**[2ª] Agora com número:** 22,0 s de bake numa carga de 28,2 s do BistroExterior, refeitos a cada
load. O Cooker não conhece OMM.

O SDK tem o caminho oficial: `omm::Cpu::Serialize` / `Deserialize` com `SerializeFlags::Compress`
(`omm.hpp:427-437`). O Indiana Jones migrou o bake para o cooker offline pela mesma razão (bakes de
30 min a horas por asset) e versionou o resultado com o resto do cozido.

E a engine já paga a ausência do cache **em qualidade**: o teto de 2^30 existe para o load não
demorar minutos, e é ele que joga as 10 malhas do Bistro no fallback. Offline, o teto pode ser o
2^32 do Indiana Jones, ou nenhum — o que resolve boa parte do P1 anterior de graça.

Caminho: bump de `kCookedVersion` (hoje 8,
[CookedFormat.h:8](Engine/Include/Smile/Scene/CookedFormat.h#L8)), bake por par no Cooker, blob num
sidecar, baker de runtime só para o rebake do editor. A validação já existe: `FOmmInputKey` + os
hashes do `FOmmBakeInputStore` são a camada que diz "este blob descreve este par".

</details>

## P2 — `ExtractAlpha`: 16 B/texel de intermediário, por par, em até 8 workers

[OpacityMicromap.cpp:90](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L90) descomprime o
BC para `R32G32B32A32_FLOAT` e monta um plano FP32 de um canal: 20 B/texel no total — 84 MB para
uma textura 2K, 335 MB para uma 4K, **por job**, com até 8 jobs simultâneos. A chave do job é
`(MeshIndex, MaterialIndex)`
([RendererSceneImport.cpp:425](Engine/Source/Graphics/Renderer/RendererSceneImport.cpp#L425)), então
um atlas compartilhado é decodificado uma vez por par que o usa.

Três consertos, em ordem de dependência:

1. **Não pré-multiplicar `BaseAlpha` no plano** (linha 110); dobrar no cutoff:
   `α·k >= cutoff` ⟺ `α >= cutoff/k`. O plano e o `omm::Cpu::Texture` passam a depender só da
   textura e podem ser compartilhados por todos os pares dela. Bordas: `k == 0` ou `cutoff/k > 1`
   → malha toda transparente.
2. **`TextureFormat::UNORM8`** (`omm.hpp:193-198`): sem perda para toda fonte de alpha da engine
   (BC1/BC3/BC7/RGBA8 guardam alpha em 8 bits), plano 4× menor. Depende de (1): pré-multiplicar em
   8 bits arredondaria diferente do shader.
3. **Descomprimir para `R8G8B8A8_UNORM`** — scratch 4× menor e decodificação exata do BC7.

**[2ª]** Rebaixado de P1 para P2: a memória é real, mas o tempo do Bistro está nas 17 s da fase
serial de bake, não na decodificação — e o Cooker some com as duas coisas de uma vez.

## P2 — Agendamento: os 8 workers esperam 17 s **[2ª]**

[RendererSceneImport.cpp:452-500](Engine/Source/Graphics/Renderer/RendererSceneImport.cpp#L452)
bakeia os 45 pares grandes **um por vez** na main thread (cada um com o OpenMP interno do SDK) e
só depois sobe os workers dos 77 pequenos. No log, os workers começam aos 21,1 s. A divisão em
duas faixas não sobrepõe as faixas no tempo, e o que o comentário promete ("separar as duas faixas
evita uma cauda single-thread") só vale se o OpenMP interno saturar a CPU — e isso **não foi
medido**. Antes de mexer, olhar a ocupação de CPU durante a fase serial. Se ela estiver baixa, rodar
os pequenos em paralelo com os grandes é o conserto barato. O Cooker torna isto irrelevante no
load, mas não no rebake do editor.

## P2 — `maxSubdivisionLevel = 8` contra o 6 de produção

[OpacityMicromap.cpp:238](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L238). Nível 8 são
65 536 microtriângulos por triângulo — 16× o dado de array do nível 6, para um ganho de cobertura
que o guia chama de retorno decrescente. O Indiana Jones shipou 6 com `dynamicSubdivisionScale 2.0`
(que a engine já usa). O teto de 8 só morde nos triângulos enormes em UV. Baixar para 6 deve sair
grátis em qualidade e diminui o array, e provavelmente o bake time — mas **não** os fallbacks: o
workload que decide o fallback não depende do nível (ver o P1). Pede A/B de recorte.

## P2 — Um `omm::Baker` por bake

[OpacityMicromap.cpp:187](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L187) cria e destrói
um baker por chamada. O guia diz que as tarefas de bake são thread safe e que não é preciso criar
vários bakers. Um baker compartilhado é o caminho documentado, e é onde o `MemoryAllocatorInterface`
do SDK ligaria a alocação do baker à contabilidade da engine.

## P3 — Não há como medir o ganho do OMM **[2ª]**

A engine exige OMM (não há BLAS alpha-tested legado), então **não há ON/OFF** para produzir a tabela
que o Indiana Jones produziu. A spec dá o instrumento sem violar a política: BLAS construído com
`D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS` aceita a instance flag
`D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS`, que cai na opacidade da geometria. Como a geometria
OMM já sai com `GEOMETRY_FLAG_NONE`, o resultado é exatamente o baseline sem OMM: todo triângulo
mascarado vira candidato e passa pelo alpha-test. Um toggle de debug nisso dá a medida do P0 e do
P1. O próprio `ALLOW_DISABLE_OMMS` pode custar algo no trace — medir com o toggle desligado antes
de deixar ligado.

## P3 — Ninguém sabia se cada micro-map estava pagando — **FEITO (metade CPU)**

Agora cada `FOpacityMicromapData` carrega um `FOmmCoverage`:

- **contagens** — resolvidos inteiros pelo hardware (especial T/O), com micro-map, UNKNOWN
  inteiros. Saem do index buffer **final**, e não das estatísticas do SDK, porque só o buffer
  final conhece os triângulos que a divisão de workload tirou do bake;
- **área de UV conhecida** — o `knownAreaMetric` do SDK. Só a `GetStats2` (que recebe o handle do
  resultado) tem a área por triângulo; a `GetStats` devolve 0 (`debug_impl.cpp:638`). Em bake
  dividido, a métrica cobre só o que foi bakeado, e a engine dobra nela a área de UV dos excluídos
  como 100% desconhecida.

O log do import soma a cena, e o build da TLAS imprime os **bytes** dos OMM Arrays, além da
contagem. O comentário de `EVramCategory::RaytracingAS` passou a dizer que eles moram lá. Custo:
nenhum mensurável (teste Release 44,8 s → 44,1 s). Teste: 5 casos sintéticos novos, inclusive a
conta da dobra conferida contra as áreas de UV exatas, e a consistência por bake nos 122 pares.

**Primeira foto do BistroExterior:**

| triângulos mascarados (898 896) | |
|---|---|
| resolvidos inteiros pelo hardware | **56,4%** |
| com micro-map | **31,0%** |
| UNKNOWN inteiros (alpha-test em todo candidato) | **12,6%** — exatamente os excluídos pelo teto |
| área de UV conhecida, média por triângulo | **83,9%** |

Cuidado ao ler a área: é ponderada em UV, então uma malha dividida aparece com 0–5% porque o
triângulo de UV repetida domina a soma. Para decisão de custo, as contagens são o número mais
honesto.

Ainda em aberto: o toggle `DISABLE_OMMS` (a metade GPU, ON/OFF para medir o ganho) e o
`rejectionThreshold = 0.0f`, que agora tem instrumento mas ainda não tem valor escolhido. Para
conferir um bake suspeito, `omm::Debug::SaveAsImages` desenha o overlay de estados sobre a textura.
O log da TLAS com bytes de OMM só foi compilado, não rodou ainda (precisa do editor com GPU).

## P3 — OMM Arrays como recurso committed individual

**[2ª] Rebaixado de P2.** [RaytracingScene.cpp:642](Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L642)
cria um `CreateUAVBuffer` por array enquanto o BLAS é pooled, e `OpacityMicromapBaseLocation` fica
fixo em 0 ([linha 651](Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L651)) — é o campo
que deixa vários BLAS compartilharem um array. A primeira passada falou em "centenas de alocações e
dezenas de MB"; no Bistro são **61 arrays**, e o arredondamento para heap de 64 KB custa no máximo
~4 MB sobre 20 MB de dado. É higiene, não urgência.

Na mesma linha: o BLAS do `EnsureBlasFor` não usa `ALLOW_COMPACTION`
([linha 291](Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L291)), e o pool usa
([linha 667](Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L667)). O Indiana Jones mediu BLAS
compactado em ~50%, e `RebuiltBlasBytes()` existe porque "o modo de falha é crescimento silencioso".

## P3 — `EnableValidation` sempre ligado

[OpacityMicromap.cpp:206](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L206). Seguro de
gatear em Debug: `bake_cpu_impl.cpp:685-696` avalia o teto de workload independentemente da flag.
**[2ª] Rebaixado de P2:** o custo não foi medido e provavelmente é pequeno — o workload já é
calculado de qualquer jeito por causa do teto. Manter o `messageCallback` sempre (o SDK recusa a
flag sem ele, `bake_cpu_impl.cpp:269`).

## P3 — Latente: `IndexFormat::UINT_8 → DXGI_FORMAT_R8_UINT`

[OpacityMicromap.cpp:266-267](Engine/Source/Graphics/RayTracing/OpacityMicromap.cpp#L266).
Inalcançável hoje (`Allow8BitIndices` não é setado), mas o ramo parece suporte e não é: o RHI da
NVIDIA trata só 16/32 bits de índice OMM no D3D12 (`nvrhi/src/d3d12/d3d12-raytracing.cpp:562`), e a
debug layer valida o campo sem documentar a lista. Apagar o ramo, ou só ligá-lo num experimento
com a debug layer.

## P3 — Comentário desatualizado em `RTAlphaTest.hlsli`

[RTAlphaTest.hlsli:15](Shaders/GI/RTAlphaTest.hlsli#L15) diz que instâncias com AlphaTest "são
marcadas FORCE_NON_OPAQUE na TLAS". **Não são:** `CollectInstances` sai com `INSTANCE_FLAG_NONE` e no
máximo `TRIANGLE_CULL_DISABLE`
([RaytracingScene.cpp:429-442](Engine/Source/Graphics/RayTracing/RaytracingScene.cpp#L429)). Quem
decide é o OMM. Num código em que os comentários carregam a decisão, este manda o próximo leitor
procurar uma flag que não existe.

## P3 — Latente: textura de albedo que falha no upload diverge do bake

Achado de passagem, ao conferir que o import e o `SmileOmmBaker` enxergam as mesmas entradas. Em
[RendererSceneImport.cpp](Engine/Source/Graphics/Renderer/RendererSceneImport.cpp), quando a
textura decodifica na CPU mas o upload para a GPU falha (`!texs[i].IsValid()`), o material cai em
`TexDefaultWhite` com `HasAlbedoMap = 0`, e o `AlphaTestPass` trata o material como **opaco**. Já o
bake usa o alpha **real** da CPU (`TextureData[i]`). O micro-map passa a descrever recortes que o
shader não aplica: microtriângulos classificados como transparentes somem, mesmo com o raster
desenhando a folha opaca. Não vi isso acontecer; precisa de upload falhando. O conserto é bakear com
a mesma textura que o material recebeu: plano branco quando ele caiu no default.

---

## Ordem sugerida **[2ª] — reordenada**

1. ~~Escada de retry no fallback~~ → **divisão de workload, FEITA** (P1). Recuperou 21 k dos 134 k
   triângulos do Bistro; o resto está 4–34× acima do teto.
2. **Instrumento:** cobertura por bake + bytes de OMM no log — **FEITO** (metade CPU). Falta o
   toggle `DISABLE_OMMS`, que fica para logo antes do P0, quando houver o que medir na GPU.
3. **Bake offline sem teto** → **sidecar `.somm` + `SmileOmmBaker`, FEITO** (P1). 0 triângulos
   UNKNOWN no Bistro; sem bump de `kCookedVersion`. Falta medir o load no editor com GPU.
4. **`FORCE_OMM_2_STATE` nos passes indiretos** (P0), pela flag de runtime do `TraceRayInline`, com
   A/B de frame e de GI na folhagem, **com a guarda por cena** (só com `FullyUnknown == 0` no load)
   e com o toggle `DISABLE_OMMS` para medir. É onde está o ganho de dois dígitos.
5. **`maxSubdivisionLevel 6`** — uma linha, com A/B de recorte. **Não** reduz fallback (o workload
   não depende do nível); reduz array e bake time.
6. **Plano de alpha por textura + UNORM8 + baker compartilhado**, e o agendamento — importam para
   o rebake do editor depois que o Cooker existir.
7. Pool de OMM Arrays, compactação no `EnsureBlasFor`, `EnableValidation`, ramo de 8 bits,
   comentário — higiene.

## Referências

Procurado e **não encontrado** nas engines e SDKs vizinhos, para a próxima sessão não repetir: nenhum
tem caminho OMM em uso. A UE 5.8 só carrega os campos `OpacityMicromapIndexFormat` /
`OpacityMicromapArray` nos descritores NVAPI de cluster/CLAS
(`Engine/Shaders/Shared/NaniteRayTracingDefinitions.h:47-61`) — struct, sem baker e sem política de
passe. Cry, Flax, RTXDI, RTXGI, SHARC e NRD não têm uso em shader. O Indiana Jones é a única
referência de produção com números.

- `D:/Engines/OMM/docs/integration_guide.md` (SDK 1.9.2) — formas de alpha-test, textura
  comprimida, 4-state vs 2-state, nível de subdivisão.
- `D:/Engines/OMM/libraries/omm-lib/src/bake_cpu_impl.cpp` — `ValidateWorkloadSize` (683),
  `maxArrayDataSize` (1563).
- `Docs/Audits/AC-Shadows-2026-10-03/editor-stderr.log` — os números do BistroExterior.
- [NVIDIA: Path Tracing Optimizations in Indiana Jones — OMMs and compaction of dynamic BLASs](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass)
- [DirectX Raytracing functional spec](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html) — classificação OMM antes das ray flags; flag de geometria ignorada; `ALLOW_DISABLE_OMMS`.
- [HLSL proposal 0024: opacity micromaps](https://microsoft.github.io/hlsl-specs/proposals/0024-opacity-micromaps) — force-2-state exige `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` no template.
- [D3D12 Opacity Micromaps (DirectX dev blog)](https://devblogs.microsoft.com/directx/d3d12-opacity-micromaps/)
