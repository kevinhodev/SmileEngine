# Ementa de curso — derivada do código da SmileEngine

> Escrito a partir da leitura do repositório em 2026-08-06. Cada módulo abaixo aponta para
> arquivos que **existem hoje**; nada aqui é conteúdo a ser inventado do zero. Onde o código
> ainda não está em estado de ensinar, está marcado com ⚠️.
>
> Documento em PT-BR; os títulos dos cursos e das aulas estão em inglês porque é nesse idioma
> que o produto deve ser publicado (mercado ~20x maior, pagamento em USD).

---

## 0. Premissa comercial

O conteúdo desta engine é **nicho estreito com alta disposição a pagar**: quem procura ReSTIR,
DDGI ou integração de DLSS-RR são talvez alguns milhares de pessoas no mundo, e boa parte delas
é paga para saber isso. Esse perfil pede **venda direta, preço cheio** (site próprio / Gumroad),
não marketplace com desconto agressivo.

Três produtos, em ordem de execução recomendada:

| # | Produto | Papel | Duração alvo | Preço alvo |
|---|---------|-------|--------------|-----------|
| **C** | *DX12 Foundations for a Modern Renderer* | funil gratuito (YouTube) | 4–6 h | grátis |
| **B** | *FFT Ocean from Scratch in DirectX 12* | produto de entrada, prova de que você entrega | 6–8 h | US$ 90–130 |
| **A** | *Real-Time Ray Traced Lighting in DirectX 12* | carro-chefe | 16–20 h | US$ 250–400 |

O B vem antes do A de propósito: é o único assunto do repositório que é **autocontido**
(não depende de entender o renderer inteiro), tem resultado visual imediato e já possui
documento de auditoria e testes de baseline. Serve como piloto barato para descobrir se você
gosta de produzir vídeo antes de investir 400 h no carro-chefe.

---

## Produto A — *Real-Time Ray Traced Lighting in DirectX 12*

**Público:** quem já escreve D3D12 e HLSL e quer sair de "sei fazer um deferred" para
"sei montar um pipeline de luz por ray tracing que roda". Pré-requisito explícito: rasterização,
BRDF e compute básico. **Não** é curso de iniciante.

**Diferencial:** não existe material didático em vídeo cobrindo ReSTIR DI + GI + ReGIR
integrados num renderer real. O que existe são papers, a amostra da NVIDIA e posts soltos.
Esse é o buraco.

### Módulo 1 — The frame we are building (≈1 h)

Tour do pipeline como mapa do curso, não como aula técnica. A ordem real dos 27 blocos de passes
está documentada e é **verificável em runtime** — os rótulos são os escopos do `FGpuProfiler`
que aparecem na janela de Stats do editor.

- Fonte: `Docs/ARCHITECTURE.md` §5.2 · `Engine/Source/Graphics/Renderer.cpp`
- Formatos de G-buffer, reverse-Z, render-res × display-res: §5.1 e §5.3
- Entregável da aula: o aluno roda a engine e reconhece cada escopo no profiler.

### Módulo 2 — DXR 1.1 inline, do zero ao primeiro hit (≈2 h)

- `FRaytracingScene`: BLAS por mesh, TLAS reconstruída quando `Scene::TransformsVersion()` muda
  — `Engine/Source/Graphics/RaytracingScene.cpp` (409 linhas)
- O **`InstanceGeo`**: snapshot por instância com índices bindless de VB/IB e texturas, lido por
  todos os passes de RT. É a peça que faz `RayQuery` virar algo utilizável.
- Shading no hit sem hit shaders: `Shaders/GI/RTGeometry.hlsli`, `HitShading.hlsli` (341 linhas),
  `RTAlphaTest.hlsli`
- Máscaras de instância (`RTMasks.h`) e política central de epsilon (`RayEpsilons.h`,
  `Shaders/RayEpsilons.hlsli`) — por que epsilon disperso pelo código é bug garantido.

### Módulo 3 — ReSTIR DI: reservoirs sobre luzes locais (≈3,5 h) ★

O módulo mais valioso do curso.

- Estrutura de reservoir e RIS: `Shaders/GI/ReSTIRReservoir.hlsli`,
  `Shaders/Lighting/ReSTIRDICommon.hlsli`
- Amostragem inicial + reuso temporal: `ReSTIRDIInitialTemporal.cs.hlsl` (249 linhas)
- Reuso espacial e o jacobiano: `ReSTIRDISpatial.cs.hlsl` (298 linhas)
- Amostragem de luz por tipo: `DILightSampling.hlsli` (222 linhas)
- C++: `Engine/Source/Graphics/ReSTIRDI.cpp` (427 linhas)

**Aulas de bug real** (o histórico do git é a fonte — isso é o que nenhum paper te conta):
- por que sorteio combinado entre pools de luz enviesa, e a correção por **orçamento de
  candidatas por pool** (commit `c0a6456`)
- por que mesh lights precisam sair do pool até a alias table existir (`bd9d297`)
- por que a contribuição de mesh light tem de entrar no reservoir em **medida de ângulo sólido**
  e não de área (`f783196`)

### Módulo 4 — Mesh lights e ReGIR (≈2 h)

- Extração de triângulos emissivos: `Shaders/Lighting/MeshLightExtract.cs.hlsl`,
  `MeshLightCommon.hlsli`, `Engine/Source/Graphics/MeshLights.cpp` (404 linhas)
- **Alias table por potência com readback diferido** (`72120b2`) — o padrão de "construir na GPU,
  consumir um frame depois" sem travar a fila.
- ReGIR como grid de reservoirs: `Shaders/GI/ReGIRBuild.cs.hlsl`, `ReGIRAverage.cs.hlsl`,
  `ReGIRSampling.hlsli`, `Engine/Source/Graphics/ReGIR.cpp`
- Ponto conceitual que quase todo mundo erra: o ReGIR **troca o loop de luzes dentro do hit
  secundário**, não substitui o DI de tela (§7.5).

### Módulo 5 — DDGI como radiance cache (≈2,5 h)

- Probes de irradiância + distância (Chebyshev): `Shaders/GI/DDGICommon.hlsli` (366 linhas)
- Trace / update / update de distância: `DDGITrace.cs.hlsl`, `DDGIUpdate.cs.hlsl`,
  `DDGIUpdateDist.cs.hlsl`
- Relocação de probe: `DDGIRelocate.cs.hlsl`
- **Invalidação espacial do atlas em vez de reset global** (`b090d60`) — a diferença entre GI que
  pisca ao editar a cena e GI que não pisca.
- Rodar na **fila de compute assíncrona** sobrepondo o bloco de raster: `ComputeQueue.cpp`, §5.2
- C++: `Engine/Source/Graphics/DDGI.cpp` (608 linhas)
- Bônus de ferramenta: a suíte de debug (`DDGIDebugProbes`, `DDGIDebugRays`, `DDGIDebugVolume`,
  `DDGIDebugStats`) — como visualizar GI é metade de como depurar GI.

### Módulo 6 — ReSTIR GI e reflexos (≈2,5 h)

- Final gather difuso sobre o DDGI: `Shaders/GI/ReSTIRGITrace.cs.hlsl` (471 linhas — o maior
  shader do repo), `ReSTIRGISpatial.cs.hlsl`
- Specular estilo Lumen: trace → resolve → temporal → composite,
  `Shaders/Reflections/*` (`ReflectionTrace`, `ReflectionTraceMirror`, `ReflectionResolve`,
  `ReflectionTemporal`, `ReflectionSpatial`, `ReflectionComposite`), `GGXSample.hlsli`
- `Engine/Source/Graphics/Reflections.cpp` (925 linhas)

### Módulo 7 — Temporal confiável: o assunto que ninguém ensina (≈2 h) ★

Diferencial silencioso do curso. Todo mundo ensina a acumular; quase ninguém ensina **quando
descartar**.

- Motion vectors duais (RT Gems II cap. 25): `Shaders/Temporal/DualMotion.cs.hlsl`,
  `TemporalSurface.cs.hlsl`, `Docs/TEMPORAL_MOTION_VECTORS.md`
- Velocity de background (céu/nuvens/fog, que o G-buffer deixaria zerado):
  `BackgroundVelocity.cpp`
- **O grafo de invalidação de históricos** — §5.4, "o contrato mais fácil de quebrar". Ensinar
  isso como problema de *design de sistema*, incluindo o modo de falha documentado em
  `Docs/KNOBS-AUDIT.md` (58 knobs que chegam por reach-through sem passar pelo grafo, 4 que
  divergem do critério aplicado a knobs irmãos).

### Módulo 8 — Denoising e reconstrução (≈2 h)

Integração de SDK proprietário é dor real de produção e há pouquíssimo material bom.

- NRD RELAX com **duas instâncias independentes** (indireto e direto) para os históricos não se
  contaminarem: `NrdDenoiser.cpp` (563 linhas), passes de pack em
  `ReSTIRDINrdPack/NrdComposite`, `ReSTIRNrdPack`, `ReflectionNrdPack`
- `IUpscaler` como interface única: FSR 3.1, DLSS-SR, DLSS-RR — `FsrPass.cpp`, `DlssPass.cpp`,
  `DlssRRPass.cpp`, e os guides de material derivados do G-buffer em `DlssRRGuides.cpp`
- Máscaras reativas e de composição do FSR (água + translúcidos)
- Degradar para TAA quando nada está utilizável, em vez de escrever numa textura estagnada (§7.7)
- Padrão de build que vale por si: **SDK ausente vira stub e a engine continua compilando** (§2)

### Módulo 9 — Diagnóstico: como se depura isso (≈1,5 h)

- `DebugTargets`: registro global nome → slot SRV + como decodificar; o editor lista, filtra e
  compõe N alvos numa grade. `DebugTargets.cpp`, `DebugView.cpp`
- `VramTracker`: 10 categorias, desregistro automático via `ID3DDestructionNotifier`, e a
  diferença para o `CurrentUsage` do DXGI exposta como "não rastreado"
- `FGpuProfiler`, `FlickerHeatmap` (achar instabilidade temporal), `BvhDebugView`
  (GPU Zen 3, 7.3.3)

**Total estimado: ≈19 h.**

---

## Produto B — *FFT Ocean from Scratch in DirectX 12*

Autocontido, resultado visual imediato, e já auditado em `Docs/OCEAN-AUDIT.md`.

| Aula | Conteúdo | Arquivos |
|------|----------|----------|
| 1 | Espectro oceanográfico e **três cascatas com bandas disjuntas** (por que bandas sobrepostas geram batimento) | `OceanSpectrum.cpp`, `OceanUpdateSpectrum.cs.hlsl` |
| 2 | IFFT em compute, passo a passo | `OceanFFT.cs.hlsl`, `OceanFFTCommon.hlsli`, `OceanFFT.cpp` (529 linhas) |
| 3 | Displacement, gradientes, cadeias de mip | `OceanCreateDisplacement.cs.hlsl`, `OceanGradients.cs.hlsl`, `OceanDisplacementMip/NormalMip` |
| 4 | Clipmap **GPU-driven**: gerar os draws na GPU | `WaterGenerateDraws.cs.hlsl` (387 linhas) |
| 5 | Shading forward + momentos de slope anisotrópicos compartilhados entre normal, Sol e reflexão | `WaterSurface.vs/ps.hlsl` (409 linhas), `WaterCommon.hlsli` |
| 6 | Reflexão hierárquica: SSR de contato → DXR no miss/cobertura parcial → céu no miss final | `WaterReflectionTrace/Temporal/Composite`, `WaterSceneColorMip.cs.hlsl` |
| 7 | Seleção de LOD contínuo pelo **footprint GGX** em vez de refletir sempre o mip 0 | `WaterCommon.hlsli` |
| 8 | Testar matemática de render sem device D3D12 | `Tests/OceanMathBaselineTests.cpp` |

A aula 8 é um argumento de venda por si só: quase nenhum curso de graphics mostra como testar
qualquer coisa.

---

## Produto C — *DX12 Foundations* (funil gratuito, YouTube)

Assunto commodity — serve para ser achado, não para vender. 6–8 vídeos curtos:

- Três filas com papéis distintos (§4) — `CommandQueue.cpp`, `ComputeQueue.cpp`
- **Um único heap CBV/SRV/UAV compartilhado**, 16384 slots, com free-list — `DescriptorHeap.cpp`
- Barreiras num lugar só (`Barriers.h`) e fábrica de recursos (`GpuResources.h`)
- Reverse-Z e o flag espelhado C++/HLSL que **tem de virar junto** (`DepthConfig.h` ↔
  `DepthConfig.hlsli`)
- Hot-reload de shader e grafo de `#include` calculado no configure (§2)
- Pipeline de asset: FBX via ufbx → `.smesh`, formato versionado (v7: transform fora do vértice,
  nome do nó, instancing) — `Tools/Cooker/main.cpp`

---

## ⚠️ O que falta no repositório para virar curso

Ordenado por quanto trava a produção:

1. **`Renderer.cpp` tem 4325 linhas e `RenderFrame()` sozinho ~2500** (§13). É impossível
   ensinar a partir de uma função de 2500 linhas. Não precisa refatorar a engine inteira —
   precisa de uma **branch de ensino** com os passes extraídos em funções nomeadas por módulo.
   Isso também melhora a engine, então não é trabalho jogado fora.
2. **Curso pede codebase progressiva**: uma tag por aula, para o aluno começar de um estado e
   chegar a outro. O histórico atual é limpo e por feature (raro e ótimo), mas não é linear por
   assunto. Escolher: reconstruir num repo-curso separado (mais trabalho, melhor didática) ou
   ensinar por leitura do código pronto (menos trabalho, menor conversão).
3. **Sem README e sem LICENSE.** O aluno vai receber código; a licença de uso precisa existir
   antes da primeira venda.
4. **Caminhos absolutos `D:/Engines/...`** nos defaults dos 4 SDKs e o DXC procurado em dois
   caminhos fixos com `NO_DEFAULT_PATH` (§13). Primeiro obstáculo de todo aluno; vira enxurrada
   de pedido de suporte. Corrigir antes de vender, não depois.
5. **Hot-reload cobre 67 de 130 shaders** e a tabela de `ReloadShaders` duplica as chamadas de
   `RecreateAllPSOs` (já divergiram). Se o curso ensina iteração rápida, isso precisa funcionar.
6. **Sem header compartilhado C++/HLSL**: 89 arquivos com `cbuffer` espelhados à mão. Ensinável
   como "faça o que eu digo, não o que eu fiz" — mas melhor resolver com `.hlsli` +
   `#ifdef __cplusplus`.
7. **Requisitos de hardware**: Windows x64 + DX12 + DXR, e NVIDIA na prática para DLSS/NRD.
   Precisa estar na página de venda em negrito, ou vira pedido de reembolso.

## Ordem sugerida

1. README com vídeo no topo + LICENSE + consertar os caminhos absolutos *(dias)*
2. 3–4 posts técnicos: mesh lights em ângulo sólido, invalidação espacial do atlas DDGI,
   orçamento por pool no ReSTIR DI, motion vectors duais *(semanas — e servem igualmente para
   ser contratado)*
3. Produto B como piloto *(1–2 meses)*
4. Produto A só se o B vender *(4–6 meses)*
