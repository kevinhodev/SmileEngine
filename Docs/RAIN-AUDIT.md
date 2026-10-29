# Auditoria do sistema de chuva

Estado em 29 de outubro de 2026. Este documento é **só levantamento**: lista bugs,
melhorias de qualidade visual, desempenho, integração temporal (TAA/DLSS/FSR), arquitetura
e UX do sistema de chuva. Nada aqui foi implementado ainda. Cada item traz o ponto do
código, o sintoma esperado e uma proposta de correção, com uma prioridade sugerida.

## Escopo lido

| Fase | O que faz | Onde |
|---|---|---|
| F1 | wetness deferred: escurece albedo, baixa roughness, poças com ripples | `Shaders/Rain/RainWetness.ps.hlsl`, `FRainWetness::Execute` |
| F2 | mapa de oclusão top-down 512² (120 m), PCF; F2b drenagem | `RainCommon.hlsli::SkyVisibilityBand`, `FRainWetness::RecordOcclusionMap` |
| F3 | cortina fullscreen: 4 cilindros presos na câmera + véu raymarch | `Shaders/Rain/RainCurtain.ps.hlsl` |
| F4 | chuva dirige céu/luz/fog/nuvens; molhado acumulado (τ 5 s / 30 s) | `RendererFrame.cpp` (`ResolveFrameLighting`, `TickWorldClock`) |
| F5a | gotas por partícula: 32768 quads procedurais num box de 24 m | `RainParticles.vs/ps.hlsl`, `FRainWetness::ExecuteParticles` |
| F5b | splash sem estado na fração "subterrânea" do ciclo da gota | `RainSplash.vs/ps.hlsl` |
| knobs | fachada com invalidação | `RenderSettings.cpp:1099-1167`, `Weather.h` |
| editor | presets e sliders | `Editor/Qml/Scene/WeatherInspector.qml`, `WeatherBridge.cpp` |

Ordem no frame (ver `Docs/ARCHITECTURE.md` §5.2): wetness roda no passo 12, logo depois
do G-buffer. Cortina e partículas rodam no passo 22, **depois** do fog, de translúcidos e
da água, e antes do TAA/upscaler.

## Veredito

A base é boa. Ela segue a CryEngine (rain occlusion map, wetness deferred que reescreve
o G-buffer), é 100% procedural (não usa asset, buffer persistente nem compute) e já cuida
de motion vectors para o DLSS-RR. Os problemas que mais pesam:

1. **Corretude:** o mapa de oclusão nunca é invalidado; o terreno não entra nele; o knob
   "Oclusão" desliga também a colisão das partículas; "Cortina 0%" não esconde as gotas.
2. **Temporal:** o MV da cortina está errado quando a câmera translada. A chuva também
   não escreve as máscaras reactive/composition do FSR.
3. **Visual:** a chuva cai em linha reta (sem vento nem velocidade relativa); a gota é
   subpixel e cintila; só a key light ilumina a gota (postes e faróis não); a chuva
   aparece por cima da neblina sem atenuar.
4. **Desempenho:** a wetness copia GBufferA/B inteiros todo frame; o draw de splash
   processa 32768 instâncias, e só cerca de 10% delas estão vivas.

---

## 1. Bugs e inconsistências (corrigir primeiro)

### B1 — O mapa de oclusão nunca é invalidado · **alta**

`FRainWetness::InvalidateOcclusion()` (`RainWetness.h:76`) não tem nenhum chamador no
repositório. Hoje o mapa só é re-renderizado quando o centro snapado (quantum de 16 m) muda.

- **Sintoma:** carregar outra cena com a câmera na mesma célula deixa a oclusão da cena
  anterior. Mover, apagar ou importar objetos no editor também não atualiza o mapa. O
  resultado é um telhado fantasma deixando o chão seco, ou um interior novo molhando, até
  a câmera andar 8 m ou mais.
- **Proposta:** chamar `InvalidateOcclusion()` no import ou unload de cena, em mudanças de
  transform, visibilidade ou malha de objetos estáticos, e quando o terreno carregar.
  Para objetos dinâmicos, ver P5 (atualização parcial).

### B2 — O terreno não entra no mapa de oclusão · **alta**

A lista de occluders sai só de `AllItems` (`RendererPasses.cpp:1609-1617`). O terreno é
desenhado por fora (`Terrain.RenderGBuffer`), e o CSM só o recebe por um callback extra
(`TerrainCasters`).

- **Sintomas:**
  - sobre o terreno, a gota não "bate": não tem kill nem splash (o splash depende do
    topo do mapa);
  - a gota atravessa o chão e só some pelo depth test;
  - `DrainageMask` não enxerga relevo, então poças aparecem em encosta íngreme de malha
    plana do heightmap;
  - um morro entre a câmera e a chuva distante não corta o véu da cortina.
- **Proposta:** adicionar `Terrain.RenderRainOcclusion(...)` com o mesmo molde do
  `RenderShadowCascade` (ortho top-down, depth-only). Outra opção é amostrar direto o
  heightmap do terreno no `SkyVisibilityBand` e no VS das partículas e usar o maior dos
  dois.

### B3 — O knob "Oclusão" também desliga a colisão das partículas · **alta**

O comentário em `RendererPasses.cpp:1607` diz que *"as partículas sempre usam este mapa
para colisão; o knob decide apenas se ele também mantém interiores secos"*. Mas
`RainOccParams.x` é `GetRainOcclusion() && OccEverRendered` (`RainWetness.cpp:329`), e
os VS de gota e de splash só fazem kill e splash quando `RainOccParams.x > 0.5`. A
cortina também perde a oclusão.

- **Sintoma:** com "Oclusão" desligada, chove dentro de casa e embaixo de marquise, e
  não sai nenhum splash em lugar nenhum.
- **Proposta:** separar em dois sinais, `RainOccParams.x` (wetness respeita oclusão) e um
  novo `RainOccParams.?` (o mapa existe e é válido), e fazer partículas, splash e cortina
  usarem o segundo.

### B4 — "Cortina de gotas 0%" não esconde as gotas · **média**

`Weather.h:58` documenta `CurtainAmount = 0` como *"só wetness, sem gota visível no ar"*.
Só que `ExecuteParticles` é chamado sem condição, e `ParticleCount` depende apenas de
`RainAmount` (`RainWetness.cpp:339-341`).

- **Sintoma:** o slider do inspector não faz o que o rótulo promete; as gotas próximas
  continuam visíveis.
- **Proposta:** escalar `ParticleCount`/alpha por `CurtainAmount`, ou criar um knob
  próprio "Gotas próximas" e corrigir o comentário. Ver U2.

### B5 — O motion vector da cortina supõe streak fixo no mundo · **alta (temporal)**

Em `RainCurtain.ps.hlsl:150-163`, o ponto anterior é `wall + (0, speed·dt, 0)` projetado
com `PrevRainViewProj`. Isso supõe que o streak fica parado no mundo. Mas o cilindro é
centrado na câmera: `u` depende só de `dir`, e `wall = Cam + dir·t`. Logo, em XZ o padrão
**anda com a câmera**.

- **Sintoma:** ao andar ou dirigir, o MV aponta como se a chuva ficasse para trás,
  enquanto o padrão acompanha a tela. Isso dá ghosting e smear no TAA/DLSS-RR, pior em
  translação lateral.
- **Proposta:** usar como ponto anterior
  `wall + (PrevCam.xz - Cam.xz, 0) + (0, speed·dt, 0)` (o Y continua fixo no mundo,
  porque `v` usa `wall.y`). Validar com o `Tools/SmileMCP/scripts/dlss-rain-validation.mjs`
  andando com a câmera.

### B6 — Precisão do tempo (`f32` desde o boot) · **média**

`ElapsedTime` é `f32` em segundos desde o boot (`RendererFrameState.h:27`). Ele entra em
`fmod(h·boxH - cam + time·speed, boxH)`, em `frac(t + …)` dos ripples e na fase dos
splashes.

- **Sintoma:** com 10 h de sessão (t ≈ 36000 s, `time·speed` ≈ 4,3·10⁵), o ulp passa de
  3 cm por frame. Gotas e ripples começam a "tremer" e a andar aos saltos. Uma sessão de
  editor longa ou um servidor de captura chega nisso.
- **Proposta:** passar ao shader um tempo de chuva **embrulhado** no CPU, em `double` com
  módulo de um período comum. Como os períodos dependem de `speed` por gota, o mais
  simples é embrulhar a fase `time·speed` por eixo, mantendo a continuidade com um
  crossfade curto, ou usar um período longo (por exemplo, 1024 s) com fade de 1 s na
  dobra.

### B7 — Poça sobre metal continua metálica · **média**

A wetness reescreve só A/B. `GBufferC.r` (metallic) fica como está.

- **Sintoma:** uma poça sobre chapa ou tampa de bueiro metálico reflete com o F0 colorido
  do metal. Água por cima é dielétrica (F0 ≈ 0,02, IOR 1,33).
- **Proposta:** incluir C no passe, com uma cópia scratch a mais ou um MRT de 3 alvos, e
  fazer `metallic = lerp(metallic, 0, puddle)`. Opcionalmente, guardar a cor base
  original para a camada sob a água (ver Q8).

### B8 — `Load` fora da faixa em `uv == 1.0` · **baixa**

`RainParticles.vs.hlsl:37` e `RainSplash.vs.hlsl:34` fazem
`Load(int2(uvz.xy * size))` sem clamp. Com `uv == 1` exato, o índice vira 512, o `Load`
fora da faixa retorna 0 e a gota morre na borda. É raro, mas é um clamp de uma linha:
`min(int2(...), size-1)`.

### B9 — Uma rampa de chuva em runtime reseta o GI todo frame · **média (risco)**

`SetRainAmount` chama `MarkIndirectLightingDirty()` (coalescido) a cada mudança quando
`DriveSky` está ativo (`RenderSettings.cpp:1099-1106`). Isso funciona bem no slider. Mas
uma transição de clima de gameplay (sair de 0 e chegar a 1 em 60 s) muda o valor **todo
frame**, e o DDGI/ReSTIR ficaria em reset contínuo.

- **Proposta:** suavizar no Renderer o `RainSky` efetivo com o mesmo filtro do
  `Wetness`, e invalidar só por **degrau** (por exemplo, Δ > 0,05 acumulado). Outra opção
  é baixar a hysteresis do DDGI temporariamente em vez de resetar.

### B10 — Desalinhamento entre comentário e layout do CB · **baixa**

`RainWetness.h:23` diz `RainParams1: x = 1/PuddleScale, yzw = -`, e
`RainCommon.hlsli:12` diz `zw = -`. Na prática, `y = 1/OccRange` e `z = RainAmount`
instantâneo (`RainWetness.cpp:327`). Como os dois layouts "espelham" um ao outro,
documentação errada aqui vira bug na próxima mexida. Corrigir os dois comentários e, de
preferência, adicionar `static_assert(offsetof(...))` nos campos.

### B11 — O hot reload não recria os PSOs do mapa de oclusão · **baixa**

`Recreate()` reconstrói wetness, cortina, partículas e splash, mas não `OccOpaquePSO` nem
`OccMaskedPSO`, que usam `ShadowDepth.vs/ps`. `ShaderStems()` também não lista esses
stems. Editar `ShadowDepth.*` atualiza o CSM, mas não a oclusão da chuva.

---

## 2. Desempenho

> Antes de qualquer mudança, medir. Já existem os scopes "Chuva — wetness" e
> "Chuva — cortina/gotas". Sugestões: abrir scopes separados para *mapa de oclusão*,
> *cortina*, *gotas* e *splash*, e capturar com Bistro em 1440p e 4K, com e sem DLSS.

### P1 — A wetness copia GBufferA/B inteiros todo frame · **alta**

`RainWetness.cpp:378-379` faz dois `CopyResource` de tela cheia (RGBA8 + RGB10A2,
8 B/px), mais a leitura e a reescrita. Em 4K isso dá cerca de 130 MB de tráfego por
frame, e acontece **mesmo quando a molhadura é residual** (`Active()` durante 30 s de
secagem). Opções, da mais barata para a mais completa:

1. **Aplicar a wetness dentro do PS do G-buffer:** um include compartilhado
   `RainSurface.hlsli` chamado pelos materiais e pelo `TerrainGBuffer.ps`. Elimina cópia,
   passe e barreiras. O custo é tocar os shaders de material e ter permutação
   chuva on/off.
2. **Compute in-place com UAV:** RGB10A2 aceita typed UAV load na maioria do hardware
   D3D12 (verificar `D3D12_FEATURE_DATA_FORMAT_SUPPORT`). A precisa de um alias
   `R8G8B8A8_UNORM` com conversão sRGB manual. Assim não há cópia, e dá para classificar
   por tile e pular tiles de céu ou totalmente cobertos.
3. **Manter o fullscreen, copiando só o necessário:** marcar por stencil (ou por tile)
   os pixels expostos com `N.y` relevante e copiar por região.

### P2 — O draw de splash processa 32768 instâncias, com ~10% vivas · **média**

A janela do splash é `0.22 s · speed` de queda num ciclo de `boxH = 24 m`. Com
`speed ∈ [10,2; 16,2] m/s`, isso dá 2,2–3,6 m de 24 m, ou seja, 9–15% das instâncias vivas.
O resto vira quad degenerado depois de calcular hash, `Load` do mapa e as duas matrizes.
Além disso, o box envolve a câmera inteira e mais da metade das gotas fica fora do frustum.

- **Proposta:** um compute curto por frame que gera posição, faz cull por frustum,
  oclusão e "splash vivo", e compacta em dois append buffers com `ExecuteIndirect`.
  O ganho é grande: VS só nas instâncias visíveis, contagem de gotas livre de crescer, e
  o caminho abre espaço para vento, velocidade relativa e colisão com o depth (ver Q1).
- **Alternativa barata:** distribuir as gotas num **volume alinhado ao frustum**
  (Tatarchuk, ToyShop) em vez do box simétrico.

### P3 — A cortina faz ~48 `Load` por pixel em resolução cheia · **média**

`RainCurtain.ps.hlsl`: o véu tem 8 taps × `SkyVisibility` (PCF 2×2 = 4 `Load`); cada
cilindro tem mais um `SkyVisibilityBand` (4 `Load`); somam-se `atan2`, `pow` e 2
`StreakPattern` por cilindro.

- **Propostas:**
  - rodar em meia resolução com upsample bilateral por depth (os streaks são de baixa
    frequência a 12/28 m);
  - pular o raymarch do véu quando o mapa não tem nenhum occluder no volume (flag no CPU
    ao renderizar o mapa);
  - trocar os 4 `Load` do PCF por um `GatherRed` (1 fetch).

### P4 — CPU: vetor de occluders montado todo frame · **baixa**

`RendererPasses.cpp:1610-1615` aloca e preenche `std::vector` com todo o `AllItems` todo
frame, mesmo quando `RecordOcclusionMap` vai retornar por cache. Melhor checar o cache
antes (`NeedsOcclusionUpdate(CamPos)`) ou passar um span e um adaptador sem cópia.

### P5 — Pico ao cruzar o quantum de 16 m · **baixa/média**

Ao cruzar a fronteira, o mapa inteiro de 120 m é redesenhado num frame, o que causa um
spike em cena densa. As opções são: mapa **toroidal/rolante** (redesenhar só a faixa nova
e endereçar com wrap), ou dividir o redraw em N frames com dois mapas em ping-pong. Isso
também resolve objetos dinâmicos: com orçamento por frame, dá para re-renderizar só as
células sujas.

---

## 3. Qualidade visual

### Q1 — Vento e velocidade relativa da câmera · **alta**

Hoje o eixo da gota é sempre `(0,-1,0)` (`RainParticles.vs.hlsl:43`), e a cortina rola só
em Y. Não existe vento na chuva, embora `Water` (`SetWindDirection/Speed`) e
`VolumetricClouds` (`WindParams`) já tenham o seu.

- **Proposta:** um `FWeather::Wind` (direção, velocidade, rajada) como **fonte única**
  para água, nuvens e chuva.
  - Na gota: velocidade = queda + vento, e o eixo do streak é
    `normalize(v_gota - v_câmera)`, que inclina ao correr ou dirigir, como deve.
  - Na cortina: cisalhamento de `u` com a altura.
  - No mapa de oclusão: com vento, a chuva entra inclinada debaixo da marquise. Dá para
    projetar o ortho na direção da chuva em vez de reto para baixo (a Cry faz isso).

### Q2 — Tamanho e velocidade da gota pela intensidade · **média**

Hoje só a **contagem** escala com `RainAmount`: a velocidade é fixa em 12 m/s ±25% e a
largura é fixa. Na física, a distribuição de tamanho (Marshall–Palmer) muda com a taxa de
chuva, e a velocidade terminal depende do diâmetro (Gunn–Kinzer: ~2 m/s em 0,5 mm,
~6,5 m/s em 2 mm, ~9 m/s em 5 mm).

- **Proposta:** amostrar o diâmetro por gota a partir de uma distribuição parametrizada
  pela intensidade e derivar daí velocidade, comprimento do streak, alpha e tamanho do
  splash. Assim a garoa sai fina, lenta e densa, e a tempestade sai grossa e rápida. Os
  presets "Garoa" e "Tempestade" passam a ter visual diferente, e não só densidade
  diferente.

### Q3 — Iluminação das gotas: luzes locais, sombra e neblina · **alta**

Gota, splash e cortina usam só key light + ambient do céu.

- Chuva noturna sob poste ou farol é *o* visual de chuva em jogo, e hoje ela é invisível
  nesses cones. Proposta: no VS das gotas (é por vértice, então é barato), somar as luzes
  locais do `LightBuffer` com o mesmo cull da direta, ou **reusar o froxel do volumetric
  fog** (in-scattering já integra luzes locais e sombras): amostrar a radiância do froxel
  na posição da gota.
- Sombra: gota dentro da sombra de um prédio continua recebendo o tint da key. Uma
  amostra do CSM por gota no VS resolve.
- Forward scattering: o termo `pow(…,4)` é heurístico. Os dados de Garg & Nayar (aparência
  do streak por direção de luz e de visão, e oscilação da gota) são a referência para um
  lobo melhor ou uma pequena LUT.

### Q4 — Gota subpixel cintila (aliasing) · **alta**

A largura vai de 4,5 a 8,5 mm (`RainParticles.vs.hlsl:45`). A partir de 2–3 m, isso é
menos de 1 px: a gota liga e desliga entre frames, e o TAA/DLSS interpreta como ruído ou
apaga.

- **Proposta:** a técnica "phone-wire AA" (Persson/Humus, *GPU Pro 5*). No VS, calcular a
  largura em pixels; se for menor que 1 px, prender a largura em 1 px e multiplicar o
  alpha pela razão `largura_real / largura_clamp`. Isso conserva a cobertura média e
  elimina o flicker. O mesmo vale para o comprimento e para o splash longe.

### Q5 — A chuva fura a neblina · **média**

Cortina e gotas são compostas **depois** do fog (passo 22) sem atenuar pela transmitância.
Com fog pesado (que a F4 ainda engrossa em 2,5×), o streak a 28 m aparece com o mesmo
contraste do streak a 2 m.

- **Proposta:** multiplicar a contribuição pela transmitância do height fog ou da aerial
  perspective na distância do streak (`t`) e somar o in-scatter correspondente. Outra
  opção é compor a chuva *antes* do fog, o que exige que o fog leia um alvo separado.

### Q6 — Ripples: densidade e água de verdade · **média**

- `RippleLayer` sempre tem uma gota por célula. A intensidade só escala a **amplitude**.
  Na garoa deveria haver **menos** anéis, não anéis mais fracos. Proposta: gate por hash
  e por ciclo, como o splash da F4 já faz
  (`Hash21(cell + cyc) < rainNow`).
- **A chuva não toca a água** (oceano e lago são um pipeline à parte, passo 19). Faltam
  ripples na superfície da água, splash na água e o "chiado" de alta frequência na normal
  do oceano. Proposta: expor um termo de normal de chuva para o PS da água (o mesmo
  `RippleGradient`) e incluir a superfície da água como topo de colisão para o splash.

### Q7 — Splashes · **média**

- O billboard é **vertical** de frente para a câmera (`RainSplash.vs.hlsl:47`). Visto de
  cima (câmera alta, drone, top-down), fica de perfil e some. Proposta: combinar uma coroa
  vertical com um anel horizontal (decal no plano do topo), com peso por `|dir.y|`.
- O ponto de impacto é amostrado por *point* no mapa (23 cm/texel), então o splash pode
  flutuar ou afundar perto de quina. Usar o tap bilinear do `OccDepthBilinear` com o
  depth da cena como desempate.
- Tempestade: faltam névoa e spray rente ao chão (camada de "mist" baixa e densa perto da
  superfície), que vendem muito a intensidade.

### Q8 — Poças: controle artístico e modelo físico · **média**

Hoje a máscara é um value noise em XZ com limiar (`PuddleMask`) e drenagem pelo mapa.
Melhorias, em ordem de impacto:

1. **Máscara de artista:** os canais livres `GBufferC.gba` (`ARCHITECTURE.md` §5.3) podem
   carregar, por material, *porosidade* e *aceita poça* (ou um mask texture ou vertex
   color). Isso evita poça em capô de carro, mesa e telhado inclinado plano.
2. **Água preenche as frestas primeiro** (Lagarde, *Water drop 2b / 3a*): usar a altura do
   material (height ou AO) com o nível d'água subindo com o `Wetness`. A poça nasce nos
   rejuntes e cresce, em vez de aparecer pelo noise.
3. **Pontos baixos:** usar o mapa de oclusão como heightmap e favorecer mínimos locais
   (a mesma ideia da `DrainageMask`, mas com sinal positivo).
4. **Camada d'água:** F0 da água ≈ 0,02 por cima, metallic → 0 (B7), e o albedo de baixo
   escurecido. Isso aproxima o modelo de duas camadas do Lagarde.

### Q9 — Porosidade vem da roughness · **baixa**

`porosity = saturate((rough - 0.5)/0.3)` (`RainWetness.ps.hlsl:131`) é a heurística da
Cry. Funciona na média, mas erra em materiais como mármore polido poroso ou plástico
rugoso não poroso. Resolver junto com Q8.1 (parâmetro por material, com a heurística como
fallback).

### Q10 — Superfícies verticais e vidro · **média**

Parede hoje recebe 30% de wetness, de forma uniforme.

- Escorrimento (streaks e filetes) em parede e vidro: uma máscara procedural vertical
  rolando com o tempo, modulada pela oclusão. Isso deixa seca a área sob o beiral.
- Gotas em vidro e para-brisa: uma normal procedural no PS dos translúcidos.
- (Opcional) Gotas na lente quando a câmera olha para cima, como pós-efeito com toggle.

### Q11 — Secagem uniforme · **baixa**

`Wetness` é um escalar global (τ 5 s / 30 s, `RendererFrame.cpp:279-286`). Na prática,
o chão seca em manchas: o poroso seca depois e a poça seca por último (esta última já
acontece em parte, pelo limiar). Propostas: limiar com noise por pixel e taxa por
porosidade; gotejamento sob árvores e beirais por alguns segundos depois que a chuva para.
Expor τ de molhar e de secar como knobs.

### Q12 — A wetness não chega em translúcidos nem nos hits de RT · **baixa/média**

Ela é aplicada no G-buffer, então translúcidos forward (vidro, água) não molham, e as
reflexões, o GI e o DDGI veem material **seco** nos hits (já documentado em
`RenderSettings.cpp`). Extrair a lógica para `RainSurface.hlsli` e chamar também nos
hit shaders resolve, e combina com P1.1.

### Q13 — A cortina anda com a câmera · **baixa**

É o mesmo fato de B5, mas visto como aparência: ao andar, os cilindros de 12/28 m não têm
parallax de translação, e em velocidade alta (veículo) a chuva parece "presa" na tela.
Propostas: deslocar `u` pela distância percorrida tangencial (fase acumulada no CPU), ou
reduzir a cortina ao véu distante quando as partículas estiverem ativas.

### Q14 — Relâmpago e trovão · **feature**

Clarão direcional curto (key light temporária, clarão nas nuvens volumétricas, com
invalidação controlada do GI) com cronograma aleatório por intensidade, e um hook de
áudio. A referência é Tatarchuk (ToyShop), que tem um sistema de iluminação por
relâmpago.

---

## 4. Integração temporal (TAA / DLSS / DLSS-RR / FSR)

- **T1 — B5** (MV da cortina na translação) é o item mais importante desta seção.
- **T2 — Máscaras do FSR:** a água escreve reactive e composition
  (`SceneTargets.h:62-65`), mas a chuva não. Gotas, splash e cortina deveriam escrever
  `reactive ≈ alpha` (e composition onde não há MV confiável, como no véu). Com isso o
  FSR não acumula nem borra os streaks.
- **T3 — Partícula fina antes do upscale:** mesmo com Q4, uma gota de 1 px em render-res
  vira um rastro reconstruído. Avaliar compor gotas e splashes **depois** do upscale, em
  resolução de saída (o padrão "translucency after TSR" da Unreal). O custo é perder o
  "motion blur grátis" do acumulador, então isso exige um streak com comprimento físico
  (`speed × tempo de exposição`).
- **T4 — Splash:** `prevLife < 0 ⇒ prevLife = life` faz o splash nascer sem histórico
  válido. Isso está certo, mas o MV fica zero no frame de nascimento. Avaliar zerar também
  a contribuição no reactive mask nesse frame.
- **T5 — Validação:** estender o `dlss-rain-validation.mjs` com um caminho de câmera
  (translação lateral, veículo, olhar para cima) e uma métrica de flicker (o engine já tem
  heatmap de flicker, `ARCHITECTURE.md` passo 23).

---

## 5. Arquitetura e código

- **A1 — `FRainWetness` faz cinco coisas** (wetness, mapa de oclusão, cortina, gotas,
  splash) com root signature compartilhada. Sugestão: dividir em `FRainOcclusion`
  (mapa + cache + invalidação), `FRainSurface` (wetness) e `FRainPrecipitation`
  (cortina + partículas + splash), com o CB comum num header.
- **A2 — Números mágicos duplicados:**
  - 12 m/s (CPU), 0,22 s (nos dois VS do splash) e box 12/24 m vivem em lugares
    diferentes;
  - o tint `keyLum/lerp 0.35` está copiado em 3 shaders, com coeficientes levemente
    diferentes (0,03–0,11 / 0,04–0,14 / 0,10);
  - a conversão clip → UV do MV está copiada em 3 PS.

  Sugestão: `RainTint()`, `ClipToUvVelocity()` e as constantes no CB ou num
  `RainConfig.hlsli`.
- **A3 — Blend states duplicados** em `BuildCurtainPSO` e `BuildParticlesPSO`; extrair
  um helper.
- **A4 — Testes:** não há nenhum teste de chuva em `Tests/`. Candidatos baratos:
  - integração do `Wetness` e `SettledWetness()`;
  - `RainWrapAxis` (continuidade world-fixed, em CPU espelhando o HLSL);
  - `static_assert` de offsets do CB contra o `.hlsli` (B10);
  - snap do mapa (cache invalida ao cruzar o quantum).
- **A5 — Estado de clima não persistido:** não encontrei serialização de `FWeather` na
  cena. O único caminho fora do inspector é o override de captura de `RainAmount`
  (`RenderSettingsController.cpp:246`). Poças, ripples, oclusão e DriveSky se perdem ao
  reabrir a cena. Confirmar e, se for o caso, salvar no `.sscene` (o
  `CAPTURE-PROTOCOL.md` já cobre o `Wetness` assentado).

---

## 6. UX do editor

- **U1 — O preset não reflete o estado:** `presetIndex` é estado local do QML
  (`WeatherInspector.qml:12`). Ao reabrir o inspector ou carregar uma cena, ele volta a
  "Personalizado" mesmo que os valores batam com um preset. O preset "Seco" só zera
  `RainAmount` e deixa o resto como estava. Sugestão: derivar o preset comparando os
  valores, ou persistir o preset no `FWeather`.
- **U2 — Knobs que faltam:**
  - vento (direção, força, rajada) — Q1;
  - densidade e tamanho das gotas próximas — B4, Q2;
  - splashes on/off e intensidade;
  - densidade de ripples — Q6;
  - tempo para molhar e para secar — Q11.
- **U3 — Visualizações de debug:** mapa de oclusão, máscara de exposição ao céu, máscara
  de poça e drenagem, e o `Wetness` atual. Dá para pendurar no preview de render targets
  que já existe.
- **U4 — Transições de clima:** uma API `SetRainTarget(valor, duração)` com curva, base
  para gameplay e para um timeline de clima, que depende de B9.
- **U5 — Feedback:** mostrar o "molhado atual" (somente leitura) ao lado da intensidade.
  Sem isso, o usuário zera a chuva e não entende por que o chão continua molhado por 30 s.

---

## 7. Ordem sugerida de execução

| Ordem | Itens | Motivo |
|---|---|---|
| 1 | B1, B2, B3, B4, B8, B10 | correções pequenas e localizadas que mudam o comportamento visível |
| 2 | B5 (+ T2) | maior defeito temporal; barato |
| 3 | Q4, Q5 | antialiasing da gota e neblina: grande ganho visual com pouco código |
| 4 | medição + P1, P2 | os dois maiores custos de GPU |
| 5 | Q1 (+ U2 vento), Q3 | vento e luzes locais: o salto de qualidade |
| 6 | B6, B7, B9 | robustez (sessão longa, metal, transições) |
| 7 | Q2, Q6–Q8, Q10 | física da gota, água, poças com controle |
| 8 | A1–A5, U1, U3–U5, Q11–Q14 | limpeza, ferramentas e features |

## Referências

- Sébastien Lagarde, série *Water drop* (Dontnod, 2012–2013): *Observe rainy world*,
  *Dynamic rain and its effects*, *Physically based wet surfaces*, *Reflecting wet
  world* — seblagarde.wordpress.com. Índice:
  [3dvf](https://3dvf.com/actualite-5676-sebastien-lagarde-simulation-pluie-en-jeu-video-html/).
  Exemplo de implementação com água F0 0,02:
  [gist](https://gist.github.com/eviltak/e04b83ffdb91aa3d477bbfe0ca370da7).
- Natalya Tatarchuk, John Isidoro, *Artist-Directable Real-Time Rain Rendering in City
  Environments* (EG NPH 2006 / SIGGRAPH 2006 Course 26, demo ToyShop):
  [slides](https://advances.realtimerendering.com/s2006/Tatarchuk-Rain.pdf),
  [capítulo](https://advances.realtimerendering.com/s2006/Chapter3-Artist-Directable_Real-Time_Rain_Rendering_in_City_Environments.pdf).
- Kshitiz Garg, Shree Nayar, *Photorealistic Rendering of Rain Streaks* (SIGGRAPH 2006):
  [SIGGRAPH](https://history.siggraph.org/?p=113590); uso em tempo real no NVIDIA SDK 10
  (rain textures por direção de luz e de visão):
  [SDK 10](https://developer.download.nvidia.com/SDK/10.5/direct3d/samples.html).
- Emil Persson, *Wire Antialiasing* (GPU Pro 5) / Humus *Phone-Wire AA*:
  [Geeks3D](https://geeks3d.com/?p=8334), [GPU Pro 5](https://www.oreilly.com/library/view/gpu-pro-5/9781482208641/chapter-52.html).
- Tiago Sousa, *Graphics Gems from CryENGINE 3* (SIGGRAPH 2013):
  [slides](https://www.slideshare.net/slideshow/graphics-gems-from-cryengine-3-siggraph-2013/25052583);
  entidade Rain da CryEngine (oclusão, poças, splashes):
  [docs](https://cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/11240646).
- Gunn & Kinzer (1949), velocidade terminal × diâmetro; Marshall & Palmer (1948),
  distribuição de tamanhos: [SJSU](https://www.sjsu.edu/faculty/watkins/raindrop.htm),
  [NSF PAR](https://par.nsf.gov/biblio/10533483).
- AMD FSR 2.1+, reactive mask e transparency & composition mask para partículas:
  [GPUOpen](https://gpuopen.com/meet-fidelityfx-super-resolution-2-1),
  [plugin UE parte 4](https://gpuopen.com/learn/fsr-2-1-unreal-engine-plugin-part4/).
