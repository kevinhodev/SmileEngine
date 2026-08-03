# Auditoria e contrato do oceano

Estado em 3 de agosto de 2026. Este documento registra a validação do oceano FFT,
as correções aplicadas e os limites que continuam deliberadamente fora do sistema.

## Veredito

O pipeline deixou de ser uma demonstração calibrada em unidades implícitas. O estado
atual tem espectro físico em metros e segundos, bandas adjacentes, deslocamento e
derivadas métricas, motion da fase da onda, integração temporal explícita, guides de
Ray Reconstruction correspondentes à superfície e clipmap com geomorph ativo.

A implementação ainda não é um sistema completo de gameplay aquático: flutuação,
ondas de costa e câmera submersa continuam ausentes. A superfície agora possui SSR de
contato sobre as cópias sem água, fallback DXR para a cena e céu no miss; reflexão
planar permanece uma alternativa futura.
Também permanecem compromissos de rendering descritos em “Limites conhecidos”.

## Escopo validado

- geração de `h0(k)` e evolução `h(k,t)`;
- simetria Hermitiana, FFT 2D e convenções de sinal;
- três cascatas espectrais e sua conversão para unidades de mundo;
- deslocamento horizontal, normal, Jacobiano e espuma temporal;
- clipmap, costura, culling, geomorph e draws indiretos;
- refração, ordem de transparências e cópias da cena;
- velocity, reactive/composition masks e guides do DLSS Ray Reconstruction;
- lifetime de uploads, estados D3D12, hot reload e configurações de build;
- controles do editor, builds Debug/Release, testes matemáticos e smoke test.

## Comparação com a auditoria anterior

| Achado anterior | Veredito | Estado atual |
|---|---|---|
| `WorldSize` nunca configurado | correto, mas era só o sintoma | contrato espectral refeito de ponta a ponta |
| sinal do choppiness invertido | correto pela álgebra | teste de modo único e sinal corrigido |
| geomorph sempre zero | correto | peso topológico e por footprint ativos |
| água apagava alpha foreground | correto | transparências foreground compostas depois da água |
| água entregava guides do fundo ao RR | correto | água escreve G-buffer de guides, velocity e spec-hit válidos |
| normal/J em texels | correto | diferenças centrais em metros por domínio |
| PS reamostrava em `x'` | correto | coordenada paramétrica pré-deslocamento preservada |
| upload de `H0` sem contrato de fence | correto | staging por frame e dirty update na gravação do frame |
| hot reload incompleto | correto | todos os shaders de água/FFT têm rota explícita |
| Phillips sem dissipação de alta frequência | correto, mas a correção isolada seria insuficiente | Phillips substituído por TMA/JONSWAP + spreading direcional |
| redução imediata das cascatas | prematura | 256² preservado até existirem especializações coerentes |

## Contrato espectral físico

O estado de mar usa o modelo apresentado por Christopher Horvath em *Empirical
Directional Wave Spectra for Computer Graphics*:

- dispersão de profundidade finita com termo capilar;
- espectro de frequência JONSWAP com correção TMA;
- distribuição direcional Donelan–Banner normalizada em `[-pi, pi]`;
- controle de swell do modelo;
- mudança de variável de frequência para espaço de vetores de onda.

Para cada célula espectral:

```text
DeltaK = 2*pi / L
P(kx,ky) = S(omega) * D(omega,theta) * (domega/dk) / k
E[|h0(k)|²] = P(kx,ky) * DeltaK² / 2
```

`WavesAmount` é ganho linear de altura, portanto entra quadraticamente na densidade.
O tempo fornecido à dispersão é o tempo decorrido em segundos; não existem fatores
temporais por cascata ou compensações dependentes do tamanho antigo do tile.

| Cascata | Domínio | Banda em ciclos/tile | Comprimentos de onda aproximados |
|---|---:|---:|---:|
| 0 | 64 m | `[2,129)` | 0,50–32 m |
| 1 | 384 m | `[2,12)` | 32–192 m |
| 2 | 1536 m | `[2,8)` | 192–768 m |

As fronteiras são adjacentes em frequência de mundo e não recebem boosts empíricos.

## Convenção de sinal e geometria

O armazenamento centrado usa `k = N/2 - index`, a FFT usa exponencial de sinal
positivo e a saída aplica `(-1)^(x+y)`. A altura não é negada na criação do mapa.
Com `lambda > 0`, o operador horizontal `-i*kHat*h` comprime o perfil na crista.
`Tests/OceanMathBaselineTests.cpp` contém um modo único que verifica explicitamente
que o Jacobiano é menor na crista do que na cava.

O mapa final armazena deslocamento já escalado em metros:

```text
Ocean.xyz = (Dx, Dz, h)
P(x,z) = (x + Dx, waterLevel + h, z + Dz)
```

Para texel de mundo `Delta = L/N`, as diferenças centrais usam `1/(2*Delta)`.
As tangentes e o Jacobiano horizontal são:

```text
Tx = (1 + dDx/dx, dh/dx, dDz/dx)
Tz = (dDx/dz, dh/dz, 1 + dDz/dz)
N  = normalize(cross(Tz, Tx))
J  = (1 + dDx/dx)(1 + dDz/dz) - dDx/dz*dDz/dx
```

O VS combina as derivadas das cascatas antes do produto vetorial. O PS recebe a
coordenada paramétrica anterior ao deslocamento horizontal; normal, espuma e debug
não reavaliam o campo em `x'`.

## Ordem do frame e contrato temporal

```text
opacos/deferred/reflexões gerais
    -> cópias de cor e profundidade para refração
    -> água forward (HDR + depth + velocity + guides + masks)
    -> SSR de contato -> cobertura restante DXR -> restante/miss céu
    -> histórico + composite especular exclusivos da água
    -> transparências foreground
    -> nuvens/fog/chuva
    -> TAA ou upscaler/Ray Reconstruction
```

Cada cascata preserva o deslocamento anterior antes de gravar a fase atual. O primeiro
frame e toda mudança de espectro inicializam `previous = current`. A água escreve:

- motion `curUV - prevUV`, incluindo animação das ondas;
- reactive mask e transparency/composition mask;
- albedo aproximado, normal/roughness e shading model de água nos guides;
- `specHitDist` real do raio de reflexão da água; miss do céu usa zero pelo contrato do RR.

Sem Ray Reconstruction, a reflexão usa ping-pong e reprojeção pela velocity da própria
onda. O histórico da água também guarda a distância do hit e rejeita troca céu/geometria
ou saltos de profundidade, evitando uma segunda imagem deslocada. Com RR, esse acumulador
é bypassado: o composite entrega radiância estocástica
crua e o histórico neural recebe motion, normal/roughness, reactive mask e hit distance.

## Clipmap e antialiasing

O gerador GPU atribui geomorph suave nas coroas externas que possuem nível mais
grosso. O VS combina o peso com footprint em pixels e preserva vértices de borda.
O mesmo footprint em metros seleciona uma cadeia de 9 mips do deslocamento, Jacobiano
e histórico anterior, filtrando frequências dentro de cada banda antes de a geometria
deixar de representá-las; o descarte terminal da cascata continua suave.

A cadeia de normal guarda momentos de slope `(E[s_w], E[s_c], E[s_w²], E[s_c²])`
nos eixos vento/transversal. A normal filtrada vem da média; a energia subpixel
`E[s²]-E[s]²` migra para os dois eixos do GGX usado pelo Sol, pelos reflexos e pelo
composite. Assim o detalhe que sai da geometria/normal não desaparece nem vira apenas
um blur isotrópico. A variância de Karis continua como termo isotrópico de footprint.

O A/B visual de 3 de agosto identificou dois contratos ainda agressivos: a grade fina
era 64/32 = 2 m e o filtro/morph entrava em uma única célula da borda. A grade fina
agora é 16/32 = 0,5 m, com raiz de 32,768 km no depth 11; morph e low-pass atravessam
respectivamente quatro e oito células. O lobo solar também passou a usar Fresnel e
normalização, e a reflexão recebe uma fração maior da normal filtrada.

## Comparação Unreal, Cry e Bruneton

A Unreal 5.8 desenha Single Layer Water no G-buffer, classifica seus tiles, executa
SSR ou Lumen depois que depth/normal/roughness já representam a água e compõe o
resultado sobre reflection captures e skylight antes das transparências comuns. Sol
e ambiente usam o mesmo GGX e os mesmos parâmetros de superfície.

O CryEngine clássico usa para o oceano uma câmera planar refletida com clip plane,
resolução de 1/4 a 1/2 e atualização amortizada. O RT projetado recebe distorção da
normal; water volumes também possuem um caminho SSR com fallback de probe/cubemap.
Sua ordem separa transparências abaixo e acima da água. O material `Watercfx` foi uma
fonte direta de vários controles do Smile, mas o antigo Phong de dois lóbulos não foi
reintroduzido.

O Smile adota a arquitetura pós-G-buffer da Unreal: o `SceneColor/Depth` sem água
resolve primeiro o contato on-screen; o TLAS DXR cobre misses e objetos fora da tela;
o cubemap atmosférico/HDRI pré-filtrado fecha o restante. Como no composite da Unreal,
o fallback usa pico off-specular e é misturado pela confiança do SSR, em vez de uma
troca binária. A reflexão tem alvo, motion, histórico, confiança e hit distance
exclusivos da água. Planar continua útil como fallback sem ray tracing ou para
qualidade determinística.

Bruneton, Neyret e Holzschuch determinam o contrato de antialiasing: frequências que
saem da geometria migram para a normal, e frequências que saem da normal migram para
a BRDF. A implementação de momentos de slope acima é a versão estacionária desse
contrato para as cascatas FFT do Smile.

## Lifetime, configuração e performance

- `H0` é NxN e usa um staging por frame em voo;
- setters apenas marcam o espectro dirty e resets invalidam os históricos;
- normalização direcional é cacheada por raio espectral;
- twiddles usam LUT e não recalculam seno/cosseno por thread/estágio;
- argumentos indiretos são compactados e usam count buffer real por frame;
- VB/IB estáticos do clipmap residem em heap `DEFAULT` após upload único;
- shaders têm saída por configuração e rotas explícitas de hot reload;
- o editor expõe vento, fetch, profundidade, swell, ganho, displacement e choppy.

## Validação automatizada

O alvo `SmileOceanMathBaselineTests` verifica FFT contra DFT, Hermitian/DC/Nyquist,
bandas, energia, dispersão, normalização direcional, momentos de `h0`, ganho, sinal
choppy, derivadas métricas, LUT de twiddles, low-pass dos mips e a guarda gaussiana.
O teste de AA também verifica que os momentos de slope preservam energia direcional,
geram variância apenas para detalhe não resolvido e não alargam um slope constante.

Debug e Release passam via CTest no marco espectral. Para a integração de reflexos,
todos os shaders, o teste e o `SmileEditor` completo compilam em Release num build
isolado. O A/B visual e uma captura com a camada D3D12 continuam obrigatórios antes
de considerar encerrado o tuning da resposta especular.

## Limites conhecidos

1. Transparências atrás ou imersas na água não participam da refração; a composição
   atual prioriza transparências foreground corretas.
2. Overlays forward posteriores à água alteram HDR sem reescrever os guides do RR.
3. Nuvens, fog e chuva ainda não escrevem reactive/composition masks próprios.
4. O Jacobiano combinado no PS é aproximação de primeira ordem; espuma exata exigiria
   guardar os tensores diferenciais ou recalculá-los no PS.
5. O corte entre bandas ainda é uma janela rígida e pode produzir ringing discreto.
6. O SSR de contato ainda marcha o depth full-resolution: o HZB atual guarda apenas o
   depth mais distante para occlusion culling, enquanto SSR precisa também do mais
   próximo. A cor SSR já usa uma pirâmide HDR completa da cena sem água; o mip contínuo
   vem do footprint do lobo GGX, da distância do hit e da projeção da câmera. Falta
   tornar o HZB min/max e falta o fallback planar do Cry. Também faltam câmera submersa,
   batimetria, ondas/espuma de costa, buoyancy e consulta de altura para gameplay.
7. A redução das cascatas longas foi adiada: 256 está fixo em groupshared, dispatch,
   mips e sampling. Variantes 128/256 devem mudar esse contrato coordenadamente e ser
   comparadas por captura e perfil antes de substituir o padrão.
8. O primeiro corte da reflexão da água é full-resolution e mantém radiância, motion e
   dois histories RGBA16F próprios. Tile classification, half-resolution adaptativo e
   compactação desses alvos dependem primeiro de captura de qualidade e perfil de VRAM/GPU.

## Referências

- Christopher Horvath, *Empirical Directional Wave Spectra for Computer Graphics*,
  DigiPro 2015, DOI `10.1145/2791261.2791267`.
- Jerry Tessendorf, *Simulating Ocean Water*.
- Eric Bruneton, Fabrice Neyret e Nicolas Holzschuch, *Real-time Realistic Ocean
  Lighting using Seamless Transitions from Geometry to BRDF*, Eurographics 2010.
- Unreal Engine, `SingleLayerWaterRendering` / `SingleLayerWaterComposite`.
- CryEngine, `CREWaterOcean` / `Watercfx` / `WaterReflectionsPass`.
- `blackencino/EncinoWaves`, implementação de referência do autor.
- `speps/GX-EncinoWaves`, porte HLSL/D3D11 para comparação de convenções.
