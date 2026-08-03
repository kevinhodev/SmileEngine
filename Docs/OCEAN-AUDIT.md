# Auditoria e contrato do oceano

Estado em 3 de agosto de 2026. Este documento registra a validação do oceano FFT,
as correções aplicadas e os limites que continuam deliberadamente fora do sistema.

## Veredito

O pipeline deixou de ser uma demonstração calibrada em unidades implícitas. O estado
atual tem espectro físico em metros e segundos, bandas adjacentes, deslocamento e
derivadas métricas, motion da fase da onda, integração temporal explícita, guides de
Ray Reconstruction correspondentes à superfície e clipmap com geomorph ativo.

A implementação ainda não é um sistema completo de gameplay aquático: flutuação,
ondas de costa, câmera submersa e reflexão da cena sobre a água continuam ausentes.
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
opacos/deferred/reflexões
    -> cópias de cor e profundidade para refração
    -> água forward (HDR + depth + velocity + guides + masks)
    -> transparências foreground
    -> nuvens/fog/chuva
    -> TAA ou upscaler/Ray Reconstruction
```

Cada cascata preserva o deslocamento anterior antes de gravar a fase atual. O primeiro
frame e toda mudança de espectro inicializam `previous = current`. A água escreve:

- motion `curUV - prevUV`, incluindo animação das ondas;
- reactive mask e transparency/composition mask;
- albedo aproximado, normal/roughness e shading model de água nos guides;
- `specHitDist = 0`, pois a reflexão forward não corresponde ao hit do fundo.

## Clipmap e antialiasing

O gerador GPU atribui geomorph suave nas coroas externas que possuem nível mais
grosso. O VS combina o peso com footprint em pixels e preserva vértices de borda.
O mesmo footprint em metros seleciona uma cadeia de 9 mips do deslocamento, Jacobiano
e histórico anterior, filtrando frequências dentro de cada banda antes de a geometria
deixar de representá-las; o descarte terminal da cascata continua suave. As normais
usam mips, `SampleGrad`, Toksvig e variância de Karis.

O A/B visual de 3 de agosto identificou dois contratos ainda agressivos: a grade fina
era 64/32 = 2 m e o filtro/morph entrava em uma única célula da borda. A grade fina
agora é 16/32 = 0,5 m, com raiz de 32,768 km no depth 11; morph e low-pass atravessam
respectivamente quatro e oito células. O lobo solar também passou a usar Fresnel e
normalização, e a reflexão recebe uma fração maior da normal filtrada.

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

Debug e Release passam via CTest. Todos os shaders e o `SmileEditor` completo compilam
nos dois modos. O executável Debug permaneceu responsivo no smoke test, com a camada
de debug habilitada e sem saída de erro.

## Limites conhecidos

1. Transparências atrás ou imersas na água não participam da refração; a composição
   atual prioriza transparências foreground corretas.
2. Overlays forward posteriores à água alteram HDR sem reescrever os guides do RR.
3. Nuvens, fog e chuva ainda não escrevem reactive/composition masks próprios.
4. O Jacobiano combinado no PS é aproximação de primeira ordem; espuma exata exigiria
   guardar os tensores diferenciais ou recalculá-los no PS.
5. O corte entre bandas ainda é uma janela rígida e pode produzir ringing discreto.
6. Não há reflexão da cena na água, SSR/planar, câmera submersa, batimetria,
   ondas/espuma de costa, buoyancy ou consulta de altura para gameplay.
7. A redução das cascatas longas foi adiada: 256 está fixo em groupshared, dispatch,
   mips e sampling. Variantes 128/256 devem mudar esse contrato coordenadamente e ser
   comparadas por captura e perfil antes de substituir o padrão.

## Referências

- Christopher Horvath, *Empirical Directional Wave Spectra for Computer Graphics*,
  DigiPro 2015, DOI `10.1145/2791261.2791267`.
- Jerry Tessendorf, *Simulating Ocean Water*.
- `blackencino/EncinoWaves`, implementação de referência do autor.
- `speps/GX-EncinoWaves`, porte HLSL/D3D11 para comparação de convenções.
