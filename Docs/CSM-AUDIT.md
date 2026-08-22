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
| 3 | Bias constante em NDC varia 2,6× em texels entre cascatas | médio | **corrigido (F2)** |
| 4 | Bias dobrado no caminho volumétrico desloca o feixe | médio | **corrigido (F1)** |
| 5 | Piso de 1 texel anula a penumbra constante em mundo | médio | **corrigido (F3)** |
| 6 | Nenhum bias depende de N·L | médio | **corrigido (F2)** |
| 7 | Culling de caster contra a caixa ortográfica, não contra a fatia | médio | **corrigido (F4)** |
| 8 | `SampleCSM` chamado onde não há luz direta | baixo-médio | **corrigido (F1)** |
| 9 | Sampler de comparação do sun shafts com `CLAMP` | baixo | **corrigido (F1)** |
| 10 | Sem margem na borda da cascata para o kernel do PCF | baixo | **aberto** (ver Fila) |

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
- `Engine/Source/Graphics/Lighting/SunShadows.cpp`, `Lighting/LocalShadows.cpp`,
  `Scene/Terrain.cpp` —
  `DepthBiasClamp = 0.001f` como teto do slope bias.
- `Engine/Source/Graphics/Environment/SunShafts.cpp` — `ADDRESS_MODE_BORDER` no sampler de
  comparação, que faltava para a cor de borda branca deixar de ser código morto.
- `Shaders/DeferredLighting.ps.hlsl`, `Shaders/ForwardBlend.ps.hlsl` — o CSM só é
  amostrado quando há luz direta a modular.

Dois desses itens mudam a imagem e precisam de A/B: as sombras finas que voltam nas
cascatas 0 e 1, e os feixes de fog e sun shafts que reancoram na origem. Os outros três
são idênticos pixel a pixel.

## O que a fase 2 aplicou

O bias deixa de ser um escalar em NDC e passa a ser expresso em **texels da cascata**,
convertido para NDC por cascata na CPU (`DepthBiasTexels × texel[c] / range[c] ×
CascadeBiasScale[c]`, em `CSMConstants::BiasNdc`). É a identidade que a Unreal obtém com
`ShadowCascadeBiasDistribution = 1` e a Cry com `e_ShadowsAutoBias`. O cálculo fica fora
do laço de cascatas de propósito: cascata congelada pelo cache não reescreve texel e
range, mas os valores retidos no constant buffer continuam descrevendo o mapa que está lá.

O padrão é 2,0 texels, escolhido para não introduzir acne em lugar nenhum — fica dentro
da faixa que já valia nas cascatas próximas e acima da que valia nas distantes:

| casc | antes (texels) | antes (m) | depois (texels) | depois (m) |
|---|---|---|---|---|
| 0 | 3,28 | 0,077 | 2,00 | 0,047 |
| 1 | 1,75 | 0,162 | 2,00 | 0,186 |
| 2 | 1,37 | 0,454 | 2,00 | 0,660 |
| 3 | 1,28 | 1,294 | 2,00 | 2,027 |

O normal offset passa a escalar por `sqrt(1 − NoL²)`, o seno do ângulo entre a normal e a
luz. O erro que ele combate é a quantização do rasterizador do shadow map — um texel
guarda uma única profundidade — e o deslocamento lateral necessário para sair da célula
errada vale `texel × sin(α)`. Em `N·L = 1` esse erro é zero, e o valor constante anterior
produzia peter-panning de graça, comendo o contato e o auto-sombreamento fino; em
`N·L → 0` ele é máximo e o valor constante ficava curto, deixando acne na faixa do
terminador. A Flax usa `saturate(1 − NoL)`, que subestima no ângulo médio — em
`N·L = 0,5` dá 0,5 contra o 0,866 correto — e custa o mesmo que o seno exato.

| N·L | 1,00 | 0,90 | 0,70 | 0,50 | 0,25 | 0,05 |
|---|---|---|---|---|---|---|
| antes (texels) | 2,50 | 2,50 | 2,50 | 2,50 | 2,50 | 2,50 |
| agora, `sin(α)` | 0,00 | 1,09 | 1,79 | 2,17 | 2,42 | 2,50 |
| Flax, `1 − NoL` | 0,00 | 0,25 | 0,75 | 1,25 | 1,88 | 2,38 |

A direção da key light entrou no constant buffer (`SunDirection`) porque o offset agora é
por pixel. O fator não depende da cascata, então é calculado uma vez e serve aos três
caminhos — cascata atual, fallback e blend —, e o offset continua sendo refeito com o
texel da cascata seguinte no blend, que é onde esquecer disso produz uma faixa clara ou
escura na transição.

O slider "Bias de profundidade" mudou de unidade, de NDC para texels; a faixa da UI
passou a 0–8.

## O que a fase 3 aplicou

O filtro passa a ter duas famílias explícitas, separadas como a Unreal separa em
`r.Shadow.FilterMethod`, porque optimized PCF e PCSS não compõem: o primeiro tem
footprint fixo, o segundo precisa de raio arbitrário por pixel.

**Raio fixo — optimized PCF 5×5 em 9 taps.** Cada `SampleCmpLevelZero` já é um PCF
bilinear 2×2 do TMU; escolhendo offsets e pesos em função da posição fracionária do
receptor dentro do texel, a soma ponderada de 9 taps reproduz *exatamente* um box 5×5
uniforme — não é aproximação. É o filtro do The Witness (Castaño), na forma do sample do
MJP e do `SampleShadowMapOptimizedPCF` da Flax. Cobre as cascatas 2 e 3 sempre, e todas
as cascatas quando o PCSS está desligado.

**Penumbra variável — o disco de Poisson rotado permanece**, mas só nas cascatas 0 e 1 com
PCSS ligado e só quando a penumbra estimada passa do piso.

A tentativa de manter a penumbra constante em mundo saiu. Ela era anulada pelo piso de um
texel a partir da cascata 1 e nenhuma das três referências tenta isso.

| chamada de `CSM_PCF` | antes | depois |
|---|---|---|
| cascatas 2-3 | 16 `SampleCmp` | **9** |
| PCSS no piso (contato) | 16 `SampleCmp` + 17 `Load` | **9** + 17 |
| PCSS com penumbra larga | 16 `SampleCmp` + 17 `Load` | inalterado |

O piso da penumbra subiu de 1 para 2 texels, casado com a meia-largura do box 5×5 para que
a troca entre as duas famílias seja contínua: no contato, onde o PCSS colapsa, os dois
filtros têm o mesmo alcance. Com o sol a 0,53° o oclusor precisa estar a mais de 10 m na
cascata 0 e a mais de 40 m na cascata 1 para sair do piso — ou seja, na cascata 1, que
cobre 18 a 80 m, o caso comum cai no caminho determinístico e ela deixa de pagar o disco
por um resultado que já estava grampeado.

Efeito colateral desejado: o caminho de raio fixo não depende mais do ruído animado, e
portanto não depende do TAA nem do FSR2 para não virar padrão fixo. Sem boiling em
disocclusion.

O knob `PcfRadiusTexels` mudou de significado — era o raio do kernel fixo, agora é o piso
da penumbra do PCSS — e o padrão foi de 2,5 para 2,0.

## O que a fase 4 aplicou

**Volume de culling.** Os planos laterais deixaram de sair da matriz do ortho — que é a
caixa da esfera de *fitting* — e passaram a vir da extensão real da fatia do frustum em
espaço de luz. É exato: um caster só sombreia um receptor se estiver sobre o segmento que
vai do receptor até o sol, logo precisa compartilhar a coordenada `(right, up)` dele; quem
está fora da extensão da fatia não pode sombrear ninguém dentro dela, por mais longe que
esteja na direção da luz. Os planos são montados em espaço de mundo no `UpdatePerFrame` e
retidos junto da matriz, para que cascata congelada pelo cache continue cullando contra o
volume que gerou o mapa dela. Margem de 8 texels cobre o normal offset e o kernel do PCF.

Medido na Bistro Exterior, isolando o teste de planos (1542 casters na lista):

| casc | caixa do ortho | fatia | corte |
|---|---|---|---|
| 0 | 1106 | 889 | −20% |
| 1 | 1535 | 1445 | −6% |
| 2 | 1542 | 1491 | −3% |
| 3 | 1542 | 1542 | 0% |

**A razão de volume de 24× a 46× registrada acima não se traduz em 24× a 46× menos
casters.** Numa cena compacta como a Bistro, de ~200 m, quase tudo cai dentro da footprint
da fatia também, e na cascata 3 o culling não corta nada. O corte útil está na cascata 0,
que é justamente a que redesenha todo frame enquanto as distantes são cacheadas. O ganho
escala com o tamanho do mundo, não com o da cascata; a Bistro é o caso pequeno. O hull
silhueta da Unreal é mais apertado que esta AABB e continua disponível como refinamento.

**Ordenação dos casters.** A lista é ordenada uma vez no `Renderer`, por alpha-test e
depois por material, em vez de ser varrida em ordem de cena quatro vezes. O PSO passa a
trocar uma única vez por cascata, na fronteira entre opacos e masked, e o `Bind` do
material é pulado quando repete — o que só é possível porque itens do mesmo material agora
são adjacentes. `Cur` e `LastMat` zeram por cascata, porque o `SetGraphicsRootSignature` no
topo do laço invalida as descriptor tables.

**D32 para D16.** O array de cascatas caiu de 64 MB para 32 MB, e aparece na categoria
Sombras da janela de Estatísticas. É o formato das três referências: a Unreal define
`PF_ShadowDepth` como `R16_TYPELESS`, a Flax escolhe `D16_UNorm` como primeiro suportado, a
Cry usa D16 no cache. Precisão: 65536 níveis sobre o range do ortho dão 1,95 mm na cascata
0 e 3,3 cm na 3, contra um bias de 24 e 62 níveis — folga de sobra. O `DepthBias` do
rasterizador é 0, então a mudança da unidade `r` do formato UNORM, que multiplica apenas
aquele termo, não afeta nada, e o slope bias é imune ao formato. O PSO de sombra do terreno
precisou de `DSVFormat` próprio, já que ele também alimenta as sombras locais, que seguem
em D32.

## O que falta

Fases 1 a 4 estão aplicadas e commitadas. O que segue é o que sobrou, em ordem de
prioridade. Nada aqui bloqueia nada — o sistema está funcional e validado.

### 1. Dívida de A/B (bloqueia fechar o ciclo)

Duas fases mudaram a imagem e ainda não foram validadas visualmente. A F4 foi validada
com ganho de FPS confirmado.

| fase | o que olhar |
|---|---|
| F2 | Contato com sol alto, onde o normal offset agora zera em `N·L = 1` — é o ganho maior e o risco maior ao mesmo tempo, porque quem segura a acne ali passa a ser só o depth bias. E a faixa do terminador, onde o offset agora cresce e a acne rasante deve sumir. |
| F3 | Sombra distante, que ficou mais macia: o box 5×5 é mais largo que o texel único que sobrava do piso antigo. Se ficar macia demais, trocar o 5×5 por 3×3 (4 taps) no caminho de raio fixo — é uma linha e sai ainda mais barato. |

### 2. Defeito 10 — margem na borda da cascata

O único defeito do laudo que segue aberto, de severidade baixa. São duas coisas na mesma
região:

O far do ortho fica exatamente em `radius`, então um receptor na borda da esfera dá
`z_ndc = 1,0` e o `CSM_InBounds`, que exige `< 1,0`, o rejeita — ele cai no fallback para
a cascata seguinte sem transição. Basta uma folga pequena no far.

E o kernel do PCF não tem *inset*: taps que saem do slice devolvem "iluminado" pela cor de
borda branca, o que abre uma franja clara nos cantos extremos da fatia. A Unreal reserva
`SHADOW_BORDER = 4` texels e renderiza numa região menor que o tile; a documentação da
Microsoft recomenda reservar metade do kernel. Custo: encolher o viewport do passe de
profundidade e compensar na matriz.

### 3. Ferramental (barato, ajuda todo o resto)

~~O array de cascatas não está registrado no visualizador de alvos.~~ **Feito na F0 da frente
de casters estáticos/dinâmicos**, junto com contadores por cascata na janela de estatísticas.

Não existe teste unitário do fitting. Os candidatos naturais são a distribuição de splits
contra a fórmula da Unreal, o raio da esfera, a invariância do bias em texels entre
cascatas (que a F2 introduziu e que uma regressão silenciaria) e a estabilidade do
snapping sob translação da câmera. Exige extrair a matemática de `SunShadows.cpp`, hoje
acoplada ao D3D12, para um header puro — o mesmo movimento que o `AtmosphereMath.hlsli`
fez pela atmosfera, e que o `TimeOfDayMoonTests` já usa como padrão de teste.

Faltam knobs na UI: normal offset, blend band, caster pullback, expoente de distribuição e
teto de penumbra do PCSS não são editáveis. E não há persistência nenhuma das
configurações de render — não existe `QSettings` no editor, então todo ajuste volta ao
default a cada boot, o que torna qualquer sessão de tuning descartável.

### 4. Hipóteses a testar

Não são defeitos confirmados; as duas dependem de A/B e as duas são baratas.

Usar a normal geométrica do `NormalBuffer` do z-prepass no normal offset, em vez da normal
do G-buffer, que carrega normal map. A bibliografia recomenda a geométrica, porque com
normal map o offset aponta para direções que não correspondem à superfície real e produz
acne ondulada seguindo o padrão do mapa. Mas a Flax usa a do G-buffer exatamente como a
Smile faz hoje, então não é consenso.

Somar a distância à borda lateral da cascata ao peso do blend, que hoje é calculado só por
profundidade. Com esfera e snapping a cascata é maior que a fatia, e um pixel pode estar
dentro da fatia mas perto da borda da caixa; o fallback atual troca de cascata sem
transição nesse caso. É o `distToEdge` do sample do MJP.

### 5. Estrutural (maior esforço, ganho maior)

**SDSM na variante barata.** Ajustar os splits ao intervalo de profundidade dos pixels
realmente visíveis, em vez de `[near, far]` da câmera. O ganho está no `zMin`: com a
câmera encostada numa parede, a cascata 0 hoje gasta boa parte da resolução em espaço
vazio. O `zMin`/`zMax` sai quase de graça do último mip da HZB que o occlusion culling v2
já constrói, e o readback ring já existe. Atenção ao conflito conhecido: splits que mudam
por frame mudam o raio da esfera, o que muda o tamanho do texel e desloca a grade do
snapping — ou seja, reintroduzem shimmer. Mitigação usual é quantizar os splits e só
recalcular quando saem de uma faixa.

**Sombras ray-traced híbridas.** Classificar a banda de penumbra pelo PCF que já é
calculado (`0 < pcf < 1`, custo marginal zero), traçar um raio só ali, com `TMin`/`TMax`
derivados do shadow map, e compor com `lerp(csm, rt, banda)`. É o padrão do FidelityFX
Hybrid Shadows e o que o levantamento apontou como melhor custo-benefício em 2026 — no
material medido, híbrido ficou em 13,2 ms contra 92,2 ms de full ray tracing. A Smile já
tem TLAS, NRD e classificação por tile; o denoiser seria o SIGMA, do mesmo pacote do NRD
que o ReSTIR já usa.

**Hull-silhueta no culling.** A F4 apertou o volume para a AABB da fatia em espaço de luz.
A Unreal vai além com o convex hull da fatia extrudado na direção da luz
(`ComputeShadowCullingVolume`), montado a partir dos pares de planos adjacentes com
produto escalar de sinais opostos. Só vale se o mundo crescer: na Bistro a AABB já não
corta quase nada, e o hull cortaria ainda menos do que já não corta.

**Reduzir o PCSS à cascata 0.** A F3 mediu que na cascata 1 a penumbra fica no piso a menos
que o oclusor esteja a mais de 40 m, o que naquele intervalo (18 a 80 m) é o caso raro.
Ela paga 17 `Load` do blocker search para quase sempre cair no caminho determinístico.
Restringir o PCSS à cascata 0 economizaria isso — mas muda a imagem em sombras difusas de
meia distância, então é decisão artística, não técnica.

## Frente nova: casters estáticos e dinâmicos

Rodada aberta em 6 de agosto de 2026, separada das fases 1 a 4 porque não corrige defeito do
laudo: ataca o custo do passe de profundidade classificando os casters em estáticos e
dinâmicos, para que o mapa de um caster que não se move seja reaproveitado entre frames.

O motivo não é só custo. O cache round-robin atual redesenha **tudo** quando uma cascata
distante refresca, o que significa que um objeto em movimento a 100 m tem a sombra atualizada
a cada 2 frames e a 300 m a cada 4 — 33 ms e 66 ms de defasagem a 60 fps. Hoje isso é
invisível porque nada na cena se move fora do gizmo, mas é defeito latente. Separar as duas
classes permite dinâmico todo frame **e** estático congelado, que é mais rápido e mais correto
ao mesmo tempo.

### Como as referências fazem

| | mecanismo | granularidade | ancoragem do cache |
|---|---|---|---|
| Unreal 5.8 | dois mapas por cascata, `SDCM_StaticPrimitivesOnly` (cacheado) + `SDCM_MovablePrimitivesOnly`; o segundo **copia a profundidade** do primeiro (`CopyCachedShadowMap`, PS escrevendo `SV_Depth` com `CF_Always`) e desenha os móveis por cima | por primitiva (`IsMeshShapeOftenMoving`) | câmera, com `SDCM_CSMScrolling`: centro andou mas sobreposição > 0,75 → rola o mapa e redesenha só a faixa exposta; desiste acima de 5 casters extras |
| CryEngine | cascatas a partir de `nFirstStaticLod` viram frustums `e_GsmCached` só com estático (exclui `ERF_DYNAMIC_DISTANCESHADOWS`); `eFullUpdate`, `eFullUpdateTimesliced` e `eIncrementalUpdate` (≤ 50 nós/frame, com generation id por nó para não redesenhar o que já entrou) | por render node | **mundo**: só refaz quando a câmera sai dos bounds cacheados |
| Flax | `StaticFlags::Shadow` com `StaticFlagsMask`/`StaticFlagsCompare` filtrando a draw list da shadow view; `ShadowsUpdateRate` por luz e por distância | por ator | por luz |

Duas leituras. A primeira: `r.Shadow.CSMCaching` vem **0 por padrão** na Unreal 5.8 — só o
`CacheWholeSceneShadows`, que é de point/spot, vem 1. Nem a Epic liga cache de CSM direcional
por default, e o caso difícil é exatamente o das cascatas próximas. A segunda: a Cry ancora o
cache no **mundo** e não na câmera, o que evita o problema do fit que muda todo frame em vez
de resolvê-lo.

### Fases

- **F0 — instrumentação e baseline.** Aplicada (ver abaixo).
- **F1 — classificação.** `EMobility` no `FRenderable`, default estático, persistido no
  `.smap`, UI no editor, grafo de invalidação (mover, ocultar, adicionar, remover ou trocar
  material de um estático invalida a cascata cuja `CullPlanes` a AABB toca). Objeto sendo
  arrastado no gizmo conta como dinâmico enquanto arrasta, senão cada frame de mouse invalida
  o cache. Brinde de CPU: a lista de casters é montada e ordenada por frame no `Renderer`
  (~1542 itens); a parte estática passa a ser montada uma vez por mudança de estrutura.
- **F2 — cache estático nas cascatas 2 e 3.** Substitui o round-robin. Risco baixo: o fit
  dessas cascatas já é congelado com 10% de folga, então a cópia estático→runtime é exata sem
  offset de profundidade. O terreno entra no mapa estático — hoje ele redesenha em toda
  cascata que atualiza.
- **F3 — estender às cascatas 0 e 1 com scrolling.** É onde está o ganho (são as que
  redesenham tudo todo frame) e o risco. Exige quantizar o `cf`, que hoje **não é snapado**:
  quando a câmera anda, a profundidade cacheada passa a estar num referencial diferente. A
  alternativa é copiar com offset de profundidade, que é o `DepthOffsetScale` do
  `FScrollingShadowMaps2DPS` da Unreal.
- **F4 — time-slicing incremental estilo Cry.** Só se o mundo crescer.

Custo de VRAM: +16 MB se o gêmeo estático cobrir só as cascatas 2 e 3, +32 MB para as quatro.

A favor da Smile aqui: o snapping é `floor(centro/texel)*texel`, então o deslocamento entre
dois fits é um número **inteiro** de texels e o scroll é exato, sem reamostragem.

### O que a fase 0 aplicou

Instrumentação. Nenhuma mudança de imagem, nenhuma mudança de custo além de 2 timestamps por
cascata congelada.

`FSunShadows::FCascadeStats` guarda, por cascata, quantos casters foram desenhados e quantos
cada filtro cortou (tamanho e depois planos da fatia, na ordem em que o passe aplica). Os
contadores descrevem o **último update daquela cascata**, não o frame corrente: cascata
congelada não desenha nada, e zerar perderia justamente o que interessa, que é o custo dela
quando dispara. A frequência sai de `UpdateHistory`, uma janela de 64 frames deslocada uma vez
por frame para toda cascata, inclusive a congelada. O custo médio por frame é o produto dos
dois, e é o número a comparar num A/B.

`SnapDeltaTexels` mede quanto o centro snapado andou desde o último fit, em texels da própria
cascata. É a medida que decide a F3: delta pequeno significa que quase todo o conteúdo do mapa
anterior continua válido, só deslocado, e a faixa nova custa `delta/2048` do passe.

O escopo do profiler passou a abrir sempre, inclusive na cascata congelada. Antes ele ficava
depois do `continue` e a linha sumia da janela nos frames em que o cache pegava, reaparecendo
no seguinte — um ranking que pisca não dá para ler, e `0,00 ms` é precisamente o que se quer
enxergar.

O array de cascatas entrou no visualizador de alvos, que era o item 3 da fila de ferramental.
Exigiu um caminho de `Texture2DArray` no `FDebugView`, que só sabia ler `Texture2D`: não
existe SRV de dimensão `TEXTURE2D` que enderece array slice, então alvo de array precisa de
declaração própria no HLSL. `t5` recebe o mesmo descritor de `t0` e o `Decode` escolhe qual
das duas o shader lê. As quatro cascatas são registradas sobre o mesmo SRV, com a fatia no
`SubIndex`; selecionando as quatro de uma vez a grade mostra o encaixe entre elas. O atlas das
sombras locais é o próximo candidato ao mesmo caminho.

Fica de fora da contagem o terreno, que é desenhado pelo `ExtraCascadeDraw` e não passa pela
lista de itens. Ele é caster estático por definição e a F2 o tira de todas as cascatas
cacheadas — o ganho dele não aparece nestes números.

### Baseline medido

Bistro Exterior, RTX 3060 Ti, 1542 renderáveis na lista de casters, **câmera parada**.

| casc | casters | −tamanho | −fatia | taxa | draws/frame | GPU (EMA) | por disparo |
|---|---|---|---|---|---|---|---|
| 0 | 896 | 0 | 646 (42%) | 64/64 | 896 | 0,56 ms | 0,56 |
| 1 | 1329 | 129 | 84 (5%) | 64/64 | 1329 | 0,79 ms | 0,79 |
| 2 | 1216 | 284 | 42 | 32/64 | 608 | 0,59 ms | ~1,18 |
| 3 | 873 | 669 (43%) | 0 | 16/64 | 218 | 0,35 ms | ~1,40 |

Total: **3051 draws de sombra por frame** de um teto de 6168, e **2,29 ms de um frame de
9,04 ms — 25,4%**. O CSM é o passe serial mais caro da engine; o DDGI aparece maior (2,43 ms)
mas roda async.

Quatro leituras.

**Os dois filtros de culling trocam de papel exatamente como projetados.** Na cascata 0 o
volume da fatia corta 646 e o filtro de tamanho corta zero — texel de 2,3 cm, nada é pequeno
demais. Na cascata 3 é o inverso perfeito: tamanho corta 669, fatia corta 0. Cada um cobre a
ponta onde o outro é inútil, o que justifica os dois coexistirem.

**A cascata 1 é a mais cara e a menos cullável.** 1329 draws todo frame com só 14% cortado:
ela é grande o bastante para conter a Bistro inteira e o texel dela (9,3 cm) é pequeno demais
para o filtro de tamanho morder. Nenhum culling geométrico melhora esse caso — só cache.

**As cascatas 0 e 1 somam 2225 draws por frame incondicionais**, 73% do custo, pagos com a
cena inteiramente parada.

**Com a câmera parada, `SnapDeltaTexels` é 0 nas quatro cascatas.** O fit é idêntico entre
frames — mesmo centro snapado, mesmo raio, mesma direção de luz — e mesmo assim as cascatas 0
e 1 redesenham tudo. O desperdício não é estimado, é observado, e a condição que o detecta já
está calculada no `UpdatePerFrame`.

Consequência para o plano: a F2 não deve se limitar às cascatas 2 e 3. O recorte certo é
**todas as cascatas, com o redesenho do estático condicionado a o fit não ter mudado**, que é
o caso dominante no editor. A F3 passa a cobrir só o caso da câmera em movimento.

Para a cascata 1 a âncora de mundo da Cry (`m_CachedShadowsBounds`) é mais promissora que o
scrolling da Unreal: raio de 95 m com texel de 9,3 cm significa que um mapa 2048² cobre 190 m
naquela resolução, ou seja, a Bistro inteira. Ancorada no mundo ela não re-renderiza enquanto
a câmera não sair dos bounds — sem scrolling, sem `cf` quantizado, sem faixa exposta. Não
escala para mundo grande, e é por isso que a Cry expõe os bounds em vez de derivá-los; a
Emerald Square serve de contraprova.

### O que a fase 2 aplicou

Um segundo array D16 de 4 fatias (`StaticArray`, +32 MB, categoria Sombras) guarda **só os
casters estáticos**. Por cascata e por frame o passe decide entre três coisas:

- **nada** — mapa estático válido e nenhum dinâmico agora nem no frame anterior. O alvo de
  runtime já está correto. É o caso da cena inteiramente estática, e ele custa zero;
- **copiar e desenhar o dinâmico** — `CopyTextureRegion` da fatia estática para a de runtime
  (mesmo formato e mesmas dimensões, então é exata: sem shader, sem reamostragem) e os
  dinâmicos por cima, sem clear, contra o depth que veio da cópia;
- **re-rasterizar o estático** e então as duas coisas acima.

O terreno entrou no mapa estático. Ele é caster estático por definição e antes redesenhava em
toda cascata que acordava — nas cascatas 2 e 3 da Bistro ele é praticamente o único conteúdo.

**Invalidação.** Três testes: o fit congelado ainda cobre a fatia, o sol não girou além da
tolerância, e o `StaticCastersVersion` da cena não mudou. As duas políticas de fit são
deliberadamente diferentes — com folga (cascatas 2 e 3) vale enquanto a esfera ideal couber na
congelada; sem folga (0 e 1) vale enquanto o fit recalculado der os mesmos números, isto é,
enquanto a matriz for a mesma. A folga não foi estendida às cascatas próximas de propósito:
custaria 10% de texel, e a medida da F0 mostrou que com a câmera parada o fit já é idêntico.

**A tolerância do sol é por cascata, em texels de deslocamento da sombra.** O deslocamento vale
`range × δ`, e `range/texel` é 5470 na cascata 0 contra 2127 na 3 — o mesmo ângulo custa 4,8
texels na próxima e 1,9 na distante. Um limiar único em graus, que era o que existia, protege a
distante demais e a próxima de menos.

**O round-robin trocou de função.** Antes ele redesenhava as cascatas 2 e 3 a cada 2 e 4 frames
*incondicionalmente* — um timer cobrindo a falta de um sinal de conteúdo. Com o
`StaticCastersVersion` o sinal existe, e a mesma aritmética de fase passa a **espalhar**
refreshes pendentes em vez de forçá-los. Só invalidação por sol entra na fila: fit e conteúdo
são atendidos no mesmo frame. Adiar conteúdo tem sintoma concreto — ao soltar o gizmo o objeto
sai do conjunto dinâmico e passa a depender do mapa estático, do qual foi retirado no início do
arraste, então a sombra dele sumiria pelos frames de espera.

**Defeito encontrado no primeiro A/B, e a lição dele.** Marcar um objeto como dinâmico e mexer
na câmera fazia regiões enormes da cena escurecerem e piscarem. A causa: o constant buffer da
matriz de cascata é indexado por frame-in-flight, mas só era escrito no caminho que refaz o
fit — o slot alternado ficava com a matriz de um fit antigo. Enquanto a cascata congelada
simplesmente não era desenhada isso era **inofensivo, porque ninguém lia aquele endereço**. A
fase 2 passou a rasterizar os dinâmicos sobre a cascata que reusa o mapa estático, o endereço
voltou a ser lido, e um único caster projetado por um fit alheio cobre uma área enorme do
shadow map com profundidade próxima — tudo atrás dele lê como sombreado. Como o slot alterna a
cada frame, piscava. A matriz retida passa a ser escrita no slot atual para toda cascata, todo
frame, refazendo o fit ou não.

Vale como categoria, não como episódio: **um cache latente vira defeito no dia em que alguém
começa a ler o que ele guardava sem que ninguém lesse.** Vale reler com essa lente qualquer
outro estado da engine que seja indexado por frame-in-flight e escrito condicionalmente.

**Resultado medido.** Na Bistro, ~20 FPS de ganho com o ReSTIR GI desligado e ~10 FPS com ele
ligado mais o denoiser, passando a bater perto de 60 FPS com NRD. O ganho menor no segundo caso
é o esperado: com o RTGI ativo o frame deixa de ser dominado pelo CSM, então a mesma economia
em milissegundos vale menos em FPS.

**Bypass por velocidade do sol.** Se o sol anda mais em um frame do que a tolerância mais
frouxa, nenhum mapa estático chega inteiro ao frame seguinte e mantê-lo é pagar rasterização e
cópia para jogar fora. Nesse caso o gêmeo é abandonado e o passe rasteriza tudo direto no alvo
de runtime, que é exatamente o caminho de antes do cache: a degradação é para **igual**, não
para pior. O limiar dá ~4 minutos por dia — um ciclo normal nem chega perto. Não é um switch de
"TOD ligado": o que decide é a velocidade medida, então sol fixo e ciclo lento ficam com o
cache inteiro, que é o caso da maioria dos jogos.

## Sombra de contato em screen-space: tentada e revertida

Implementada e revertida em 7 de agosto de 2026. Fica registrada porque o motivo da falha é
específico e determina o desenho da próxima tentativa.

**O que foi feito.** Port do `CastScreenSpaceShadowRay` da Unreal: raio curto do pixel em
direção à luz, marchado em NDC contra o depth buffer, 8 passos, janela de espessura de
`2 × tolerância`, fora-da-tela contando como miss, dither no offset inicial. Multiplicava no
termo do CSM, como o `ApplyContactShadowWithShadowTerms` faz. A única divergência deliberada
era o comprimento do raio, derivado do buraco do bias
(`(depthBiasTexels + normalOffsetTexels·sin α) × texel[c]`) em vez de autorado por luz.

**O veredito foi "parece que tem dois sistemas de sombras competindo"**, e é literalmente o
que estava acontecendo. O CSM com PCSS produz um termo **macio e largo**; o contact shadow em
screen-space produz uma oclusão **binária e dura**. Multiplicar um pelo outro coloca um núcleo
duro dentro de uma penumbra macia — duas famílias de sombra diferentes no mesmo pixel.

E a derivação "principiada" do comprimento **piorou** isso em vez de melhorar. O buraco do
bias é exatamente a região onde a penumbra do PCSS está em rampa; amarrar o raio a ele
garantiu que o termo duro caísse precisamente onde o termo macio vive. Um comprimento fixo e
curto, como a Unreal usa, teria sobreposto menos — a escolha mais defensável no papel foi a
pior na tela.

**A lição que vale para a fase de RT:** não multiplicar um segundo termo de visibilidade.
Traçar **dentro da banda de penumbra** que o PCF já identifica (`0 < pcf < 1`) e **substituir**
o termo ali, `lerp(csm, rt, banda)`, produzindo um resultado da mesma família — macio,
governado pelo tamanho angular da luz — em vez de um segundo termo por cima. É o que o
FidelityFX Hybrid Shadows faz, e agora há uma razão medida para seguir esse contrato à risca.

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
