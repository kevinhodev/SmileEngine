# Auditoria do Sistema de Oceano — SmileEngine

**Data:** 2026-07-30
**Escopo:** `Engine/{Include,Source}/…/Graphics/{OceanFFT,Water}.{h,cpp}` + `Shaders/Water/*` (14 arquivos, ~3.100 linhas), mais os pontos de integração em `Renderer.cpp`.
**Método:** análise estática. 100% dos arquivos lidos, mais 3 agentes de auditoria em paralelo, com reverificação manual de toda afirmação contestada.
**Referência primária:** Horvath, C. J. *Empirical Directional Wave Spectra for Computer Graphics.* DigiPro '15, pp. 29–39. DOI `10.1145/2791261.2791267`.

> **Limite honesto: o engine nunca foi executado nesta auditoria.** É Windows + Qt 6 + D3D12; a análise rodou em container Linux. Nada aqui foi confirmado visualmente ou com captura de frame. Os achados que dependem de confirmação visual estão marcados como tal, com o teste que os resolve.

---

## Veredito

O núcleo matemático é **melhor que a média do que se vê em implementações de FFT ocean**. A borboleta DIT, a disciplina de barreiras, a expressão de `h(k,t)`, o empacotamento complexo de Dx/Dz e — notavelmente — as bandas espectrais disjuntas entre cascatas estão todos corretos. Um detalhe do tratamento de `h₀(±k)` é inclusive *melhor* que a formulação canônica do Tessendorf, e coincide com a correção que o Horvath propõe (§7.1.2 do paper).

O que está quebrado é a **ponte entre a matemática e as unidades de mundo**. `WorldSize` nunca é atribuído, o fator de área de célula `Δkx·Δky` do espectro não existe, e uma cadeia de nove constantes mágicas foi calibrada à mão para compensar. É isso que torna os sliders não-físicos, a espuma e as normais mal-pesadas entre cascatas, e o `WindSpeed` praticamente decorativo.

O contraste com o Horvath é direto. Ele escreve, sobre a implementação dele:

> *"No fudge parameters (amplitude, filtering) are used - just physical constants."*

---

## Tabela de severidade

| # | Sev | Achado | Local |
|---|-----|--------|-------|
| 1 | 🔴 | `WorldSize` = 1.0, nunca atribuído → corte de vento do Phillips inerte | `OceanFFT.h:74` |
| 2 | 🔴 | Sinal do choppiness invertido (cristas chatas, cavas pontudas) — *pendente de confirmação visual* | `OceanUpdateSpectrum.cs.hlsl:26` + `OceanCreateDisplacement.cs.hlsl:18` |
| 3 | 🔴 | Geomorph fixado em zero → LOD **popa** | `WaterGenerateDraws.cs.hlsl:256` |
| 4 | 🔴 | Água apaga geometria alpha-blended na frente dela | `Water.cpp:210-215` |
| 5 | 🔴 | Água fora do G-buffer → quebra os guides do DLSS Ray Reconstruction | `Renderer.cpp:3101` |
| 6 | 🟠 | PS amostra a FFT na posição **deslocada** → normal/espuma fora da geometria | `WaterSurface.vs.hlsl:83-84` |
| 7 | 🟠 | `exp(-k²l²)` ausente no Phillips → energia cheia até Nyquist | `OceanFFT.cpp:29-46` |
| 8 | 🟠 | Jacobiano/slope sem divisão pelo texel de mundo → pesos 2,7×/5,3× errados | `OceanGradients.cs.hlsl:27-30` |
| 9 | 🟠 | `saturate` interno mata o fade de distância da refração | `WaterSurface.vs.hlsl:101` |
| 10 | 🟠 | Jacobiano amostrado em LOD 0 de textura com 1 mip → espuma aliasa | `WaterCommon.hlsli:51` |
| 11 | 🟠 | `0.125` no `SetTime` fixa tile de 64 m → velocidade errada se `WavesAmount≠1` | `OceanFFT.h:22` |
| 12 | 🟠 | Escrita CPU em heap UPLOAD sem fence, com a GPU possivelmente lendo | `OceanFFT.cpp:92-118` |
| 13 | 🟠 | **Fator `Δkx·Δky` ausente na amplitude de h₀** (achado novo, via Horvath eq. 46-47) | `OceanFFT.cpp:82-83` |
| 14 | 🟡 | Direção do vento provavelmente invertida (e briga com o fluxo da espuma) | `OceanFFT.cpp:33-34` |
| 15 | 🟡 | Ondas achatadas num raio de ~2 m da câmera | `WaterSurface.vs.hlsl:41-42` |
| 16 | 🟡 | BRDF é Phong de 2 lóbulos, sem normalização, sem G, sem Fresnel no lóbulo do sol | `WaterCommon.hlsli:186-192` |
| 17 | 🟡 | Fresnel com fudge `lerp(0.5,1,gloss)` → quebra o limite rasante | `WaterCommon.hlsli:112-115` |
| 18 | 🟡 | Velocity só de câmera → TAA/DLSS não reprojeta a fase da onda | `WaterSurface.vs.hlsl:91-94` |
| 19 | 🟡 | `WaterGenerateDraws.cs` fora do hot-reload | `Renderer.cpp:825-826` |
| 20 | 🟡 | Corte espectral em parede-de-tijolo, sem taper → ringing nas bordas de banda | `OceanFFT.cpp:74-79` |
| 21 | 🟡 | RNG sequencial em vez de hash por texel → mudar N muda o mar inteiro | `OceanFFT.cpp:57` |
| 22 | 🟡 | `w == 0` não rejeitado no Marsaglia polar → NaN envenena o mapa inteiro | `OceanFFT.cpp:22` |
| — | ⚪ | **Faltando:** flutuação/física, SSR, submerso, costa, testes, docs, UI de tuning | — |

---

## Os cinco primeiros

### 1. 🔴 `WorldSize` nunca é atribuído — a raiz de quase toda a fragilidade

```cpp
// Engine/Include/Smile/Graphics/OceanFFT.h:74
f32 WorldSize = 1.0f;
```

Grep no repositório inteiro: **3 ocorrências** — a declaração e duas leituras (`OceanFFT.cpp:53` e `:75`). Nunca é escrito. Logo `k = (128−n)·2π` em "radianos por tile", não rad/m.

O Horvath torna a consequência precisa. Ele demonstra (eq. 21, 26, 27) que o espectro do Tessendorf **é** um espectro A,B — praticamente idêntico ao Pierson-Moskowitz:

```
S_Tessendorf(k) = A_T · exp(−g²/(U⁴k²)) / k⁴ · D(k)          (21)
  ⟺  A_ab = 2·A_T·g²,   B_ab = (g/U)⁴                        (26, 27)
```

> *"Given that A_T is unspecified, the Tessendorf Spectrum is nearly identical to the Pierson-Moskowitz spectrum, differing only in a multiplier of 0.6858 on the coefficient B, or alternatively, a 1.0989 multiplier on the wind speed U."*

Ou seja: **a velocidade do vento entra no espectro por exatamente um lugar — o termo `exp(−g²/(U⁴k²))`**, que é o `exp(−1/(k²L²))` com `L = U²/g` da linha 42. E é justamente esse termo que está morto: com `k ≥ 4π ≈ 12.6` (o piso de banda, em unidades de tile) e vento de 4 m/s, `exp(−1/(k²L²)) ≈ 0.998`.

**`WindSpeed` é praticamente um liga/desliga**, não um controle de estado de mar. Só `Amplitude` e o termo direcional respondem.

Consequências em cadeia:
- `ω = √(gk)` sai errado por `√L`, remendado pelo `0.125` mágico no `SetTime` (achado #11), que por sua vez só vale para tile de 64 m.
- Os boosts `2.2 / 4.5` por cascata (`Water.cpp:846`) são a compensação empírica.

O próprio autor documentou o sintoma em `Water.cpp:843-845`:
> *"Phillips não tem o corte de vento em unidades de mundo aqui, então o swell sai gigante."*

Atribuir `WorldSize = TileSize` e remover os remendos resolve #1, #7, #8, #11, #13 e boa parte de #14 de uma vez. É a correção de maior alavancagem do sistema.

### 2. 🔴 Sinal do choppiness — cristas chatas e cavas pontudas

Dois flips compõem.

**Flip A** — o código usa `k_code = 128 − n`:
```hlsl
// Shaders/Water/OceanUpdateSpectrum.cs.hlsl:26-27
float2 k = float2(float(int(DISP_MAP_SIZE) / 2) - float(loc1.x),
                  float(int(DISP_MAP_SIZE) / 2) - float(loc1.y));
```
Mas o twiddle é `e^{+iθ}` (`OceanFFT.cs.hlsl:29`) e o shift `(−1)^(x+y)` (`OceanCreateDisplacement.cs.hlsl:13`) fazem a síntese ser `Σ H[n]·e^{+2πi(n−128)x/N}`. Ou seja, o slot `n` carrega `k_true = (n−128)Δ = −k_code`.

**Flip B** — a altura é negada na saída (`OceanCreateDisplacement.cs.hlsl:18`, `-h`).

Os dois juntos fazem o código implementar **fielmente a fórmula do Tessendorf como escrita**: `D̃ = −i·k̂·h̃` com λ > 0, sob convenção de síntese `e^{+ikx}`.

Essa combinação dá o sinal errado. Derivado por dois caminhos independentes:

*Potencial de velocidade (onda linear de água profunda).* Com `φ = (Aω/k)e^{kz}sin(kx−ωt)`, integrando `u = ∂φ/∂x` e `w = ∂φ/∂z` no tempo:
```
ζ = A·cos(kx−ωt)        (vertical)
ξ = −A·sin(kx−ωt)       (horizontal)
```
**Logo `h = A cos θ` exige `Δx = −A sin θ`.** A relação é independente da direção de propagação — é uma afirmação sobre a forma instantânea da superfície.

*Trocóide, conferido numericamente.* Com `x = a − sin a, y = cos a`, a altura cai de 1 a 0 em `x ∈ [0, 0.57]` e de 0 a −1 em `x ∈ [0.57, 3.14]` → crista estreita, cava larga ✔. Com `x = a + sin a`, o inverso.

Aplicando o operador `−i·k̂` a `h = cos(kx)`: os coeficientes viram `(−i/2)e^{ikx} + (i/2)e^{−ikx} = +sin(kx)`. Ou seja `Δx = +A sin θ` — **o trocóide invertido**. Cristas alargam, cavas viram bico.

Corolário: como a espuma vem do Jacobiano (dobra), ela nasceria **nas cavas em vez das cristas**. Casa com o comentário do autor em `WaterSurface.ps.hlsl:215-217` de que a versão sem tweak "tomou conta do mar".

> **Confiança alta na álgebra; não confirmado visualmente.** Teste de 30 segundos que encerra a discussão: rodar com `ChoppyScale` bem alto e olhar o perfil da onda. Bico pra cima = correto; bico pra baixo = confirmado.
>
> Correção: **um caractere.** Trocar `128 − loc` por `loc − 128` em `OceanUpdateSpectrum.cs.hlsl:26-27` **ou** tirar o `-` de `-h` na linha 18 do `OceanCreateDisplacement` — um dos dois, nunca os dois.

### 3. 🔴 Geomorph é código morto — o LOD popa

```hlsl
// Shaders/Water/WaterGenerateDraws.cs.hlsl:256
Source.Data2 = PackTileData2(0u, Pattern, 255u);
//                           ^^ MorphUnorm
```
A assinatura é `PackTileData2(uint MorphUnorm, uint Pattern, uint CoverageUnorm)` (linha 85). O primeiro argumento é sempre `0`. Do outro lado, `WaterSurface.vs.hlsl:24` lê `geomorph = (InstanceData2 & 0xFF)/255.0` → sempre `0.0`, e o `lerp` da linha 31 sempre devolve `localUV`.

Toda a maquinaria de `nextLodStep` / `coarseUV` / `sampleWorldXZ` está inerte, e as transições entre anéis de LOD estalam sem blend. O debug view de geomorph (`WaterSurface.ps.hlsl:35-36`) sempre mostra zero.

Detalhe adicional: mesmo se ligado, `geomorph *= saturate((1.0 - GridPos.z) * 16.0)` (`vs:28`) zera o morph justamente na **borda** do tile — morferia o interior, não a costura. Ligar o valor sozinho não basta.

### 4. 🔴 A água apaga geometria transparente na frente dela

Translúcidos desenham em `Renderer.cpp:2634-2657` com `PSOForwardBlend`, cujo depth state é `DEPTH_WRITE_MASK_ZERO` (`PipelineState.cpp:356`). A água desenha depois, opaca, com `DepthWriteMask = ALL` e `BlendEnable = FALSE` (`Water.cpp:210-215`) — e passa no teste de profundidade contra o depth **pré-translúcido**. Qualquer partícula, vidro ou efeito alpha na frente do mar some.

Duplamente errado: esses mesmos translúcidos estão dentro do `SceneColorCopy` (capturado em `:2675`, depois do pass translúcido) e viram fonte da refração.

### 5. 🔴 A água está fora do G-buffer — e isso quebra o DLSS-RR

A água é um pass forward injetado depois de todo o deferred (`Renderer.cpp:2692`). Nunca escreve no G-buffer, então:
- não recebe GTAO, nem ReSTIR GI, nem SSR;
- **nada da cena reflete na água** (o trace de reflections roda em `:2462`, 230 linhas antes do desenho da água) — nem barco, nem terreno, nem personagem;
- pior: `RRGuides.RecordGuides` lê `GBuffer.SRVTableStart()` (`Renderer.cpp:3101-3103`), então os pixels de água entregam ao DLSS Ray Reconstruction o albedo/normal/roughness de **quem estava atrás da água**. No caminho RR isso vira mis-denoising no oceano inteiro.

Combinado com #18 (velocity sem a fase da onda), é o mecanismo por trás de ghosting/smearing nas cristas.

---

## O que o Horvath 2015 diz sobre este código

### Confirma o achado #7 — o `exp(−k²l²)` ausente é conselho do próprio Tessendorf

Horvath, §5.1.3:

> *"Tessendorf advises that his spectrum by itself has 'poor convergence properties for high values of k', and advises damping out small scale waves and multiplying the final spectrum by the factor `exp(−k²l²)`, for a user-provided small wavelength `l ≪ L`."*

`ComputePhillips` (`OceanFFT.cpp:29-46`) não tem esse fator. Agravante: a banda superior da cascata 0 é **129 ciclos** (`Renderer.cpp:126`) — *acima* de Nyquist em 128. Os menores comprimentos de onda carregam energia `k⁻⁴` cheia até o limite de amostragem. É a origem clássica do cintilar em ângulos rasantes, e é o que a filtragem Toksvig está tendo que absorver sozinha.

### Achado NOVO #13 — falta o fator de área de célula `Δkx·Δky`

Horvath, eq. 46-47:
```
ā(kx, ky, Δkx Δky) = √(2·S(kx,ky)·Δkx·Δky)                    (46)
a(kx, ky, Δkx Δky) = ν_(kx,ky) · √(2·S(kx,ky)·Δkx·Δky)        (47)
```
onde `ν` é uma variável normal de média 0 e desvio 1.

O SmileEngine faz (`OceanFFT.cpp:82-83`):
```cpp
const f32 h0x = sqrtP * FrandGaussian() * recipSqrt2;
const f32 h0y = sqrtP * FrandGaussian() * recipSqrt2;
```
— sem nenhum fator `Δk`. Com `Δk = 2π/L` e `WorldSize = 1`, o fator omitido vale `(2π)² ≈ 39.5`, absorvido em `Amplitude`.

Consequência prática, e é a mais irritante: **a amplitude das ondas não escala corretamente com a resolução nem com o tamanho do patch.** Mudar `kGridSize` de 256 para 512, ou mexer no tamanho do tile, muda a altura do mar — e obriga a re-tunar `MaxWaveSize`, `ChoppyWaveScale` e os boosts. É a mesma raiz do achado #1.

### Achado NOVO #21 — semeadura sequencial em vez de hash por número de onda

Horvath, §7.1.1 *Random Seeding*:

> *"It is common for artists to want to raise and lower the resolution of a height field and get the same ocean appearance up to the limits of the resolution. In order to create repeatability, we create pseudorandom numbers by seeding a random number generator with a hash of the wave number truncated to five digits of precision, to prevent numerical imprecision from changing the seeding."*

O SmileEngine faz `Rng.seed(Seed)` uma vez (`OceanFFT.cpp:57`) e depois consome o stream sequencialmente varrendo a grade. Determinístico ✔, mas **amarrado à ordem de varredura**: mudar `kGridSize` reordena tudo e produz um mar completamente diferente. Hash por `k` resolve — e é pré-requisito para mover o bake de H0 para compute shader (ver seção de performance).

### VALIDA uma escolha do código — `h₀(+k)` e `h₀(−k)` independentes

Este é o ponto mais interessante do paper para esta base. Horvath, §7.1.2:

> *"[Tessendorf 2001] uses a single complex variate, and then uses the complex conjugate of that variate to produce waves towards and against the wave direction when propagating. **This is problematic**, because the waves corresponding to the negative direction, represented by wave number −k = (−kx, −ky) have a different spectrum value associated with them, and need to be handled separately. We therefore store a separate complex variate for the positive and negative waves in two separate 2D complex spectral arrays in the initial state."*

O SmileEngine faz exatamente isso, ainda que num único array de 257×257: cada texel recebe seu próprio sorteio gaussiano com o `sqrt(P(k))` daquele `k`, e `OceanUpdateSpectrum.cs.hlsl:11-17` lê `h₀` em `loc1` e em `loc2 = 256 − loc1` como **duas variáveis independentes**, não como conjugado.

E isso **importa aqui**, porque o espectro é assimétrico: o fator `0.25` a favor/contra o vento (`OceanFFT.cpp:44`) faz `P(k) ≠ P(−k)`. Sob a formulação canônica do Tessendorf, essa assimetria seria perdida.

Verifiquei que a simetria Hermitiana continua valendo apesar disso: com `H[n] = g(n)e^{iωt} + conj(g(N−n))e^{−iωt}`, tem-se `conj(H[n]) = H[N−n]` para todo `n ∈ [1,255]` ✔. A saída da IFFT é real.

> **Correção de um achado preliminar meu:** eu havia marcado `loc2 = N − loc1` como leitura fora dos limites. Está errado — `H0Tex` é **257×257** (`OceanFFT.h:54`: `M = N + 1`), então `loc2 = 256` está dentro. Não é bug.

### O caminho de upgrade do espectro: PM → JONSWAP → TMA

Se o objetivo for tirar as constantes mágicas, esta é a escada, com as fórmulas exatas do paper.

**Pierson-Moskowitz** (eq. 18) — mar totalmente desenvolvido, sem fetch:
```
S_PM(ω) = (α·g²/ω⁵)·exp(−β·(ω₀/ω)⁴)
  α  = 8.1e−3
  β  = 0.74
  ω₀ = g/(1.026·U)
  ω_p = 0.855·g/U                                             (19)
```

**JONSWAP** (eq. 28) — adiciona *fetch*, pico bem mais pronunciado:
```
S_JONSWAP(ω) = (α·g²/ω⁵)·exp(−(5/4)(ω_p/ω)⁴)·γ^r
  r   = exp(−(ω−ω_p)² / (2σ²ω_p²))
  α   = 0.076·(U²/(F·g))^0.22
  ω_p = 22·(g²/(U·F))
  γ   = 3.3
  σ   = 0.07 se ω ≤ ω_p ; 0.09 se ω > ω_p
  F   = fetch (m)
```
> *"the primary wavelengths are strongly visible, with other wavelengths relatively damped out. This produces a much better visual match to photographs of ocean states."*

**TMA** (eq. 30) — JONSWAP × atenuação por profundidade de Kitaigorodskii:
```
S_TMA(ω) = S_JONSWAP(ω) · Φ(ω, h)
```
com a aproximação de Thompson & Vincent (erro < 4% no domínio útil), `ω_h = ω√(h/g)`:
```
Φ ≈ ½·ω_h²                se ω_h ≤ 1
Φ ≈ 1 − ½·(2 − ω_h)²      se ω_h > 1
```
Horvath observa que um smoothstep de 0 a 2.2 ou um `tanh` esticado também servem.

Ganho concreto para o SmileEngine: `fetch` e `depth` viram parâmetros de verdade, e é **exatamente o que resolve o problema documentado no `Water.cpp:843`** — o TMA amortece o swell por profundidade em vez de exigir os boosts `2.2/4.5` na mão.

⚠️ **A armadilha**, avisada pelo próprio autor (§8): converter `S(ω,θ)` para `S(kx,ky)` exige o determinante Jacobiano da mudança de variáveis (eq. 61):
```
S(kx, ky) = S(ω, θ) · (dω/dk) / k
```
> *"The last piece we were able to get correct was the change of variables determinant of the Jacobian, which seems obvious when formally written, but took a long time to understand and apply correctly."*

O Phillips atual já é um espectro em espaço-k e não precisa disso — mas qualquer migração para TMA precisa, e é onde a conversão costuma quebrar.

### Espalhamento direcional: o `cos²θ` atual é o mais cru da literatura

O SmileEngine usa `(k̂·ŵ)²` com supressão `0.25` contra o vento (`OceanFFT.cpp:41-44`). Horvath chama isso de *Positive Cosine Squared* (eq. 32) e é explícito:

> *"It does not match empirical data, and does not always look right aesthetically, which motivates a need for more robust, empirical models."*

Note que a forma canônica (eq. 32) **trunca** — zero para ondas contra o vento — enquanto o SmileEngine usa `0.25`, uma versão suavizada. Também: `D_cos²` não depende de `ω`, então alonga todos os comprimentos de onda igualmente, o que é fisicamente errado.

As alternativas empíricas, todas com fórmula fechada no paper:

| Modelo | Eq. | Forma |
|---|---|---|
| Mitsuyasu | 33-36 | `D = Q(s)·\|cos(θ/2)\|^{2s}`, `Q(s) = (2^{2s−1}/π)·Γ(s+1)²/Γ(2s+1)`, `s_p = 11.5(ω_p U/g)^{−2.5}` |
| Hasselmann | 37 | mesma forma, `s = 6.97(ω/ω_p)^{4.06}` (ω≤ω_p) / `9.77(ω/ω_p)^{−2.33−1.45((Uω_p/g)−1.17)}` (ω>ω_p) |
| **Donelan-Banner** | 38 | `D = (β_s / (2·tanh(β_s·π)))·sech²(β_s·θ)` |
| Flat | 39 | `1/(2π)` |
| Misturado | 40 | `(1−τ)·D_A + τ·D_B` |

Para Donelan-Banner, `β_s = 2.61(ω/ω_p)^{1.3}` para `0.56 < ω/ω_p < 0.95`; `2.28(ω/ω_p)^{−1.3}` para `0.95 ≤ ω/ω_p < 1.6`; `10^ε` acima, com `ε = −0.4 + 0.8393·exp(−0.567·ln((ω/ω_p)²))`. Horvath nota que na implementação dele usa o primeiro caso abaixo de 0.95, porque truncar em 0.56 "não fica agradável".

### O parâmetro *swell* — o que falta pra ter mar de tempestade distante

Horvath introduz (eq. 44-45) um parâmetro `ξ` normalizado em [0,1] que alonga as ondas em trens paralelos, crescendo com o comprimento de onda:
```
D_ξ(ω,θ) = Q_ξ(s_ξ)·|cos(θ/2)|^{2s_ξ}
s_ξ      = 16·tanh(ω_p/ω)·ξ²
D_final  = Q_final(ω)·D_base(ω,θ)·D_ξ(ω,θ)
```
O `16` é declaradamente o único número mágico do sistema dele. Se `D_base` for Mitsuyasu ou Hasselmann, o produto continua sendo Longuet-Higgins com os parâmetros de forma somados, então a normalização fechada da eq. 34 continua valendo — senão precisa de integração numérica (ou LUT 2D).

Isso é o que hoje o SmileEngine emula empiricamente com as três cascatas de bandas fixas.

### Configuração recomendada pelo autor

Horvath, §9:
> *"We default the system to always use the Capillary dispersion relationship, the TMA non-directional spectrum, and the Donelan-Banner directional spreading function."*

A dispersão capilar (eq. 9), que cobre profundidade finita e tensão superficial num só lugar:
```
ω² = (g·k + (σ/ρ)·k³)·tanh(k·h)
  σ = 0.074 N/m     (tensão superficial)
  ρ = 1000 kg/m³
  g = 9.81
```
O SmileEngine usa `ω = √(g·k)` puro (`OceanFFT.cpp:85`) — sem `tanh(kh)`, sem termo capilar. Para mar aberto é defensável, mas as menores ondas da cascata 0 (λ ≈ 0.5 m) já estão na faixa onde a tensão superficial começa a contar, e água rasa é impossível.

---

## O que está CORRETO (verificado símbolo a símbolo)

1. **`h(k,t)`** — `OceanUpdateSpectrum.cs.hlsl:23-24`. Expandindo `h₀(k)e^{iωt} + conj(h₀(−k))e^{−iωt}`: `Re = cos(a+c) − sin(b+d)`, `Im = cos(b−d) + sin(a−c)`. Bate exatamente.
2. **Simetria Hermitiana** — emerge da fórmula de evolução, não do bake. Verificado para todo `n ∈ [1,255]`. E o tratamento de `h₀(±k)` é o que o Horvath recomenda (ver acima).
3. **Empacotamento 2-em-1 de Dx/Dz** — `:31-35`. `D = Dx + i·Dz` num único FFT complexo, válido porque ambos são individualmente Hermitianos.
4. **Borboleta DIT radix-2** — `OceanFFT.cs.hlsl:21-38`. Conferida estágio a estágio: thread inferior recebe `W_m^{+j}`, superior recebe `W^{k+N/2} = −W^k` → `a − W·b`. Bit-reversal em forma scatter, correta por ser involução. 8 estágios = log₂256.
5. **Barreiras** — leituras sempre em `PingPong[src]`, escritas em `[1−src]`, barreira no fim de cada iteração. Sem race.
6. **Shift `(−1)^(x+y)` aplicado exatamente uma vez**, na saída, aos dois campos.
7. **Bandas espectrais das cascatas — genuinamente disjuntas.** Bandas (2,129), (2,12), (2,8) ciclos/tile sobre tiles T, 6T, 24T dão, em número de onda de mundo: `[2,129)/T`, `[0.333,2)/T`, `[0.083,0.333)/T`. **Contíguas, sem sobreposição e sem buraco** (8/24 = 2/6 = 0.333; 12/6 = 2). Sem dupla contagem de energia — o erro mais comum em multi-cascata foi evitado de propósito.
8. **Escalas de tempo por cascata** — 1 / 0.4082 / 0.2041 = 1/√1, 1/√6, 1/√24, exatamente a razão de dispersão de água profunda.
9. **Barreiras e ordem de dispatch da cadeia inteira** — todo par produtor→consumidor tem transição, `FFTTemp` reusado corretamente entre as cadeias H e D, mips por subrecurso corretos.
10. **Layout do constant buffer** — 28 membros conferidos campo a campo contra o HLSL. 640 B naturais, `alignas(256)` → 768. **Sem bug de padding.**
11. **Teste de frustum** — `C.z < 0 || C.z > C.w` é exatamente o volume de clip do D3D, válido nas duas convenções de Z; só falsos positivos.
12. **O bound de culling considera o deslocamento vertical** (`Pad` de 128 m) — tiles não popam no horizonte.
13. **Costura de T-junction** — 81 subsets stitchados por densidade; grau de decimação `2^(1+Δd)` confere.
14. **Beer-Lambert + reconstrução do comprimento de caminho** — conversão de view-z para comprimento ao longo do raio correta, extinção por canal.
15. **Guarda de refraction bleeding** — o `SceneDepthCopy` é anterior ao desenho da água, então o fallback `screenUV` é garantidamente atrás dela.
16. **Filtragem Toksvig + variância de Karis** — cadeia de 9 mips com `SampleGrad` e aniso 16×. A parte mais bem resolvida do shading.
17. **Espuma com histerese temporal** — mínimo relaxado do Jacobiano, com guarda de primeiro frame e `dt` clampado. Modelo certo.
18. **`Resize` vazio está correto** — `FWaterRenderer` não possui nenhum recurso dimensionado por tela; `ScreenParams` é reescrito todo frame.
19. **Ausência de normalização `1/N²` na IFFT está correta** — a síntese do Tessendorf *é* uma IDFT não-normalizada.

---

## O que simplesmente não existe

| Ausência | Evidência |
|---|---|
| **Flutuação / física** | Grep por `buoyan\|empuxo\|GetWaterHeight\|WaterHeightAt` → **zero**. O displacement nunca sai da GPU. Gameplay só enxerga `GetWaterLevel()` — o plano liso. Qualquer objeto boiaria num espelho enquanto a tela mostra 2 m de swell. |
| **Reflexão de cena** | Sem SSR, sem planar. Só cubemap pré-filtrado ou céu analítico hard-coded que nem combina com o céu renderizado. |
| **Câmera submersa** | Nenhuma. Backface rasteriza (`CULL_MODE_NONE`), mas o PS não tem branch de `dot(N,V)<0`: de baixo, `F → 0.95` e a superfície vira espelho do **céu**, em vez de reflexão interna total. |
| **Acoplamento com terreno/costa** | Grep em `Shaders/Terrain/` e `Terrain.h` por `water\|shore` → **zero**. Sem atenuação em água rasa, sem refração de costa. As ondas atravessam o terreno na linha d'água. |
| **Espuma de costa** | API pública completa (`SetShoreFoamWidth/Intensity`), escrita no CB, e **nenhum shader lê**. Sliders no-op. |
| **Testes** | Zero no repositório inteiro. |
| **Documentação** | `Docs/ARCHITECTURE.md` não tem seção de oceano; o diagrama de shaders omite `Water/`. |
| **UI de tuning** | 10 `EDebugMode` e ~50 setters em C++, **nenhum com UI**. O Editor expõe um toggle liga/desliga. Tunar o mar exige recompilar. |

Parâmetros plumbados que nenhum shader lê (verificado por grep): `OceanParams0` inteiro, `InvViewProj` (64 B por frame à toa), `SunColor.w` (WaterClarity), `WaterFXParams.w`, `DepthParams.w`, `BumpParams.x/.y`, `QuadTreeParams.w`, `AbsorptionColor.w`, `WaterAmbient.w`.

---

## Inventário de constantes mágicas

Todas privadas, sem setter, sem UI. Comparar com o *"no fudge parameters — just physical constants"* do Horvath.

| Constante | Valor | Local | Nota |
|---|---|---|---|
| `MaxWaveSize` (HeightScale) | 200 | `OceanFFT.h:76` | escala pós-FFT |
| `ChoppyWaveScale` | 400 | `OceanFFT.h:77` | 2× a de altura — viés de steepness não documentado |
| `NormalUp` | 8 | `OceanFFT.h:78` | substituto de `2·Δx_world / HeightScale`, que difere 24× entre cascatas |
| `kChoppyJacobianCalib` | `0.15/(1.5·1.0·0.75·1.5)` | `OceanFFT.h:79` | calibrado para uma combinação específica de sliders |
| `0.125` no `SetTime` | — | `OceanFFT.h:22` | cancela `√64`; só vale para tile de 64 m |
| `0.25` contra o vento | — | `OceanFFT.cpp:44` | Tessendorf/DXSDK usam 0.07 |
| `0.06` no displacement da VS | — | `WaterSurface.vs.hlsl:44` | |
| boosts `2.2 / 4.5` | — | `Water.cpp:846` | compensação do corte de vento inerte |
| pesos de espuma `0.6 / 0.35` | — | `WaterSurface.ps.hlsl:220-221` | fisicamente seriam 0.37 / 0.19 |

---

## Performance

- Cascatas 1 e 2 rodam FFTs 256² completas para espectros **99,3% e 99,7% zerados** (discos de raio 12 e 8). 32²/64² seriam visualmente idênticas e 16-64× mais baratas.
- Twiddles recalculados com `sin`/`cos` por thread por estágio: ~6,3 M pares transcendentais/frame. Tabela de 256 entradas resolve.
- Prefix-sum de 243 iterações **numa única lane** (`WaterGenerateDraws.cs.hlsl:320-344`).
- `ExecuteIndirect` sempre emite os 243 comandos sem count buffer; tipicamente <20 têm instância.
- Vertex/index buffer dos tiles moram **permanentemente em heap UPLOAD** (~3 MB lidos pelo IA via PCIe todo frame). Invisíveis no `VramTracker`, que pula UPLOAD.
- Dois `CopyResource` de tela cheia por frame (~50 MB/frame em 4K) mesmo sem um pixel de água visível.
- `ComputeH0()` é bake síncrono de 257² na render thread: ~400 k gaussianas por frame enquanto qualquer slider de vento é arrastado. Mover para compute shader exige o hash por número de onda do achado #21.
- As 3 cascatas são gravadas em série, totalmente serializadas por barreiras, num workload que não enche uma GPU moderna.

---

## Referências

**Primária desta auditoria**
- Horvath, C. J. *Empirical Directional Wave Spectra for Computer Graphics.* DigiPro '15, pp. 29–39. DOI [10.1145/2791261.2791267](https://doi.org/10.1145/2791261.2791267). Implementação aberta do autor: `blackencino/EncinoWaves`; port HLSL/D3D11: `speps/GX-EncinoWaves`.

**Fundamentais (abertas)**
- Tessendorf, J. *Simulating Ocean Water.* SIGGRAPH Courses. [PDF](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)
- Tessendorf, J. *Normal Maps for Rendering Vast Ocean Scenes.* [PDF](https://jtessen.people.clemson.edu/reports/papers_files/normalmap_eg.pdf) — trata o achatamento de normal rumo ao horizonte.
- Dupuy, J. & Bruneton, E. *Real-time Animation and Rendering of Ocean Whitecaps.* [HAL](https://inria.hal.science/hal-00967078v1/document) — whitecap pré-filtrável; upgrade direto do achado #10.
- Darles et al. *A Survey of Ocean Simulation and Rendering Techniques in CG.* [arXiv:1109.6494](https://arxiv.org/pdf/1109.6494)
- *Arc Blanc: a real time ocean simulation framework* (2025). [arXiv:2503.03326](https://arxiv.org/pdf/2503.03326)
- Tcheblokov, T. *Ocean simulation and rendering in War Thunder.* NVIDIA CGDC 2015. [PDF](https://developer.download.nvidia.com/assets/gameworks/downloads/regular/events/cgdc15/CGDC2015_ocean_simulation_en.pdf)
- Ubisoft La Forge. *Making Waves in Ocean Surface Rendering using Tiling and Blending.* [Link](https://www.ubisoft.com/en-us/studio/laforge/news/5WHMK3tLGMGsqhxmWls1Jw/making-waves-in-ocean-surface-rendering-using-tiling-and-blending)
- Crest Ocean System — referência de design para flutuação e costa. [Shorelines](https://crest.readthedocs.io/en/stable/user/shallows-and-shorelines.html) · [Buoyancy](https://crest.readthedocs.io/en/stable/user/collision-shape-and-buoyancy-physics.html)
- Papadopoulos & Papaioannou. *Realistic Real-time Underwater Caustics and Godrays.* [PDF](https://graphics.cs.aueb.gr/graphics/docs/papers/GraphiCon09_PapadopoulosPapaioannou.pdf)

**Citadas pelo Horvath (fontes primárias dos espectros)**
- Pierson & Moskowitz 1964 — espectro PM
- Hasselmann et al. 1973 — JONSWAP e espalhamento de Hasselmann
- Kitaigorodskii et al. 1975 — função de atenuação por profundidade
- Bouws et al. 1985 — espectro TMA
- Hughes 1984 — estudo do TMA
- Donelan et al. 1985 — espalhamento Donelan-Banner
- Mitsuyasu 1975 / Longuet-Higgins et al. 1961 — espalhamento Mitsuyasu
- Thompson & Vincent 1983 — aproximação de Φ
- Ochi 1998, *Ocean Waves*, Cambridge University Press

**Repos de referência**
- `blackencino/EncinoWaves`, `speps/GX-EncinoWaves` — Horvath, C++/OpenGL e HLSL/D3D11
- `2Retr0/GodotOceanWaves` — cascatas + espuma
- `tessarakkt/godot4-oceanfft` — FFT **com buoyancy** via compute
- `Mozobo/Ocean-Simulation` — 3 cascatas, arranjo idêntico ao daqui
- `wave-harmonic/water-resources` — bibliografia agregada
