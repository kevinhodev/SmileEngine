# Auditoria e contrato do CSM

Estado em 4 de agosto de 2026. Este documento registra a revisão completa da sombra do
sol (cascaded shadow maps), cruzada com o código-fonte local da Unreal 5.7/5.8, da
CryEngine e da Flax, mais bibliografia pública. Registra também o que a fase 1 corrigiu
e a fila do que ficou.

## Veredito

O núcleo está correto e não é improvisado. A distribuição de splits (`Accum` em
`SunShadows.cpp`) é um port literal do `ComputeAccumulatedScale` da Unreal com expoente
3.0, e o sphere-fit — `OptimalOffset`, `clamp(czv, dn, df)`, `ceil` do raio — é um port
literal do `GetShadowSplitBounds` do mesmo arquivo. A seleção de cascata por
profundidade de view concorda com as três engines de referência.

Em três pontos a Smile está à frente das referências. O pancaking é feito por hardware
(`DepthClipEnable = FALSE`, clamp por pixel) e não pelo clamp por vértice do VS da
Unreal, que a própria Epic documenta como problemático em triângulo grande cruzando o
near. O snapping usa `floor` e não o `trunc` da Cry, que tem descontinuidade de um
quantum ao cruzar a origem do mundo. E o array de slices evita o vazamento entre tiles
que a Flax admite ter no atlas dela (`TileBorder = 0` nas cascatas direcionais).

Os defeitos encontrados são periféricos ao núcleo, mas mensuráveis, e concentram-se em
bias, filtro e custo do passe de profundidade.

## Escopo auditado

- distribuição de splits, fitting da esfera, snapping e estabilização temporal;
- range de profundidade do ortho, pancaking e culling de casters;
- bias constante, slope bias de hardware e normal offset;
- filtro PCF, PCSS/contact hardening e transição entre cascatas;
- consumo pelo deferred, pelo ForwardBlend, pela água, pelo froxel e pelos sun shafts;
- cache round-robin das cascatas distantes e filtro de caster pequeno;
- estados D3D12, samplers estáticos, formato e VRAM;
- knobs do editor e instrumentação.

## Geometria das cascatas, medida

Configuração de referência: 4 cascatas, 2048², alcance 800 m, expoente 3.0, FOV 60°,
16:9, `CasterPullback = 80 m`, `DepthBias = 6e-4`. Splits em profundidade de view:
0,1 / 20,1 / 80,1 / 260,1 / 800.

| casc | fatia (m) | raio (m) | texel (m) | range Z (m) | bias (m) | bias (texels) |
|---|---|---|---|---|---|---|
| 0 | 0,1 → 20,1 | 24 | 0,0234 | 128 | 0,077 | **3,28** |
| 1 | 18,1 → 80,1 | 95 | 0,0928 | 270 | 0,162 | 1,75 |
| 2 | 74,1 → 260,1 | 338 | 0,3301 | 756 | 0,454 | 1,37 |
| 3 | 242,1 → 800,0 | 1038 | 1,0137 | 2156 | 1,294 | 1,28 |

O `dn` recuado em relação ao split é a sobreposição do crossfade: a cascata seguinte é
esticada para trás por `span(c-1) × BlendBand`, para conter fisicamente a faixa onde o
blend ocorre. Esse contrato está correto e casa com o peso calculado no shader.

Duas leituras importantes da tabela. A primeira é que o bias, constante em NDC, vale
2,6× mais texels na cascata 0 do que na 3 — consequência do `CasterPullback` ser
**aditivo** (`range = 2R + 80`) e não proporcional a `R`. A segunda é que a cascata 3
cobre um raio de 1038 m para um alcance de 800 m, com 1,01 m por texel.

## Defeitos encontrados

| # | Defeito | Severidade | Estado |
|---|---|---|---|
| 1 | PCSS devolve "iluminado" quando o blocker search não acha oclusor | alto | **corrigido (F1)** |
| 2 | `DepthBiasClamp = 0` deixa o slope bias de hardware ilimitado | médio-alto | **corrigido (F1)** |
| 3 | Bias constante em NDC varia 2,6× em texels entre cascatas | médio | F2 |
| 4 | Bias dobrado no caminho volumétrico desloca o feixe | médio | **corrigido (F1)** |
| 5 | Piso de 1 texel anula a penumbra constante em mundo | médio | F3 |
| 6 | Nenhum bias depende de N·L | médio | F2 |
| 7 | Culling de caster contra a caixa ortográfica, não contra a fatia | médio | F4 |
| 8 | `SampleCSM` chamado onde não há luz direta | baixo-médio | **corrigido (F1)** |
| 9 | Sampler de comparação do sun shafts com `CLAMP` | baixo | **corrigido (F1)** |
| 10 | Sem margem na borda da cascata para o kernel do PCF | baixo | F4 |

### 1. PCSS apagava sombras finas

O conjunto `kPoisson16` não tem amostra central: a mais interna está a 0,201 do raio,
ou 1,61 texels com `MaxPenumbraTexels = 8`. A densidade da busca é de um tap por
12,6 texels². Quando nenhum tap encontrava oclusor, o código devolvia `1.0f` — luz
plena. Um caster mais fino que ~1,6 texels (poste, grade, galho, cabo) passava entre os
taps e perdia a sombra; como a rotação do disco vem de um IGN por pixel e por frame, o
resultado piscava. Ativo por padrão, já que `SunAngularSizeDeg = 0,53`.

A Cry roda o mesmo algoritmo e nunca devolve iluminado no miss: reescala a matriz de
rotação e sempre filtra. A correção acrescenta o tap central e troca o retorno
antecipado por queda no PCF de raio mínimo, que ainda lê o texel do próprio receptor.

### 2. Slope bias sem teto

No D3D12 um `DepthBiasClamp` de zero significa *sem clamp*, e o termo do slope é
`SlopeScaledDepthBias × max(|dz/dx|, |dz/dy|)`. Em polígono quase paralelo à direção da
luz esse máximo diverge e o caster é empurrado para longe da superfície, vazando luz — a
documentação da Microsoft descreve exatamente o caso. As três referências clampam: a
Unreal em `r.Shadow.ShadowMaxSlopeScaleDepthBias = 1.0` (e sequer usa slope bias de
hardware — faz analítico no VS com `tan(θ)` clampado), a Cry em `fDepthBiasClamp = 0.001`
no raster e `fSlopeClamp = 0.001` no PS.

### 4. Bias no meio participante

Uma amostra de froxel está no ar: não há superfície, normal nem auto-sombreamento a
combater. A única coisa que um depth bias faz ali é deslocar o feixe ao longo da direção
da luz. O `CSM_VolumeTap` usava o dobro do bias do opaco, o que vale 15 cm na cascata 0
e 2,6 m na cascata 3 — o sintoma clássico de feixe que começa tarde e desgruda da janela
que o produziu. A Cry usa o mesmo `fDepthTestBias` do opaco no fog volumétrico, nunca o
dobro.

### 3 e 6. Bias que não escala e não conhece N·L

Unreal e Cry convergiram independentemente na mesma solução: bias proporcional ao
tamanho do texel em mundo. A forma da Unreal é
`DepthBias = CSMDepthBias × (R/Res) / Zrange` com `ShadowCascadeBiasDistribution = 1`,
que se reduz algebricamente a `CSMDepthBias/2` texels — exatamente constante em toda
cascata, independente do raio e do range. A Cry chega ao mesmo com
`biasAmount × texelSizeScale / (far − near)` no `e_ShadowsAutoBias`.

Sobre N·L, a Smile é a única das três sem nenhuma dependência. A Flax escala o normal
offset por `saturate(1 − NoL)`; a Unreal modula a largura da transição por
`lerp(0.1, 1, NoL)`. A formulação canônica (Castaño) é mais precisa que a aproximação da
Flax: o normal offset é proporcional a `sin(α)` e o depth bias a `tan(α)`, sendo
`sqrt(1 − NoL²)` o primeiro. `1 − NoL` subestima no ângulo médio — em `NoL = 0,5` dá 0,5
contra o 0,866 correto — e é onde a acne reaparece.

### 5. Penumbra constante em mundo, anulada pelo piso

O raio pedido pelo escalonamento `texel0/texelI` é sub-texel a partir da cascata 1
(0,63 / 0,18 / 0,06 texels), então `max(..., 1.0f)` morde sempre. A penumbra efetiva vai
de 5,86 cm para 9,28, 33,01 e 101,37 cm. O objetivo declarado no comentário não é
atingido, e as cascatas 1 a 3 gastam 16 `SampleCmp` num kernel de um texel.

Nenhuma das três referências tenta manter a penumbra constante em mundo. A Cry usa uma
tabela por cascata — `arrWidthS = {1.94, 1.0, 0.8, 0.5, 0.3}` multiplicada por
`r_ShadowJittering = 3.4`, ou 6,6 / 3,4 / 2,7 / 1,7 texels — que encolhe mas nunca abaixo
de ~1,7 texels, de modo que o filtro sempre faz alguma coisa.

Registro adicional: o raio máximo do `kPoisson16` é 1,234, não 1,0, então o kernel
alcança 23% além do valor pedido em texels. A distribuição radial em si está correta
(56% dos taps além de 0,7·Rmax, contra 51% de um disco uniforme).

### 7. Culling contra a caixa ortográfica

A caixa testada tem de 24× a 46× o volume da fatia do frustum que precisa cobrir, com
lados de 48, 190, 676 e 2076 m. Nas cascatas 2 e 3 a caixa é maior que a Bistro inteira,
de modo que os planos ±x e ±y não cortam nada e só o `MinCasterTexels` filtra. A Unreal
resolve com `ComputeShadowCullingVolume`, o convex hull da fatia extrudado na direção da
luz, montado a partir dos pares de planos adjacentes com produto escalar de sinais
opostos: é exato e muito mais apertado.

A Cry tem um truque mais agressivo — objeto contido inteiro na cascata N não é desenhado
em N+1 — que **não deve ser copiado** aqui: com sol baixo, um caster próximo projeta
sombra longa dentro da cascata distante e ela desapareceria. O sistema de time-of-day da
Smile passa por essa condição todo ciclo.

## Comparação com Unreal, Cry e Flax

| | Smile | Unreal 5.7 | CryEngine | Flax |
|---|---|---|---|---|
| Splits | `Accum(e=3)` | `Accum(e=3)`, idêntico | geométrico 3 m × 3ⁿ | manual 0,05/0,15/0,5/1 |
| Fit | esfera da fatia | esfera da fatia | esfera sequencial na aresta | esfera da fatia |
| Snap | `floor`, 1 texel | `fmod`, 4 texels | `trunc`, 2 texels | `floor` + `round` |
| Snap em Z | não | não | não | não, explícito |
| Range Z | `2R + 80` | `2·max(R, 50 m)` | `2·max(256, casterZ)` | `2R`, near em 0 |
| Bias | constante em NDC | ∝ texel em mundo | ∝ texel, ou tabela do ToD | constante em NDC |
| Slope bias | hardware, clampado | VS analítico, clampado | hardware e PS, clampados | — |
| Normal offset | constante, 2,5 texels | não tem | não tem | `× saturate(1−NoL)` |
| Filtro | 16 Poisson rotado | 5×5, 9 `Gather` | 16 Poisson rotado | 5×5, 9 taps otimizados |
| Comparação | binária | rampa suave | binária | binária |
| Transição | lerp de 2 cascatas | 2 passes, alpha blend | 2 cascatas, stencil | dither, por padrão |
| Culling | caixa ortográfica | hull-silhueta | hull e dedup entre cascatas | frustum por contexto |
| Formato | D32 | D16 | D16 no cache | D16 |
| Cache distante | round-robin 2/4 frames | scrolling, desligado | time-sliced com stamps | update-rate |

A Flax obtém a proporcionalidade do bias de graça porque o range dela é exatamente `2R`,
sem pullback: o bias constante em NDC vira automaticamente constante em texels. O
pullback aditivo da Smile é o que quebra essa propriedade.

O cache round-robin da Smile sobrevive à câmera em movimento, coisa que o da Flax não
faz — lá um deslocamento de 10 cm (`SHADOWS_POSITION_ERROR`) invalida tudo e as quatro
cascatas voltam a ser redesenhadas todo frame.

## O que a fase 1 aplicou

- `Shaders/Shadow/CSMCommon.hlsli` — tap central no blocker search do PCSS e queda no
  PCF de raio mínimo no lugar do retorno antecipado iluminado; `CSM_VolumeTap` sem bias
  de superfície, só um epsilon de precisão.
- `Engine/Source/Graphics/SunShadows.cpp`, `LocalShadows.cpp`, `Terrain.cpp` —
  `DepthBiasClamp = 0.001f` como teto do slope bias.
- `Engine/Source/Graphics/SunShafts.cpp` — `ADDRESS_MODE_BORDER` no sampler de
  comparação, que faltava para a cor de borda branca deixar de ser código morto.
- `Shaders/DeferredLighting.ps.hlsl`, `Shaders/ForwardBlend.ps.hlsl` — o CSM só é
  amostrado quando há luz direta a modular.

Dois desses itens mudam a imagem e precisam de A/B: as sombras finas que voltam nas
cascatas 0 e 1, e os feixes de fog e sun shafts que reancoram na origem. Os outros três
são idênticos pixel a pixel.

## Fila

**F2, bias coerente.** Bias em NDC por cascata proporcional ao texel em mundo, com
`CascadeBiasScale` rebaixado a multiplicador artístico por cima; normal offset escalado
por `sqrt(1 − NoL²)` com clamp.

**F3, filtro.** Optimized PCF de 5×5 em 9 taps, com pesos bilineares derivados da posição
fracionária dentro do texel, no lugar dos 16 Poisson rotados — é o que Unreal e Flax
usam, é determinístico e dispensa o ruído. Piso do kernel em ~2 texels, ou tabela por
cascata no estilo da Cry.

**F4, custo do passe de profundidade.** Culling por hull-silhueta; ordenação dos casters
por alpha-test e material uma única vez, fora do laço de cascatas; migração de D32 para
D16, que corta a VRAM de sombra de 64 MB para 32 MB e é o formato das três referências.

**F5, opcional.** SDSM na variante barata, com `zMin`/`zMax` saindo do último mip da HZB
que o occlusion culling já constrói; sombras ray-traced híbridas na banda de penumbra
classificada por `0 < pcf < 1`, com denoise pelo SIGMA do NRD; registro do array de
cascatas no visualizador de alvos, onde as LUTs da atmosfera já estão e o shadow map não;
teste unitário do fitting, que exigiria extrair a matemática para um header puro como o
`AtmosphereMath.hlsli` fez; knobs ausentes na UI — normal offset, raio do PCF, blend
band, pullback, expoente — e persistência das configurações, que hoje voltam ao default
a cada boot.

## Hipóteses a testar

Não são defeitos confirmados; ambas dependem de A/B.

A primeira é usar a normal geométrica do `NormalBuffer` do z-prepass no normal offset, em
vez da normal do G-buffer, que carrega normal map. A bibliografia recomenda a geométrica,
mas a Flax usa a do G-buffer exatamente como a Smile faz hoje.

A segunda é somar a distância à borda lateral da cascata ao peso do blend, que hoje é
calculado só por profundidade. Com esfera e snapping a cascata é maior que a fatia, e um
pixel pode estar dentro da fatia mas perto da borda da caixa; o fallback atual troca de
cascata sem transição nesse caso.

## Limites conhecidos e decisões deliberadas

O alcance de 800 m com 4 cascatas dá 1,01 m por texel na cascata 3; qualquer coisa além
do alcance é tratada como iluminada, após um fade nos últimos 54 m. Translúcidos não
projetam sombra opaca, por decisão. O caminho de ray tracing sombreia o sol com shadow
ray binário, independente do CSM, o que é o padrão híbrido e concorda com o Lumen. O
`MinCasterTexels` mede a maior extensão da AABB em texels da própria cascata, que é mais
correto para sombra do que o raio em espaço de tela usado pela Unreal.

## Referências

- `D:\Engines\Unreal Engine 5.7` — `DirectionalLightComponent.cpp` (splits, fitting,
  culling volume), `ShadowSetup.cpp` (snapping, range Z), `ShadowRendering.cpp`
  (`UpdateShaderDepthBias`), `ShadowDepthVertexShader.usf` (slope analítico, pancaking),
  `ShadowFilteringCommon.ush` (PCF), `ShadowProjectionPixelShader.usf` (receiver bias).
- `D:\Engines\CryEngine` — `LightEntity.cpp` (GSM, bias), `ShadowUtils.cpp` (snap,
  Poisson, blend), `ShadowCache.cpp` (cache time-sliced), `ShadowCommon.cfi` (filtro,
  contact hardening), `VolumeLighting.cfi` (froxel).
- `D:\Engines\FlaxEngine` — `ShadowsPass.cpp` (splits, fitting, snap duplo, update rate),
  `ShadowsSampling.hlsl` (optimized PCF, dither), `ShadowsCommon.hlsl` (normal offset).
- Matt Pettineo, *A Sampling of Shadow Techniques*, e o sample `TheRealMJP/Shadows`.
- Ignacio Castaño, *Shadow Mapping Summary* (normal offset por `sin`, bias por `tan`).
- Microsoft, *Cascaded Shadow Maps*, *Common Techniques to Improve Shadow Depth Maps* e
  *Depth Bias* (fórmula do raster bias e do clamp).
- Zhang et al., *Parallel-Split Shadow Maps*, GPU Gems 3 capítulo 10.
- Lauritzen, Salvi e Lefohn, *Sample Distribution Shadow Maps*, I3D 2011.
- Kostas Anagnostou, *Experiments in Hybrid Raytraced Shadows*, e AMD FidelityFX Hybrid
  Shadows.
