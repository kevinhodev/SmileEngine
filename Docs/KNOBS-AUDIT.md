# Auditoria e contrato dos knobs de render

> [!NOTE]
> **Tipo:** auditoria histórica com implementação concluída · **Estado inicial:** 2026-08-05
> A varredura começou sem alterar código, mas a fila ao fim do documento foi implementada.
> O estado atual usa `FRenderSettings`, 17 `EHistoryTarget` e máscaras em `HistoryDomain.h`.

Estado em 5 de agosto de 2026. Este documento registra a varredura de **como os
parâmetros de render chegam do editor ao motor** e de **quais deles entram em sinal
acumulado**. É o insumo da fatia 1 do desmembramento do `Renderer` (§13 do
`ARCHITECTURE.md`): a fachada `FRenderSettings` e o grafo `EHistoryTarget`/`HistoryDomain`.

A auditoria original foi somente leitura e continua reprodutível — os comandos estão no fim.
As mudanças aplicadas posteriormente estão registradas na seção “Fila”.

## Veredito

Existem **quatro** padrões de acesso ao mesmo tipo de estado, não dois. O critério que
separa "sobe pro `Renderer`" de "fica no subsistema" é real — o knob sobe quando tem
invalidação junto — mas é **implícito**, não está escrito em lugar nenhum, e já vazou em
três pontos.

A classificação binária "autocontido × entra no sinal gravado" não fecha. O que decide não
é *se* o valor entra num histórico, é **quanto tempo esse histórico vive**. O
`FVolumetricFog` já pratica a regra certa e a documenta: reseta quando a **reprojeção fica
inválida**, não quando o valor muda. Essa é a política do tier B abaixo, e ela está correta.

Das 58 entradas auditadas, **3 divergem** do critério que o próprio código aplica em knobs
irmãos. Duas suspeitas foram **derrubadas** pela varredura — nuvens e CSM não alcançam o
ray tracing —, o que encolhe o escopo em 22 knobs.

## Escopo auditado

- os quatro padrões de acesso do editor ao estado de render;
- 58 knobs distintos que chegam por reach-through (`Renderer->GetSub().SetX()`);
- para cada um: quem consome, e se algum consumidor **grava** o valor num histórico;
- a duração de cada histórico (reservoirs, atlas do DDGI, NRD/RR/TAA, temporais próprios);
- as listas de invalidação escritas à mão no `Renderer`, como referência de política;
- o alcance real de nuvens e CSM dentro dos passes de RT (via shaders).

Fora de escopo: os 141 `Set*`/`Get*` que já vivem no `Renderer` (auditados só como
*referência de política*, não item a item) e o acesso estrutural (`GetScene`,
`GetMaterials`, `GetDevice`, `GetGpuProfiler`, `GetSRVHeap`, `GetDebugDraw`), que não é knob
e deve continuar existindo.

## Os quatro padrões de acesso

| padrão | call sites | exemplo |
|---|---|---|
| `Renderer->SetX()` / `GetX()` | 141 | `SetGIBackfacePolicy` |
| `Renderer->GetSub().SetX()` | 96 | `ViewportWidget.cpp:1068` |
| `auto& S = Renderer->GetSub(); S.SetX()` | ver nota | `ViewportWidget.cpp:1058`, `:1120` |
| `Renderer->GetWeather().Campo = V` — **campo público cru** | 18 | `ViewportWidget.cpp:1950` |

> **Nota.** O terceiro padrão não é pego por um grep de `Get…().Set…` — a referência é
> ligada numa linha e usada na seguinte. Qualquer varredura futura precisa dos **dois**
> greps do fim deste documento, senão subconta.

O quarto é o pior: `FWeather` é um `struct` de campos públicos
(`Engine/Include/Smile/Graphics/Weather.h:16`) que o editor escreve por atribuição direta.
Não há setter onde pendurar invalidação, e nenhuma existe.

Distribuição dos 58 knobs distintos por reach-through: `VolumetricClouds` 15,
`Weather` 9, `Water` 8, `SunShadows` 7, `VolumetricFog` 7, `SunShafts` 6, `ReSTIRGI` 2,
`DDGI` 1, `Reflections` 1, `Fog` 1, `AO` 1.

Não contam como knob (telemetria read-only, mantida como está): `DDGI.Spacing()`,
`DDGI.DistanceMomentMax()`, `Water.GetDebugStats()`, `Terrain.IsLoaded()`.

## Taxonomia de histórico

**Tier A — autocontido.** Lido no consumo, não sobrevive ao frame. Nenhuma invalidação.

**Tier B — histórico curto e limitado.** Temporal próprio do passe (fog, shafts, nuvens),
`FReflections` (`MaxFrames = 12.0f`, `Reflections.h:357`), NRD, Ray Reconstruction, TAA.
O valor antigo se dilui em dezenas de frames. **Reset só quando a reprojeção deixa de ser
válida** — mudança de slicing, de resolução, de conteúdo de froxel oculto. Reset por
mudança de valor aqui é ruído: custa a reconvergência e não corrige nada que o próprio
blend não corrija.

**Tier C — sinal gravado de vida longa.** Reservoirs do ReSTIR (com `ValidateInterval = 0`
não há re-shade periódico que corrija o `Lo` gravado) e atlas do DDGI
(`Hysteresis = 0.99` — 99% do valor velho sobrevive **por update**, `DDGI.h:150`).
Sem clear explícito, o valor antigo permanece por tempo indeterminado. **Invalidação
obrigatória**, e ela tem que descer a cadeia inteira: quem acumula *sobre* o reservoir
(NRD, RR) e sobre o resultado (TAA) cai junto — é o argumento já escrito em
`Renderer.h:721-730`.

## Inventário classificado

| subsistema | knobs | tier | invalidação hoje |
|---|---|---|---|
| **Weather** — `RainAmount`, `DriveSky` | 2 | **C** | ❌ nenhuma |
| **DDGI / Reflections** — `FoliageShadows` | 2 | **C** | ❌ atribuição pura |
| **ReSTIRGI** — `FoliageShadows` | 1 | **C** | ⚠️ só `NeedsClear` local |
| **Weather** — wetness (`WetDarkening`, `PuddleAmount`, `PuddleScale`, `RippleStrength`, `RainOcclusion`) | 5 | B | ❌ nenhuma |
| **ReSTIRGI** — `Visibility` | 1 | B | ❌ nenhuma |
| **SunShadows** — 7 knobs de cascata/bias | 7 | B | ❌ nenhuma |
| **Water** — FFT e espectro | 8 | B | ❌ nenhuma |
| **VolumetricClouds** | 15 | A/B | ⚠️ parcial (`UseTemporal`) |
| **VolumetricFog** | 7 | B | ✅ correta e deliberada |
| **SunShafts** | 6 | B | ✅ `VolTemporal` |
| **Fog** — `HeightFogSkyContribution` | 1 | A | n/a |
| **AO** — `HalfRes` | 1 | A | n/a (as duas cadeias ficam alocadas) |
| **Weather** — `CurtainAmount`, `RainParticles` | 2 | A | n/a |

## Divergências encontradas

### 1. `Weather.RainAmount` / `Weather.DriveSky` — tier C, campo público, zero invalidação

O caminho está documentado no próprio `RenderFrame` (`Renderer.cpp:1716-1721`): a política
de escurecimento do céu na chuva é **única** e alcança os três consumidores do céu, porque
o céu procedural não sabe da chuva. Rastreado:

| destino | linha |
|---|---|
| `RainSkyDim` → `DDGI.SetSkyIntensity` | `Renderer.cpp:2309` |
| `RainSkyDim` → reflexões | `Renderer.cpp:2356` |
| `RainSkyDim` → ReSTIR GI | `Renderer.cpp:2373` |
| `RainKeyDim` → `EffectiveSunColor` → `SunColor` dos três traces | `Renderer.cpp:1737` |
| `RainAmbDim` → ambiente hemisférico e SH | `Renderer.cpp:1829`, `:1843` |

Nenhum desses caminhos invalida coisa alguma. Com `Hysteresis = 0.99` no DDGI, arrastar o
slider de chuva deixa o indireto com energia de tempo seco por centenas de updates — e a
calibração é feita contra energia que o usuário já mandou remover. É exatamente o problema
que `MarkIndirectLightingDirty` (`Renderer.h:753`) existe para resolver, aplicado a uma
entrada que não passa por ele.

**É a divergência de maior impacto da varredura.**

### 2. `FoliageShadows` — tier C, política dividida em três e escrita na UI

`ViewportWidget.cpp:1067-1070` faz o fan-out manual para `FDDGI`, `FReSTIRGI` e
`FReflections`. Dois defeitos:

- **Quem sabe que os três andam juntos é o widget.** Política do motor morando na UI.
- **As três implementações divergem.** `FReSTIRGI::SetFoliageShadows` marca `NeedsClear`
  (`ReSTIRGI.h:145`); `FDDGI` (`DDGI.h:173`) e `FReflections` (`Reflections.h:203`) são
  atribuição pura. Ninguém derruba `Nrd`/`RRResetPending`/`TAARanLastFrame`.

O knob é tier C: entra no `HitShading.hlsli` pelo `ShadowRayMask`, ou seja, muda o `Lo`
**gravado** no reservoir e o valor devolvido às sondas. Dez linhas abaixo, no mesmo arquivo,
`ToggleGIBackfacePolicy` (`ViewportWidget.cpp:1089`) — mesmo formato de knob, também só toca
o gather — vai pelo `Renderer` com comentário explicando por quê. Os dois deveriam ter a
mesma política.

### 3. `ReSTIRGI.SetVisibility` — tier B, sem invalidação

`ViewportWidget.cpp:1055`. O comentário em `ReSTIRGI.h:159` está **certo**: o `Visibility`
só atua no Pass B e no resolve final, não toca o que está gravado no reservoir. Mas é modo
persistente que muda a radiância resolvida, e o NRD acumula sobre ela. Pelo critério do
próprio `SetGIBackfacePolicy`, faltam `Nrd`, `RRResetPending` e `TAARanLastFrame`.

### 4. Knobs de wetness — tier B, guides do RR mudam sem reset

`RainWetness.ps.hlsl` lê cópias de GBufferA/B e **reescreve os originais**: `o.A` é
BaseColor e `o.B` é normal + roughness (`RainWetness.ps.hlsl:202`). São exatamente os guides
que o Ray Reconstruction consome e o G-buffer que as reflexões leem. Os 5 knobs merecem
`RRResetPending` + `Nrd.InvalidateHistory()`.

Não são tier C: wetness é screen-space, aplicada depois do geometry pass, e os hits de RT
leem o material do `InstanceGeo` — os reservoirs não veem wetness.

### 5 e 6. Duas divergências novas, achadas ao montar a tabela (2026-08-05)

Pôr as 19 listas lado a lado num arquivo só torna óbvio o que estava espalhado. Duas bordas
de subida invalidam **menos** que as irmãs, sem motivo aparente:

| setter | derruba | a irmã derruba |
|---|---|---|
| `SetUseReSTIRGI` (rising) | `ReSTIRGI`, `NrdIndirect` | `SetUseReSTIRDI`: + RR + TAA |
| `SetUseReflections` (rising) | `NrdIndirect` | `SetReflectionsCullBackface`: + `Reflections` + RR + TAA |

O argumento que justifica as irmãs — "o RR e o TAA acumulam sobre o resultado" — vale igual
aqui: religar o ReSTIR GI ou as reflexões muda o sinal que o RR está reconstruindo e que o TAA
está acumulando.

**Corrigidas em 2026-08-05**, junto com uma terceira, no mesmo commit:

| | correção |
|---|---|
| `SetUseReSTIRGI` | reservoirs seguem só na subida (na descida param de ser lidos); `ScreenResolve` nas **duas** bordas, espelhando o `SetUseReSTIRDI` |
| `SetUseReflections` | domínio `Specular` nas duas bordas — o simétrico também vale: desligar deixa reflexão real no histórico, que passa a se misturar a zero |
| `SkyRadiance` | **removido** `TemporalMotion` |

A terceira é a única *remoção* de invalidação da série, então foi verificada no código e não
pelo nome: o histórico do `FTemporalMotionVectors` é `Surface[]` (world position + InstanceID
por pixel) mais os transforms por instância — **geométrico**, sem nada derivado de radiância.
Mudar a cor da luz não move superfície nem troca InstanceID. Continua no `MaterialRTState`,
onde é legítimo: lá as flags de instância da TLAS mudam, e o buffer é indexado pelo InstanceID.
O modo de falha da remoção é benigno — preservar um histórico que continua válido.

Os dois `if (V == atual) return;` acrescentados de quebra evitam reset quando um binding do
QML reescreve o mesmo valor.

## O que a varredura absolveu

Duas suspeitas caíram. Valem registro porque **encolhem** o escopo em 22 knobs:

**Nuvens não alcançam o RT.** `CloudShadow` aparece em 5 arquivos de shader, todos raster:
`DeferredLighting.ps.hlsl`, `ForwardBlend.ps.hlsl`, `Fog/VolumetricFogScattering.cs.hlsl`,
`SunShafts/SunShaftsVolumetric.ps.hlsl` e o próprio `Clouds/CloudShadowMap.cs.hlsl`. O raio
que escapa nos passes de trace sombreia lendo o **SkyViewLUT** da atmosfera
(`GI/HitShading.hlsli:58`), que não contém nuvens. Os 15 knobs de nuvem são tier A/B.

**CSM não alcança o RT.** `Shadow/CSMCommon.hlsli` é incluído por `DeferredLighting`,
`ForwardBlend`, `VolumetricFogScattering`, `SunShaftsVolumetric` e `WaterSurface` — todos
raster. Nos hits de RT o sol vai por shadow ray inline (`GI/HitShading.hlsli:207-212`). Os 7
knobs de `SunShadows` são tier B, e chegam a histórico só pela via indireta do froxel e do RR.

## Domínios propostos

> [!NOTE]
> Esta seção preserva a proposta original. A implementação consolidada usa o enum
> `EHistoryTarget` para os acumuladores e constantes no namespace `HistoryDomain` para as
> máscaras nomeadas pelo motivo.

Derivados da varredura, não inventados. `RayVisibility` e `SkyRadiance` reproduzem quase
exatamente as listas manuais de `SetRayEpsilons` (`Renderer.h:619`) e
`NotifyIndirectLightingChanged` (`Renderer.h:755`) — sinal de que a taxonomia bate com o que
já foi decidido à mão, e de que as divergências acima são **omissões, não políticas
diferentes**.

```
EHistoryDomain::RayVisibility    // FoliageShadows, GIBackfacePolicy, RayEpsilons
    -> ReSTIR GI, ReSTIR DI, atlas do DDGI, Reflections, Nrd, NrdDirect, RR, TAA

EHistoryDomain::SkyRadiance      // RainAmount, DriveSky
    -> atlas do DDGI, ReSTIR GI, Reflections, VolumetricFog, Nrd, NrdDirect, RR, TAA

EHistoryDomain::GBufferGuides    // knobs de wetness
    -> Reflections, Nrd (specular), RR, TAA

EHistoryDomain::ScreenResolve    // ReSTIRGI.Visibility, ReSTIRGI.Spatial
    -> Nrd, NrdDirect, RR, TAA

EHistoryDomain::Reprojection     // VolFog MaxDistance/ConservativeDepth, HalfRes, resize
    -> só o histórico do passe dono (política atual do FVolumetricFog, correta)
```

Regra de manutenção, que é o motivo de a tabela existir: **ao levar um parâmetro para
dentro do `ShadeSurfaceHit`, reveja o domínio dele.** Os dois knobs citados em
`Renderer.h:640-643` nasceram sendo de sampler puro e deixaram de ser depois, em commits que
não voltaram no setter. Com a tabela, essa revisão vira uma linha mudando de domínio em vez
de sete chamadas para lembrar.

## Fila

1. ~~**Mecânico, sem mudança visual.**~~ **FEITO em 2026-08-05.** Existe
   `Graphics/RenderSettings.{h,cpp}` e o acesso é `Renderer->Settings().X()`: 223 call sites
   na fachada, 0 campos públicos do `FWeather`, 0 reach-through de knob. Sobrou **um**
   `GetWater().GetDebugStats()` no `MainWindow`, que é telemetria, não knob — mesma categoria
   de `GetScene()`. A invalidação foi preservada **bit a bit**; as 4 divergências continuam
   exatamente como estavam, de propósito. Detalhes de desenho na nota de transição do
   `RenderSettings.h`. À época, compilava Debug + Release, os 2 testes existentes passavam e o editor foi exercitado
   em runtime mexendo nos knobs sem diferença observada — que era o critério de aceite (o
   passo é mecânico: se a imagem tivesse mudado, seria erro de digitação no move).
2. ~~**Corrigir as 4 divergências.**~~ **FEITO em 2026-08-05**, em dois commits (chuva
   separada do resto, para poder reverter isolado). Nenhuma virou lista nova escrita à mão —
   cada uma reusa a política do irmão mais próximo, e todas ganharam guarda de valor igual:

   | knob | política reusada | por quê |
   |---|---|---|
   | `RainAmount` / `DriveSky` | `MarkIndirectLightingDirty()` | é literalmente "a energia que alimenta o indireto mudou"; coalescido porque é slider. Só dispara quando o knob de fato mexe no `RainSky` (com `DriveSky` off, ou sem chuva, nada muda) |
   | `FoliageShadows` | `OnGIHitSamplingChanged()` | entra no `ShadeSurfaceHit` pelo `ShadowRayMask` — muda o `Lo` gravado e o valor devolvido às sondas, que é a definição desse domínio |
   | wetness (5) | `InvalidateGBufferGuides()`, novo helper privado | reflexões + NRD spec + RR + TAA. Não coalescido: é só escrita de flag e os históricos são curtos (`MaxFrames = 12`) |
   | `Visibility` | critério do `SetGIBackfacePolicy` | `Nrd` + RR + TAA, **sem** tocar reservoir — o `ReSTIRGI.h` está certo, o que faltava era o que acumula depois do resolve |

   O `InvalidateGBufferGuides` é o primeiro esboço do passo 3: um domínio, uma lista, N knobs
   apontando para ela. Compila Debug + Release. **Muda imagem.**
3. ~~**Tabela `EHistoryDomain`** como dado.~~ **FEITO em 2026-08-05**, bit a bit (nenhuma
   mudança de imagem). Está em `Graphics/HistoryDomain.h`, no formato de `RTMasks.h`:
   nasceu com 14 `EHistoryTarget` (um bit por histórico que sobrevive ao frame) e 11 domínios
   nomeados **pelo motivo**, não pelos alvos. O código atual possui 17 alvos. O executor único
   é `FRenderSettings::Invalidate(mask)`, uma linha por alvo.

   Invariante verificável — o mapeamento bit → chamada existe em **um** lugar:

   ```bash
   grep -nE "R\.(Nrd|NrdDirect|ReSTIRGI|ReSTIRDI|Reflections|DDGI|ReGIR|VolumetricFog|VolumetricClouds|TemporalMotion|HiZ)\.(InvalidateHistory|ResetHistory|ResetHistoryOnce|InvalidateResults)\(\)|R\.RRResetPending|R\.TAARanLastFrame" Engine/Source/Graphics/RenderSettings.cpp
   ```

   Só pode casar dentro do corpo de `Invalidate()`. Qualquer outro hit é uma lista escrita à
   mão voltando — que é exatamente o defeito que este passo eliminou.

   Sobraram **quatro** máscaras ad-hoc, de propósito, todas comentadas no ponto de uso:
   `SetUseReSTIRGI`, `SetUseReflections`, `SetUseReSTIRDI` (borda de subida parcial) e
   `SetUseClouds`/`SetOcclusionCulling` (alvo único). Não viraram domínio porque são
   condicionais de borda, não políticas.
4. **`RenderSettingsBridge`** no editor (§13), um `Q_PROPERTY` por knob, tirando a maior
   parte dos 134 do `ViewportWidget`. **FEITO em 2026-08-05**, em 8 fatias (uma por domínio,
   cada uma compilando): Oceano, Clima, Nuvens, Sun shafts, Fog volumétrico, Sombras, GI e
   Render/upscaler.

   | | antes | depois |
   |---|---|---|
   | `ViewportWidget` `Q_PROPERTY` | 134 | **44** atualmente |
   | `ViewportWidget.h` | 645 linhas | **395** atualmente |
   | `ViewportWidget.cpp` | 2796 linhas | **1923** atualmente |
   | `RenderSettingsBridge` | — | **127** `Q_PROPERTY` atualmente |

   O que sobrou no `viewportModel` é exatamente o que não é knob: view mode, visualizador de
   render targets, diagnóstico de sonda, instrumentação (BVH / timer de RT) e telemetria
   (fps, contagens, resolução, VRAM, timings de GPU). Verificável por grep:

   ```bash
   grep -rhoE "viewportModel\.[a-zA-Z]+" Editor/Qml/*.qml | sort -u
   ```

   Fica no `ViewportWidget` de propósito, porque não é knob: view mode, visualizador de
   render targets, instrumentação (BVH / timer de RT) e toda a telemetria.

   **Receita por domínio** (repetível, uma fatia = um commit que compila):

   1. copiar o bloco `Q_PROPERTY` + declarações + `signals` do `ViewportWidget.h` para o
      `RenderSettingsBridge.h`;
   2. mover os corpos do `ViewportWidget.cpp` para o `RenderSettingsBridge.cpp` **sem
      reescrever** (mesmos clamps, mesmos defaults de "antes do renderer existir");
   3. apagar do `ViewportWidget` — inclusive o `emit <Dominio>SettingsChanged()` do
      `OnRendererInitialized`, que passa a sair do `SetRenderer` da bridge;
   4. `sed` no QML: `viewportModel.<membro>` → `renderModel.<membro>`. **Antes de injetar
      `renderModel` num painel, declare `required property var renderModel` na raiz dele** —
      `setInitialProperties` só liga em propriedade declarada, e sem isso as bindings viram
      `undefined` silenciosamente (slider no zero, toggle inerte, sem erro de compilação).
      Pelo mesmo motivo, **não** injete em painel que ainda não usa: propriedade inicial
      desconhecida gera warning e pode abortar o resto do `setInitialProperties`, derrubando
      as outras ligações daquele painel;
   5. procurar `connect(...)` no `MainWindow` que referencie o sinal movido (o do Oceano
      ligava `SceneOutlinerBridge::EnvChanged` nos dois sentidos);
   6. buildar Debug + Release.

   Restam GI (11) e Render/upscaler (17) — os dois que concentram os `EnqueueRendererJob`
   que sobraram. `ResetRenderSettings` é cross-domínio e vai junto com Render/upscaler.

   Cuidado numa armadilha do `sed`: **duas `Q_PROPERTY` estão quebradas em duas linhas**
   (`heightFogSkyContribution` e `perPixelAtmoTransmittance`). Grep de linha única não as vê,
   nem para extrair nem para apagar. O mesmo vale para declaração de sinal com comentário na
   mesma linha (`void ShadowSettingsChanged(); // CSM, ...`), que escapa de um `grep -v` por
   igualdade exata.

   Dois acoplamentos achados nas fatias já feitas, que a receita não previa e provavelmente
   voltam a aparecer:

   - **`connect` órfão no `MainWindow`.** O Oceano ligava `SceneOutlinerBridge::EnvChanged`
     ↔ `ViewportWidget::OceanSettingsChanged` nos dois sentidos, porque o oceano pode ser
     ligado tanto pelo Outliner quanto pelo Settings. Mover o sinal quebra os dois lados.
   - **Chamada cross-domínio no callback do job.** `SetCloudsHalfRes` chamava
     `NotifyDebugTargetsChanged()` — recriar o RT das nuvens remapeia a lista de alvos de
     debug, que é do viewport. Virou `Viewport->NotifyDebugTargetsChanged()`, e por isso o
     `EnqueueRendererJob` passou a ser público.

## Como reproduzir

Do raiz do repositório. Os dois primeiros são necessários — o segundo pega o padrão de
referência local, que o primeiro não vê:

```bash
grep -rnoE "Get(ReSTIRGI|ReSTIRDI|Reflections|DDGI|Water|Terrain|SunShadows|SunShafts|VolumetricFog|VolumetricClouds|Fog|AO|Weather|TimeOfDay|RaytracingScene)\(\)\.[A-Za-z_]+" Editor --include=*.cpp --include=*.h
```

```bash
grep -rnE "(auto&|F[A-Za-z]+&)\s+[A-Za-z]+\s*=\s*[A-Za-z]*(Renderer|Access)(->|\.)Get[A-Za-z]+\(\)" Editor --include=*.cpp
```

E um terceiro, descoberto durante a migração: os knobs que passam por
`EnqueueRendererJob` chegam num lambda que recebe `Smile::Renderer&`, ou seja, chamam com
`.` e não com `->`. Nenhum grep de `->` os vê:

```bash
grep -rnE "_Renderer\.(Set|Get)[A-Za-z_]+" Editor --include=*.cpp
```

Alcance de nuvens e CSM dentro dos shaders (a checagem que absolveu 22 knobs):

```bash
grep -rln "CSMCommon\|CloudShadow" Shaders
```
