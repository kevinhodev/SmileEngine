# SmileEngine — Documentação de Arquitetura

> Referência rápida da engine para consulta e como base ao **portar recursos** de
> outras engines (Unreal, CryEngine, Flax, papers/SIGGRAPH, etc.) para a SmileEngine.
>
> **Versão:** 3.0.0 · **Stack:** C++20 · DirectX 12 (+ DXR 1.1 inline) · Qt 6 (Widgets + QML) · HLSL SM 6.0/6.6 (DXC)
> **Plataforma:** Windows x64 (MSVC 2022) · **Build:** CMake + Visual Studio 17 2022
>
> **Revisão técnica parcial: 2026-08-19.** Este documento é escrito a partir da leitura do
> código, não gerado — quando divergir dele, o código vence. Nesta revisão foram atualizados
> build, testes, passes, constant buffers, hot-reload e dívida estrutural. As seções §4–§10
> descrevem estado factual e envelhecem rápido; a §13 lista a dívida conhecida na data acima.
> O [índice da documentação](README.md) separa referências vivas, planos e auditorias.

---

## Índice

1. [Visão geral](#1-visão-geral)
2. [Topologia do projeto / build](#2-topologia-do-projeto--build)
3. [Mapa de módulos](#3-mapa-de-módulos)
4. [Camada de abstração DX12 (RHI)](#4-camada-de-abstração-dx12-rhi)
5. [O frame: render loop e frame graph](#5-o-frame-render-loop-e-frame-graph)
6. [Modelo de binding (root signatures & descriptors)](#6-modelo-de-binding-root-signatures--descriptors)
7. [Subsistemas de rendering](#7-subsistemas-de-rendering)
8. [Layouts de constant buffers](#8-layouts-de-constant-buffers)
9. [Pipeline de shaders](#9-pipeline-de-shaders)
10. [Arquitetura do Editor (Qt + QML)](#10-arquitetura-do-editor-qt--qml)
11. [Convenções de código](#11-convenções-de-código)
12. [Guia de portabilidade (Unreal/Cry/Flax → Smile)](#12-guia-de-portabilidade)
13. [Limitações atuais & dívida técnica](#13-limitações-atuais--dívida-técnica)
14. [Oceano FFT](#14-oceano-fft)
15. [Sombra do sol (CSM)](#15-sombra-do-sol-csm)

---

## 1. Visão geral

A SmileEngine é uma engine pessoal de aprendizado/render sobre **DirectX 12**, com
**deferred shading** + **ray tracing por DXR 1.1 inline (RayQuery)**. É dividida em
quatro artefatos:

| Artefato | Pasta | Namespace | Tipo | Descrição |
|----------|-------|-----------|------|-----------|
| **Engine** | `Engine/` | `Smile` | Biblioteca **estática** | RHI DX12 + subsistemas de rendering |
| **Editor** | `Editor/` | `SmileEditor` | Executável **Qt 6** | Host do viewport, painéis QML, tema dark |
| **Shaders** | `Shaders/` | — | Alvo de build (DXC) | 133 fontes HLSL, com 139 compilações/permutações registradas |
| **Cooker** | `Tools/Cooker/` | `Smile` | Executável CLI | FBX (ufbx) + texturas (dds/png/tga/jpg/bmp) → `.smesh`/`.sscene` |

Princípios de design observados no código:

- **Composição, não singletons.** Cada `ViewportWidget` possui um `RenderThread`, que cria e
  serializa o acesso ao seu próprio `Smile::Renderer`. O `Renderer` é dono (por valor) de
  todos os subsistemas (`FAtmosphere`, `FDDGI`, `FReSTIRGI`, …).
  *Exceções:* `VramTracker` e `DebugTargets` são registros **globais** por processo (§7.9).
- **Prefixos por tipo:** `F` para tipos "engine/value-like" (`FD3D12Device`, `FMaterial`,
  `FAtmosphere`), classes "sistema" sem prefixo (`Renderer`).
- **Convenção de subsistema** (de fato, não enforçada — ver §13):
  `Initialize(...)` → `SetupForResize(...)` → `UpdatePerFrame(...)` →
  `Record*/Execute(CommandList, SRVHeap)` → `Recreate*(...)` (hot-reload) → `InvalidateHistory()`.
- **Um único heap CBV/SRV/UAV compartilhado** (`FTextureSRVHeap`, **16384 slots**,
  shader-visible) usado por toda a engine, com **free-list** (`Allocate`/`Free`).
- **Bakes únicos no startup** para LUTs caras (IBL, atmosfera, ruído de nuvens), todos no
  mesmo padrão compute → barrier → estado de leitura.
- **Históricos temporais explícitos.** Quase todo subsistema acumula entre frames
  (DDGI, ReSTIR GI/DI, reflexões, NRD, TAA, fog, nuvens). Mudar qualquer parâmetro que
  entre no sinal acumulado **exige invalidar os históricos afetados** — ver §5.4.

```
  ┌──────────────────────────────────────┐     ┌────────────────────────────────────────┐
  │      Editor (Qt 6 · SmileEditor)     │     │         Engine (DX12 · Smile)          │
  │                                      │     │                                        │
  │            ┌─────────────┐           │     │            ┌──────────┐                │
  │            │ MainWindow  │           │     │   ┌───────►│ Renderer │◄──────┐        │
  │            └──────┬──────┘           │     │   │        └──────────┘       │        │
  │        ┌──────────┴──────────┐       │     │   │                           │        │
  │        ▼                     ▼       │     │   ▼                           ▼        │
  │  ┌──────────────┐   ┌──────────────┐ │     │ ┌──────────────────┐  ┌──────────────┐ │
  │  │ViewportWidget│   │ Bridges QML: │ │     │ │ RHI: Device ·    │  │ Subsistemas: │ │
  │  │ QWidget HWND │   │ Materials ·  │ │     │ │ CmdQueue ·       │  │ Atmosphere · │ │
  │  │ nativo       │   │ Lights ·     │ │     │ │ Upload/Compute · │  │ DDGI ·       │ │
  │  └──────┬───────┘   │ Outliner ·   │ │     │ │ SwapChain ·      │  │ ReSTIR ·     │ │
  │         │           │ TimeOfDay ·  │ │     │ │ SRVHeap · PSO    │  │ Water ·      │ │
  │         │ owns      │ Menu/Status  │ │     │ └────────┬─────────┘  │ Terrain · …  │ │
  │         │RenderThread└──────┬──────┘ │     │          ┊            └──────────────┘ │
  └─────────┼──────────────────┼─────────┘     └──────────┼─────────────────────────────┘
            │   RendererHandle │                          ┊ carrega em runtime
            └──────────────────┴────────► Renderer        ▼
                                             ┌───────────────────────────────┐
                                             │ Shaders .cso (compilados DXC) │
                                             └───────────────────────────────┘
```

---

## 2. Topologia do projeto / build

### Hierarquia CMake

```
CMakeLists.txt (raiz)                  ← project 3.0.0, C++20, x64, /W4, acha Qt6, CTest
├── cmake/CompileShaders.cmake         ← função smile_compile_shader() (acha dxc.exe)
├── Engine/CMakeLists.txt              ← add_library(SmileEngine STATIC ...) + SDKs externos
├── Shaders/CMakeLists.txt             ← add_custom_target(Shaders) (lista todos os .hlsl)
├── Editor/CMakeLists.txt              ← qt_add_executable(SmileEditor ...) + windeployqt
├── Tools/Cooker/CMakeLists.txt        ← executável do cooker de assets
└── Tests/CMakeLists.txt               ← 3 executáveis de teste (matemática + identidade de cena)
```

### Decisões de build relevantes

- **C++20**, `/W4 /permissive- /Zc:__cplusplus /MP /utf-8`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`,
  `UNICODE`. x64 forçado.
- **Qt** padrão em `C:/Qt/6.10.2/msvc2022_64` (sobrescrevível via `-DCMAKE_PREFIX_PATH`).
  Componentes: `Core Concurrent Gui Network Svg Widgets Qml Quick QuickWidgets QuickControls2`.
  AUTOMOC ligado — headers com `Q_OBJECT` **devem ser listados explicitamente** no
  `qt_add_executable` (estão em `Include/` separado de `Source/`).
- **Shaders** compilados em build time por **DXC** (`dxc.exe` do Windows SDK `10.0.22621.0`).
  Saída por configuração: `build/bin/$<CONFIG>/Shaders/<nome>.<perfil>.cso` — Debug e Release
  precisam de bytecode próprio (`-Zi/-Od` vs `-O3`), senão o timestamp de um bloqueia o outro.
  O caminho vai ao C++ via `SMILE_SHADER_DIR`.
- `VersionInfo.h` é **gerado** de `VersionInfo.h.in` via `configure_file`.

### SDKs externos (todos opcionais — ausentes viram stub, a engine continua compilando)

| SDK | Cache var (default) | Sem ele |
|-----|---------------------|---------|
| **FidelityFX / FSR 3.1** (ffx-api) | `SMILE_FSR_ROOT` (`D:/Engines/FidelityFX-SDK-v1.1.4`) | `FFsrPass` vira stub, FSR some do seletor |
| **Streamline / DLSS SR+RR** | `SMILE_SL_ROOT` (`D:/Engines/Streamline`) | `FDlssPass`/`FDlssRRPass` viram stub |
| **NRD** (NVIDIA Real-time Denoising) | `SMILE_NRD_ROOT` (`D:/Engines/NRD`) | `FNrdDenoiser` vira stub |
| **NVAPI** (timer de shader) | `SMILE_NVAPI_ROOT` (`D:/Engines/nvapi`) | permutações `*Timed` não são compiladas |

> ⚠️ Os defaults são caminhos absolutos da máquina de desenvolvimento — sobrescreva com
> `-DSMILE_NRD_ROOT=...` etc. O `Shaders/CMakeLists.txt` redeclara `SMILE_NRD_ROOT` e
> `SMILE_NVAPI_ROOT` com o mesmo default (`set(... CACHE)` não reescreve entrada existente),
> para não depender da ordem dos `add_subdirectory`.

### Como adicionar um shader novo

1. Crie o `.hlsl` em `Shaders/<Categoria>/`.
2. Registre em `Shaders/CMakeLists.txt`, **num lugar só**:
   `smile_compile_shader(Categoria/Nome.cs.hlsl cs_6_0 main SHADER_OUTPUTS)`.
   A lista do Visual Studio e as dependências de `#include` saem daí — não há segunda lista.
   Um `.hlsl` em disco que ninguém registrou vira `message(WARNING)` no configure.
3. No C++, carregue `Nome.cs_6_0.cso` (sufixo = perfil) a partir de `SMILE_SHADER_DIR`.
4. Se o shader pertence a um pipeline com hot-reload, declare o stem em `ShaderStems()` e a
   recriação em `OnRecreatePipelines()` no respectivo `FRenderPass`/`FPipelineOwner`. O
   `FPassRegistry` deriva o reload específico e o completo desse registro único.

### Dependências de `#include` dos shaders

O DXC 1.6 do Windows SDK 10.0.22621.0 **não tem `-MF`/`-MD`**, então não há depfile para o
CMake consumir via `DEPFILE`. Em vez do antigo "todo `.cso` depende de todos os `.hlsli`"
(um toque em qualquer header recompilava os 130), `_smile_shader_deps` varre a cadeia de
`#include` em tempo de configure e dá a cada shader só o que ele alcança de verdade.

- Aspas resolvem relativo ao diretório do arquivo que inclui (regra do DXC/Clang);
  o que não resolver dentro da árvore é externo (`NRD.hlsli`, via `-I`) e é ignorado.
- Varredura iterativa com conjunto de visitados — ciclo ou diamante não trava nem duplica.
- Todos os `.hlsl`/`.hlsli` entram em `CMAKE_CONFIGURE_DEPENDS`: adicionar ou remover um
  `#include` reconfigura e recalcula o grafo. **Custo:** ~1,2 s de reconfigure em cada
  edição de shader; em troca, tocar `AO/GTAOCommon.hlsli` recompila 3 shaders em vez de 135.

| Ação | Antes | Depois |
|---|---|---|
| tocar `AO/GTAOCommon.hlsli` | 135 shaders · 11,8 s | **3** shaders · 2,9 s |
| tocar `GI/DDGICommon.hlsli` (pior caso, 26 includers) | 135 shaders · 11,8 s | **30** shaders · 7,3 s |

---

## 3. Mapa de módulos

```
Engine/Include/Smile/
├── Core/
│   ├── Types.h          u8..u64, i8..i64, f32/f64, ComPtr<T> alias
│   ├── Logger.h         LogInfo/Warning/Error + SetLogSink (callback p/ o Editor)
│   ├── HResultCheck.h   macro SMILE_HR(...) → loga e lança em FAILED(hr)
│   └── VersionInfo.h.in template gerado (versão/data)
├── Math/                Vec2/3/4, Mat44 (row-major, LH), MathUtils, ToRad/ToDeg
├── Input/               CameraInput.h
├── Scene/
│   ├── Scene.h          FScene: listas planas de FRenderable/FLight + TransformsVersion
│   ├── Light.h          FLight (point/spot, Id estável, RTWeight)
│   └── CookedFormat.h   formato cozido binário (kCookedVersion) + sidecars .json
└── Graphics/
    ├── ── RHI / núcleo ──
    │   ├── D3D12Device       device + DXGI factory6 + adapter + tearing + suporte a RT
    │   ├── CommandQueue      fila DIRECT, 2 frames in-flight, fence ring, segmentação p/ async
    │   ├── UploadQueue       fila COPY dedicada (texturas/meshes sem stall)
    │   ├── ComputeQueue      fila COMPUTE assíncrona (DDGI sobreposto ao raster)
    │   ├── SwapChain         2 buffers, R8G8B8A8_UNORM, tearing opcional
    │   ├── DescriptorHeap    wrapper genérico (RTV/DSV, não-shader-visible)
    │   ├── TextureSRVHeap    heap CBV/SRV/UAV compartilhado (16384, shader-visible, free-list)
    │   ├── Barriers.h        FBarrierBatch — acumula transições, 1 ResourceBarrier no Flush
    │   ├── PipelineState     root signature principal + 10 PSOs (prepass/G-buffer/lighting/blend)
    │   ├── ComputePipeline   PSO de compute com layouts fixo e parametrizável
    │   ├── PipelineUtils     criação comum de root signatures e compute PSOs
    │   ├── DepthConfig.h     kReverseZ (true) + funções de comparação derivadas
    │   └── GpuProfiler       timestamps por escopo (FGpuScope), por fila
    ├── ── recursos / cena ──
    │   ├── Texture / CubeTexture / VolumeTexture · Mesh / GpuMesh
    │   ├── Material          FMaterial (8 slots de textura + MaterialConstants 256B)
    │   └── GBuffer           3 RTs do deferred + SRV do depth contíguo
    ├── ── ambiente / céu ──
    │   ├── HDREnvironment · Skybox · Atmosphere (Hillaire) · TimeOfDay
    │   ├── CloudNoise · VolumetricClouds
    │   ├── Fog · VolumetricFog (froxel) · SunShafts
    │   └── Weather · RainWetness
    ├── ── sombras ──
    │   ├── SunShadows        CSM 4 cascatas (§15)
    │   └── LocalShadows      atlas 2D de spots + cube array de points
    ├── ── ray tracing / GI ──
    │   ├── RaytracingScene   BLAS/TLAS + InstanceGeo (snapshot bindless) · RTMasks · RayEpsilons
    │   ├── DDGI · DDGIDebug  probes de irradiância (radiance cache)
    │   ├── ReSTIRGI          final-gather difuso por pixel sobre o DDGI
    │   ├── ReSTIRDI · ReGIR · MeshLights   direta local por reservoir
    │   ├── Reflections       specular GI estilo Lumen (trace/resolve/temporal/composite)
    │   └── NrdDenoiser       RELAX difuso+especular (2 instâncias: indireta e direta)
    ├── ── screen-space / temporal ──
    │   ├── AmbientOcclusion (GTAO) · HiZOcclusion (culling HZB)
    │   ├── TemporalAA · TemporalMotionVectors · BackgroundVelocity
    │   └── Upscaler.h (IUpscaler) → FsrPass · DlssPass · DlssRRPass · DlssRRGuides
    ├── ── água / terreno ──
    │   ├── OceanSpectrum · OceanFFT (3 cascatas) · Water (§14)
    │   └── Terrain          heightmap + CDLOD + proxy de RT
    ├── ── pós / editor ──
    │   ├── PostProcess      Bloom + ACES tonemap → swapchain
    │   ├── Picking · SelectionOutline · DebugDraw · MaterialPreview
    │   └── DebugTargets · DebugView · ShaderTimer · BvhDebugView · FlickerHeatmap · VramTracker
    └── Renderer             maestro: cria tudo, monta o frame, expõe setters p/ o Editor
```

---

## 4. Camada de abstração DX12 (RHI)

A "RHI" é deliberadamente fina — wrappers diretos sobre objetos DX12, sem indireção.

### `FD3D12Device`
Cria `ID3D12Device` + `IDXGIFactory6` + escolhe o `IDXGIAdapter1` (com descrição e VRAM).
Detecta **tearing** (vsync-off) e **suporte a ray tracing** (`RaytracingSupported()` — todo o
bloco de GI/reflexões fica atrás dessa flag). Debug layer ligada em `_DEBUG`.

### Filas: três, com papéis distintos

| Classe | Tipo D3D12 | Papel |
|--------|-----------|-------|
| `FCommandQueue` | DIRECT | o frame; **`kFramesInFlight = 2`**, um allocator por slot, fence ring |
| `FUploadQueue` | COPY | uploads de textura/mesh sem travar o frame |
| `FAsyncComputeQueue` | COMPUTE | DDGI sobreposto ao CSM/prepass/G-buffer |

`FCommandQueue` expõe `BeginFrame()` (espera a fence do slot) / `EndFrame(lists)`, além do
par legado `ResetForRecording()` / `ExecuteAndSync()` e do `Flush()` (usado antes de recriar
recursos). Para o async compute existe `SubmitSegmentAndContinue()`: como `Wait`/`Signal` de
fila só valem **entre** `ExecuteCommandLists`, o frame é fatiado em segmentos — fecha e
executa o gravado, sinaliza a fence, e reabre a **mesma** list no allocator do frame.
`GpuWait(fence, value)` faz a espera GPU-side do lado oposto.

### `FSwapChain`
2 buffers, formato **`R8G8B8A8_UNORM`** (o backbuffer final é LDR; o HDR vive em RTs
separados — ver §5). RTV heap próprio. `Present()` respeita tearing.

### `FDescriptorHeap` vs `FTextureSRVHeap`
- `FDescriptorHeap` — wrapper genérico para heaps **não shader-visible** (RTV, DSV), pequenos.
- `FTextureSRVHeap` — **o** heap CBV/SRV/UAV global, shader-visible, **`kCapacity = 16384`**,
  com heap de *staging* espelhado. `Allocate(count)` / `Free(slot, count)` operam sobre uma
  **free-list** de ranges; `Release(slot, count)` também invalida o slot do chamador.
  `CopyTable` monta tabelas contíguas a partir de slots dispersos do staging heap.

> Quem aloca é responsável por liberar no caminho de resize/shutdown. O padrão canônico é o
> par `ReleaseSizedResources(SRVHeap)` + `SetupForResize(...)` (ver `FAmbientOcclusion`,
> `FHiZOcclusion`).

### Barreiras — `Barriers.h`
- `TransitionResource(cmd, res, before, after)` — transição **única e imediata**.
- `FBarrierBatch` — acumula e emite tudo num **único** `ResourceBarrier` no `Flush`; use quando
  há 2+ transições no mesmo ponto. `TransitionTracked(res, curState, to)` lê e reescreve o
  estado rastreado pelo chamador. Contrato: `Flush` **antes** do primeiro draw/dispatch/copy
  que depende das transições. Construir com uma command list liga o **auto-flush** (modelo do
  `GPUContextDX12` do Flax): ao encher, o lote sai sozinho em vez de estourar assert — emitir
  barreira antes do previsto é sempre correto, o que não pode é emitir depois do consumidor.

> Nota de arquitetura: aqui o estado do recurso mora no **chamador** (cada passe tem seus
> `D3D12_RESOURCE_STATES XxxState`). Flax e Cry põem o estado **no recurso**
> (`SetResourceState(res, after)` consulta e decide sozinho), o que elimina a classe de bug
> "dois donos discordando sobre o estado atual". É a evolução natural daqui, mas exige um
> wrapper de recurso que a engine ainda não tem.

### Criação de recursos — `GpuResources.h`
Fábrica para a forma comum: `Tex2DDesc/Tex3DDesc/BufferDesc` (descritores),
`CreateTex2D/CreateTex3D/CreateBuffer` (DEFAULT heap), `CreateUploadBuffer` (upload mapeado
de forma persistente, com `FUploadBuffer::Slice(i)`/`Address(i)` por frame em voo),
`CreateReadbackBuffer`, e os descritores de view (`SrvTex2D`, `UavTex2D`, `RtvTex2D`,
`SrvStructuredBuffer`, `UavStructuredBuffer`).

O **registro no `VramTracker` é parte da criação** em DEFAULT heap — o chamador escolhe a
`EVramCategory` e pronto. Antes era um passo separado que cada autor precisava lembrar, e
esquecer sumia com o recurso do breakdown de VRAM do editor sem nenhum sintoma.

Em **Resource Heap Tier 2**, o funil subaloca DEFAULT heap com D3D12MA 3.2.0 e cria placed
resources; a `Allocation` é liberada pelo `ID3DDestructionNotifier` do recurso. Tier 1,
RT/DS ainda sem inicialização explícita, falha do allocator e `SMILE_DISABLE_D3D12MA=1` usam
`CreateCommittedResource` como fallback. UPLOAD e READBACK continuam committed. Os logs de
criação separam `DEFAULT/D3D12MA` de
`DEFAULT/committed` e incluem bytes reservados/ocupados pelo allocator para o A/B de custo e
residência não depender de estimativa.

Não cobre caminhos com política própria (recursos reservados ou aliasing explícito entre
recursos): monte o desc e gerencie o lifetime nesses casos. O objetivo é centralizar a forma
comum, não virar uma camada completa sobre o D3D12.

### Pipeline de compute: dois layouts

`FComputePipeline` oferece dois overloads de `Initialize`:

| Overload | Forma do root sig | Usado por |
|----------|-------------------|-----------|
| `Initialize(device, cso, sourceIsCube)` | 8 root constants em `b0` + `t0` + `u0` + `s0` linear-clamp | passes de IBL, mip de scene color |
| `Initialize(device, cso, numSRVs, numUAVs, heapDirectlyIndexed, nvApiExtnSlot)` | `b0` CBV + tabela SRV `t0..t(N-1)` + tabela UAV `u0..u(M-1)` + `s0` linear-clamp + `s1` linear-wrap | atmosfera, nuvens, AO, HZB, ReSTIR, reflexões, … |

As *dimensões* das views (2D/3D/cube) ficam no shader, então a mesma root sig serve a tudo.
Subsistemas que não cabem nesses layouts (como NRD e partes da água) montam a root signature na mão.

---

## 5. O frame: render loop e frame graph

O loop é dirigido pelo Editor, mas executado fora da GUI. O `QTimer` do `ViewportWidget`
prepara input/câmera e solicita um frame; `RenderThread` executa `Renderer::RenderFrame()` e
`PresentFrame()` na sua thread dedicada (o `Present` é separado para o dono soltar o lock
compartilhado antes de entrar no DXGI). A conclusão volta por callback enfileirado para a
thread Qt, que consome readbacks, publica telemetria e emite `FrameReady`.

### 5.1 Resolução: render-res × display-res

A cena renderiza em `RenderWidth/Height` = swapchain × `RenderScale`; o backbuffer permanece
nativo. `RenderScale` vem de três origens mutuamente exclusivas:

- **SSAA manual** (`SetRenderScale`, >1.0 = supersampling);
- **razão do upscaler** (`ApplyUpscalerScale`) quando FSR ou DLSS-SR está ativo;
- **modo de qualidade do DLSS-RR**, que **ignora** escala arbitrária (o RR não suporta DRS —
  a feature NGX nasce na resolução ótima do modo).

Eixos independentes: `EUpscaler { None, FSR, DLSS }` e `EDenoiser { None, NRD, DLSS_RR }`.
Selecionar `DLSS_RR` **força** o upscaler para DLSS (o RR faz denoise + upscale num eval só).

### 5.2 `RenderFrame()` — ordem real dos passes

> `Renderer::RenderFrame()` continua sendo a função explícita de orquestração, hoje com cerca
> de **370 linhas**. Ciclo de vida, resize, invalidação e hot-reload dos passes migrados são
> centralizados por `FRenderPass` e `FPassRegistry`; a ordem de execução permanece legível aqui.
> Os rótulos abaixo são os escopos do `FGpuProfiler` — os mesmos que aparecem na janela de
> Stats do editor, o que torna a lista verificável em runtime.

```
 [pré-frame, fora da gravação]
   coalescência de dirty flags (material RT / luz indireta) → invalidações
   BeginFrame (espera fence do slot) · readbacks do slot anterior · FrameConstants
   jitter (upscaler/TAA) · Time-of-Day · clima · UpdatePerFrame dos subsistemas
      │
 ─────┼─── FILA COMPUTE (async, opcional) ───────────────────────────────
      │      DDGI (async)  ── sobrepõe o bloco de raster abaixo
 ─────┼──────────────────────────────────────────────────────────────────
      ▼
  1. Água — FFT                     (3 cascatas: espectro → IFFT → gradientes)
  2. Céu e atmosfera                (bake sky-view/ambiente/reflexão · RenderSky · estrelas
                                     · bake de aerial perspective)
  3. Sombra das nuvens
  4. MeshLights (extract) · ReGIR (build)
  5. DDGI                           (se não rodou na fila async)
  6. Sombras — sol (CSM) · Sombras — locais
  7. Z - opacos · Z - mascarados · Z - terreno       (depth prepass, opcional)
  8. HZB                            (build + teste de oclusão, readback em ring)
  9. GTAO
 10. G-buffer - meshes · G-buffer - terreno          (MRT: A/B/C + velocity + emissivo no HDR)
 11. Velocity do background         (preenche o velocity ZERO do céu/nuvens)
 12. Chuva — wetness                (molha o G-buffer in-place)
 13. Motion temporal confiável      (vetor dual + histórico de superfície)
 14. ReSTIR GI · Reflexos (trace) · Pack GI+reflexos · RELAX indireto · Saída NRD indireta
 15. ReSTIR DI · NRD direta         (pack → RELAX → composite)
 16. ▸ Deferred lighting             (fullscreen, aditivo sobre o emissivo já no HDR)
 17. Picking (ID pass, sob demanda)
 18. Reflexos (composite)
 19. Água — superfície              (+ cópias de scene color/depth e cadeia de mips antes)
 20. Translúcidos                   (forward blend)
 21. Nuvens
 22. Volumetric fog · Sun shafts · Fog (height + aerial perspective)
 23. [debug] preview de render targets · BVH · heatmap de flicker
 24. TAA  ─ou─  DLSS-RR guides + Dispatch do upscaler (FSR / DLSS-SR / DLSS-RR)
 25. Pós (bloom + tonemap ACES)      → backbuffer LDR
 26. Contorno da seleção · Debug draw (gizmo/ícones)
 27. EndFrame · Close · Execute      → PresentFrame() (chamado pelo dono da thread)
```

### 5.3 Render targets e formatos

| Buffer | Formato | Papel |
|--------|---------|-------|
| `GBufferA` | `R8G8B8A8_UNORM_SRGB` | `.rgb` BaseColor · `.a` AO (alpha fica linear em formato sRGB) |
| `GBufferB` | `R10G10B10A2_UNORM` | `.rg` OctNormal · `.b` Roughness · `.a` ShadingModelID (2 bits) |
| `GBufferC` | `R8G8B8A8_UNORM` | `.r` Metallic · `.gba` livres |
| `HDRColorBuffer` | `R16G16B16A16_FLOAT` | SceneColor HDR (o emissivo é escrito aqui direto pelo G-buffer) |
| `DepthBuffer` | `R32_TYPELESS` (DSV `D32_FLOAT` + SRV `R32_FLOAT`) | **Reverse-Z**; SRV no 4º slot contíguo ao G-buffer |
| `VelocityBuffer` | `R16G16_FLOAT` | motion vectors em UV, RT próprio |
| `UpscaleReactive/CompositionMask` | render-res | sinais do FSR 3.1 (água + translúcidos) |
| Backbuffer | `R8G8B8A8_UNORM` | saída final LDR após tonemap |

**Reverse-Z** (`kReverseZ = true` em `DepthConfig.h`): near→1, far→0, clear em 0.0, funções de
comparação `GREATER`/`GREATER_EQUAL`. **Só a câmera principal** — as CSM são ortográficas e
permanecem forward-Z. O flag espelha `SMILE_REVERSE_Z` em `Shaders/Common/DepthConfig.hlsli`
e os dois têm de virar juntos.

**Sem MSAA:** todo o pipeline é single-sample; o AA vem do upscaler (FSR/DLSS) ou do TAA.
`NearZ = 0.1`; `FarZ = 20000` com água ativa, `4000` sem ela. FOV vertical de 60°
(`Renderer::kFovYDegrees`, fonte única — o editor lê daqui para dimensionar gizmos).

### 5.4 Invalidação de históricos — o contrato mais fácil de quebrar

Quase todo subsistema acumula entre frames. Um parâmetro que entre no sinal **gravado**
(e não só no exibido) precisa derrubar tudo que acumulou sobre ele, senão o A/B compara um
estado misturado e o knob parece inerte. `FRenderSettings` concentra os pontos de entrada e
`HistoryDomain.h` representa como máscaras os históricos afetados:

| Ponto de entrada | Derruba |
|---|---|
| `SetRayEpsilons` | ReSTIR GI/DI, NRD (×2), RR, DDGI, reflexões, TAA |
| `OnGIHitSamplingChanged` | DDGI, ReSTIR GI, reflexões, NRD, fog volumétrico, RR, TAA + repete o diagnóstico pontual |
| `SetDenoiser` / `SetUpscaler` | NRD (×2), ReSTIR GI/DI, reflexões, RR, TAA |
| `NotifyMaterialRTStateChanged` | `Flush` da fila + `RefreshInstanceGeo` + flags da TLAS + todos os históricos |
| `NotifyIndirectLightingChanged` | DDGI, motion temporal, ReSTIR GI, reflexões, NRD, fog, RR, TAA |

Edições vindas da UI chegam **entre** frames e várias caem no mesmo frame (arrastar um slider
dispara o setter por tick). Por isso existem as versões **coalescidas** `MarkMaterialRTStateDirty()`
e `MarkIndirectLightingDirty()`: os setters só marcam, e o `RenderFrame` consome a flag uma vez,
antes do `BeginFrame`. **Use sempre as coalescidas nos setters do editor** — a versão direta
custa um `Flush` da fila por chamada.

> Regra prática: ao levar um parâmetro para dentro de `ShadeSurfaceHit` (ou de qualquer coisa
> que grave no reservoir/atlas), **revise a invalidação dele**. Já aconteceu de um knob nascer
> "de sampler puro" e deixar de ser em commit posterior sem o setter acompanhar.

### 5.5 Frame de coordenadas da atmosfera/nuvens
Atmosfera e nuvens vivem num **frame em km**, desacoplado das unidades da cena
(`kKmPerWorldUnit = 0.001`). A reconstrução de raio do mundo usa `InvViewProjNoTrans`
(view sem translação · proj)⁻¹.

---

## 6. Modelo de binding (root signatures & descriptors)

### Root signature principal (`FPipelineState`) — 14 parâmetros

Compartilhada pelo prepass, G-buffer, deferred lighting e forward blend. PSOs que não
referenciam um registro simplesmente não fazem o bind — parâmetro extra é inofensivo.

```
[0]  CBV   b0            FrameConstants                       (ALL)
[1]  CBV   b1            MaterialConstants                    (ALL)
[2]  Table SRV t0..t7    8 texturas do material               (PS)
[3]  Table SRV t8..t10   IBL: irradiance, specular, BRDF LUT  (PS)
[4]  CBV   b2            ObjectConstants                      (VS)
[5]  CBV   b3            CSMCB — matrizes/splits das cascatas (PS)
[6]  Table SRV t11       CSM do sol (Texture2DArray)          (PS)
[7]  Table SRV t12..t13 + t15   DDGI: atlas de irradiância, atlas de
                                distância e o buffer de dados das probes  (PS)
[8]  Table SRV t14       AO (GTAO)                            (PS)
[9]  Table SRV t16       ReSTIR GI (difusa por pixel)         (PS)
[10] SRV   t17           luzes puntuais — ROOT SRV, sem heap   (PS)
[11] Table SRV t18..t19  sombras locais: atlas 2D + cube array (PS)
[12] Table SRV t20       ReSTIR DI (direta local integral)    (PS)
[13] Table SRV t21       LUT de transmitância da atmosfera    (PS)

Static s0  ANISOTROPIC WRAP        (materiais, MaxAniso 16)
Static s1  LINEAR CLAMP            (cubemaps + LUTs)
Static s2  COMPARISON LINEAR, BORDER **branco**, LESS_EQUAL   (PCF das sombras)
```

> `s2` usa `BORDER` branco (= "iluminado") e **não** `CLAMP`: com clamp, um tap do PCF que sai
> do slice repete o texel da borda em vez de devolver "sem sombra". O passe de fog volumétrico
> já usava border branco nos mesmos mapas; isto alinha o deferred com ele.

### Slots de textura do material (`kMaterialTextureSlots = 8`)

| t | Slot | Default |
|---|------|---------|
| t0 | Albedo / BaseColor | White |
| t1 | Normal | FlatNormal |
| t2 | MetallicRoughness (combinado, estilo glTF) | ORM |
| t3 | AO | White |
| t4 | Emissive | Black |
| t5 | Height (Parallax Occlusion Mapping) | White (=1, sem paralaxe) |
| t6 | Metalness (separado, alternativa a t2) | White |
| t7 | Roughness (separado, alternativa a t2) | White |

Cada `FMaterial` aloca **8 slots contíguos** no SRV heap (`SRVTableStart`) + um CBV de 256B
(`MaterialConstants`, de um pool paginado). `Finalize()` preenche a tabela; `Bind()` seta o
root CBV b1 e a tabela t0..t7. Flags `HasXMap` no CB dizem ao shader quais usar.

`SRVTableStart` **não é privado do raster**: o snapshot `InstanceGeo` do ray tracing publica
`SRVTableStart + {0,2,4}` como índices bindless (albedo / metal-rough / emissivo) lidos via
`ResourceDescriptorHeap`. Por isso um re-`Finalize()` **reusa** o range em vez de realocar —
trocá-lo faria os raios amostrarem a textura de outro material.

O layout do `MaterialConstants` em HLSL vive num único lugar, `Shaders/MaterialCB.hlsli`,
incluído pelos shaders que consomem material (G-buffer, forward blend, os dois depth masked,
shadow depth e o preview). Campo novo no struct C++ = editar esse header, e só ele.

### Convenção de descriptors
Tabelas contíguas são montadas com `FTextureSRVHeap::CopyTable`, que copia slots dispersos do
**staging heap** para um bloco contíguo via `ID3D12Device::CopyDescriptors`. O G-buffer já nasce
contíguo: A, B, C e o depth ocupam 4 slots sequenciais, ligados como uma única tabela t0..t3
no deferred lighting.

---

## 7. Subsistemas de rendering

### 7.1 IBL / Ambiente HDR — `FHDREnvironment`
Pipeline GPU completo, gerado de um `.hdr` (Radiance RGBE via `stb_image`):

```
.hdr (equirect float) ──CS EquirectToCube──► EnvCube 1024², 11 mips
                                              │
        ┌─────────────────────────────────────┼──────────────────────────┐
   CS MipGen                            CS IrradianceConvolution     CS SpecularPrefilter
        ▼                                      ▼                          ▼
  cadeia de mips                      IrradianceCube 32²        SpecularCube 128², 7 mips
  (correção solid-angle Karis)        (difuso)                  (1 mip por roughness 0..1)

  CS BRDFIntegration ──► BRDF LUT 128² (R16G16_FLOAT)   [gerado 1x no Initialize]
```

Swap de HDR em runtime re-roda a cadeia. Antes de qualquer HDR carregado, um 1×1 preto neutro
mantém os SRVs válidos. `kSpecularMips-1` vai no `IBLParams.z` do FrameCB.

### 7.2 Céu — `FSkybox` · `FAtmosphere` · `FTimeOfDay`
`FAtmosphere` implementa Hillaire ("Scalable and Production Ready Sky and Atmosphere"):

| LUT | Quando | Conteúdo |
|-----|--------|----------|
| Transmittance | bake 1x no startup (re-bake se `Dirty`) | extinção ao longo do raio |
| Multi-Scatter | bake 1x no startup | espalhamento múltiplo |
| Sky-View | **bake por frame** | céu visto da câmera |
| Sky Ambient (SH-L1) | por frame | ambiente do céu direcional |
| Sky Reflection | por frame, só com água | cubemap barato p/ o miss da água |
| Aerial Perspective | por frame (volume froxel) | inscatter/extinção por distância |

A atmosfera tem precedência sobre o skybox HDR quando `UseAtmosphereSky` (default **on**);
o `FSkybox` desenha o `EnvCube` quando ela está desligada. `FTimeOfDay` dirige sol/lua
(posição sideral), estrelas e o catálogo estelar.

**Transmitância por pixel:** o sol/lua podem ser atenuados **por pixel** na altitude da
superfície (LUT em t21) em vez de uma cor única por frame calculada na altitude da câmera.
`AtmoLightParams.w` é o botão do A/B; `SunColorRaw`/`MoonColorRaw` carregam a cor sem
transmitância para os consumidores que usam esse caminho.

**Ambiente do céu em SH-L1:** três `Vec4` (um por canal) na base real l=0/l=1 substituem as
duas cores chapadas (`SkyAmbientColor`/`GroundAmbientColor`), que seguem preenchidas como
fallback e como botão de A/B (`SkyAmbientSHParams.x`).

### 7.3 Nuvens volumétricas — `FCloudNoise` + `FVolumetricClouds`
- **Bake (startup):** volume base 128³ (Perlin-Worley), detail 32³ (Worley) e weather 2D 512²
  (R=coverage, G=type, B=wetness). O weather map aceita override autorado por textura.
- **Por frame:** raymarch (compute, meia-res opcional) → temporal → composite "over" com depth
  gate, mais um **cloud shadow map** consumido pelo resto da cena.
- **Controles ao vivo** sem re-bake: coverage, density, altitude, vento, phase dual-lobe,
  powder, erosão.

### 7.4 Sombras — `FSunShadows` + `FLocalShadows`
CSM de 4 cascatas para o sol (detalhes em §15) e, para luzes locais, atlas 2D de spots +
cube array de points, com budget de slices por frame e fade por slot. `FGPULight::SpotParams`
carrega o slice de sombra, o fade e o `CastShadows` pedido pelo artista — o raster usa os dois
primeiros, o ReSTIR DI usa o terceiro para decidir se emite o shadow ray.

### 7.5 Ray tracing e GI
- `FRaytracingScene` — BLAS por mesh + TLAS reconstruída quando `Scene::TransformsVersion()`
  muda ou quando flags de instância mudam. Publica o **`InstanceGeo`**: snapshot por instância
  com índices bindless de VB/IB e das texturas, lido por todos os passes de RT.
- `FDDGI` — probes de irradiância + distância (Chebyshev) como *radiance cache*; roda na fila
  de compute assíncrona quando possível.
- `FReSTIRGI` — final-gather difuso por pixel sobre o DDGI (reservoir espaço-temporal).
- `FReSTIRDI` + `FReGIR` + `FMeshLights` — direta local por reservoir; o ReGIR troca o loop de
  luzes **dentro** do hit secundário (não substitui o DI de tela).
- `FReflections` — specular GI estilo Lumen: trace (glossy + mirror) → resolve → temporal →
  composite, com caminho dedicado para a água.
- `FNrdDenoiser` — RELAX difuso+especular; **duas instâncias independentes**, uma para o
  indireto e outra para a direta, para os históricos não se contaminarem.
- Política de raio centralizada em `FRayEpsilonProfile` (`RayEpsilons.h`), empurrada para
  ReSTIR/reflexões/DDGI todo frame; máscaras de instância em `RTMasks.h`.

### 7.6 Screen-space e temporal
`FAmbientOcclusion` (GTAO, meia-res opcional), `FHiZOcclusion` (cadeia HZB + teste de AABB
com readback em ring), `FTemporalMotionVectors` (vetor dual + histórico de superfície,
RT Gems II cap. 25), `FBackgroundVelocity` (motion vector do céu/nuvens/fog, que o G-buffer
deixaria zerado).

### 7.7 Reconstrução: `IUpscaler`
Interface única implementada por `FFsrPass` (FSR 3.1 via ffx-api), `FDlssPass` (DLSS-SR via
Streamline) e `FDlssRRPass` (DLSS Ray Reconstruction — denoise + upscale num eval). O
`Renderer::ActiveUpscaler()` resolve qual está ativo e devolve `nullptr` quando nada está
utilizável, degradando para o caminho TAA/nativo em vez de escrever numa textura estagnada.
`FDlssRRGuides` deriva do G-buffer os guides de material que o RR consome.

### 7.8 Água e terreno
`FOceanFFT` (3 cascatas com bandas espectrais disjuntas) + `FWaterRenderer` (§14) e
`FTerrain` (heightmap, CDLOD com morph contínuo, LOD por screen-size, mais um proxy que
existe **só** para o ray tracing).

### 7.9 Ferramentas de diagnóstico
- **`DebugTargets`** — registro **global** nome → slot SRV + como decodificar. Qualquer passe
  publica um alvo; o editor lista, filtra ("digite `reflex`") e compõe N deles numa grade
  offscreen. Quem registra é responsável por **re-registrar após resize** (o resize realoca
  slots) — invariante de protocolo, não garantida por tipo.
- **`VramTracker`** — registro **global** e thread-safe, alimentado no `Register()` de cada
  recurso; 10 categorias (`Geometry`, `SceneTextures`, `RenderTargets`, `Shadows`,
  `RaytracingAS`, `GI`, `Sky`, `Water`, `Terrain`, `Misc`). Desregistra sozinho pelo
  `ID3DDestructionNotifier`. A diferença para o `CurrentUsage` do DXGI aparece no editor como
  "não rastreado".
- **`FShaderTimer`** — heatmap de custo por pixel nos traces de RT via NVAPI (`NvGetSpecial`,
  slot falso u999). É **permutação**, não branch: custo zero desligado.
- **`FBvhDebugView`** — raio primário por pixel na TLAS (GPU Zen 3, 7.3.3); portátil, mostra
  conteúdo e densidade da estrutura.
- **`FFlickerHeatmap`** — variância temporal por pixel.
- **`FGpuProfiler`** — timestamps por escopo (`FGpuScope`), uma instância por fila.

### 7.10 Pós-processamento — `FPostProcessor`
Bloom (extract → downsample/upsample → blur) + **ACES filmic** tonemap, escrevendo direto no
swapchain e fazendo o downsample quando `RenderScale > 1` (SSAA).

---

## 8. Layouts de constant buffers

> Todos `alignas(256)`. Campos devem casar field-for-field com os `cbuffer` HLSL.
> **Não há header compartilhado entre C++ e HLSL** — o espelhamento é manual (§13).
> Regra de ouro: **campo novo vai no FIM**, para não mexer nos offsets já lidos pelos shaders.

### `FrameConstants` (b0) — 640 B de payload (768 B com `alignas(256)`)

Blocos, na ordem do struct:

| Bloco | Campos |
|---|---|
| Câmera / tempo | `CameraPosition`, `Time`, `InvViewProj` (Mat44, jitterada) |
| IBL | `IBLParams` (intensity, rotation, maxMip, enabled) |
| Sol / lua | `SunDirection`, `SunColor`, `MoonDirection`, `MoonColor` |
| Ambiente | `SkyAmbientColor`, `GroundAmbientColor` |
| DDGI | `DDGIGridMin`, `DDGIGridCount`, `DDGIParams`, `DDGIDistParams`, `DDGIBiasParams` |
| Reflexões | `ReflectionParams` |
| Render | `RenderParams` (mip bias global de textura) |
| Sombra de nuvens | `CloudShadowParams`, `CloudShadowParams2` |
| Luzes | `LightParams`, `LightParams2` |
| Atmosfera por pixel | `AtmoLightParams`, `SunColorRaw`, `MoonColorRaw` |
| Ambiente SH-L1 | `SkyAmbientSHR/G/B`, `SkyAmbientSHParams` |
| Cascatas DDGI | `DDGICascades` (`FDDGICascadeConstants`, 144 B, anexado no offset 496) |

### `ObjectConstants` (b2) — 4 `Mat44` = 256 B
`MVP` (jitterada, p/ `SV_POSITION`) · `ModelMatrix` (world) · `CurMVPNoJitter` · `PrevMVP`.
Os dois últimos existem para o motion vector.

### `FGPULight` / `FGPULightGI`
`FGPULight` (root SRV t17) espelha o struct do `DeferredLighting.ps.hlsl`: posição+1/raio,
cor+bulb, eixo+cos(outer), spot params, matriz de sombra e a posição do frame anterior
(shadow motion vector). `FGPULightGI` é a variante **compacta** para o mundo indireto —
sem matriz de sombra (a visibilidade lá é por shadow ray inline) e **sem frustum cull**
(luz atrás da câmera ilumina GI).

### `MaterialConstants` (b1) — 256 B (`static_assert`)
Factors (baseColor, metallic, roughness, AO, emissive) · flags `HasXMap` · `NormalStrength`
/`NormalFlipY` (GL vs DX) · bloco **POM** (`HeightScale`, min/max steps, self-shadow, fade em
mips, refine binário) · flags metalness/roughness separados. Espelhado em
`Shaders/MaterialCB.hlsli`.

---

## 9. Pipeline de shaders

**133 arquivos fonte HLSL**, com **139 compilações/permutações** registradas no CMake,
organizados por categoria:

```
Shaders/
├── GBuffer · DeferredLighting · ForwardBlend · DebugView · DepthNormal{,Masked} · Skybox
├── Common/     DepthConfig.hlsli          ├── GI/          DDGI · ReSTIR GI · ReGIR · HitShading
├── BRDF · MaterialCB · LightsCommon       ├── Lighting/    ReSTIR DI · MeshLights
├── RayEpsilons · RayOffset (.hlsli)       ├── Reflections/ trace · resolve · temporal · composite
├── Shadow/     CSM                        ├── Upscale/     DLSS-RR guides · BackgroundVelocity
├── Atmosphere/ LUTs · céu · estrelas      ├── Temporal/    surface · dual motion
├── Clouds/     bakes · raymarch           ├── HZB/  AO/  IBL/
├── Fog/  Rain/  SunShafts/                ├── Water/       superfície + FFT do oceano
├── Terrain/    CDLOD                      ├── Tonemap/     bloom · ACES · TAA · flicker
├── Preview/    material preview           ├── Selection/  Gizmo/  Debug/
```

- **Perfis:** `vs_6_0` (21) · `ps_6_0` (47) · `cs_6_0` (50) · **`cs_6_6` (21)**.
  O 6.6 é exigido por RayQuery + heap diretamente indexado (`ResourceDescriptorHeap`) nos
  passes de RT.
- **Permutações** são registradas como compilações extras com `OUTPUT_NAME`
  (ex.: `WaterSurfaceBase/Masks/Reflection` do mesmo `.ps.hlsl` com entry points distintos;
  `ReSTIRGITraceTimed`/`ReflectionTraceTimed` sob `SMILE_SHADER_TIMER=1`).
- **Hot-reload:** `MainWindow` observa `.hlsl`/`.hlsli` via `QFileSystemWatcher`; ao mudar,
  recompila (`cmake --target Shaders`) e chama `Renderer::ReloadShaders(stem)`. O `Renderer`
  consulta o `FPassRegistry`: cada `FRenderPass`/`FPipelineOwner` publica seus stems ao lado da
  própria criação de PSO. Mudanças em `.hlsli` fazem reload completo; um stem sem dono
  registrado gera `LogWarning` e não altera pipelines.

---

## 10. Arquitetura do Editor (Qt + QML)

A UI está em **migração incremental para QML** (ilhas `QQuickWidget` dentro do shell Widgets);
o viewport permanece um `HWND` nativo. Os painéis Widgets originais
(`MaterialEditorPanel`/`EnvironmentPanel`/`SkyCloudPanel`) **não existem mais**.

```
  ┌─────────────────────────────────────────────┐
  │ main.cpp (QApplication · fontes · tema dark) │
  └──────────────────────┬──────────────────────┘
                         ▼
  ┌─────────────────────────────────────────────┐
  │           MainWindow (frameless)            │
  └─┬──────────┬──────────┬──────────┬──────────┘
    │ central  │ QDock    │ QDock    │ janelas QML flutuantes
    ▼          ▼          ▼          ▼
 ┌────────┐ ┌──────────┐ ┌────────┐ ┌──────────────────────────────────┐
 │Viewport│ │Scene     │ │Console │ │ Materials · TimeOfDay · Settings  │
 │Widget  │ │Outliner  │ │Panel   │ │ Stats · DebugTargets              │
 └───┬────┘ └────┬─────┘ └───┬────┘ └────────────────┬─────────────────┘
     │ owns      │           │                       │
     │RenderThread           └── LogBridge           │  *Bridge (Q_OBJECT)
     ▼           ▼                                   ▼
  ┌───────────────────────────────────────────────────────────┐
  │        RendererHandle  (lock recursivo compartilhado)      │
  └────────────────────────────┬──────────────────────────────┘
                               ▼
                     ┌───────────────────┐
                     │  Smile::Renderer  │
                     └───────────────────┘
```

- **Viewport nativo:** `ViewportWidget` é um `QWidget` com `WA_NativeWindow` +
  `WA_PaintOnScreen` e `paintEngine() == nullptr` — entrega um `HWND` real ao DX12.
  Inicializa o `Renderer` de forma lazy e assíncrona no primeiro `showEvent`; emite
  `RendererInitialized` na thread Qt quando a render thread fica pronta. Durante o boot a
  `SplashScreen` consome o `FInitProgressCallback` do `Renderer`.
- **Render loop:** `QTimer` (`OnRenderTimer`) → coleta input → monta `CameraInput` → solicita
  um frame coalescido → `RenderThread` faz `RenderFrame`/`PresentFrame` → callback Qt mede FPS
  e emite `FrameReady`. Existe **no máximo um frame em voo do lado da CPU**, evitando fila e
  latência acumulada (independente dos 2 frames in-flight da GPU).
- **Sincronização:** `RendererHandle` protege o acesso das bridges com o mesmo lock recursivo
  usado pela render thread; `Lock()` devolve um guard RAII. Inicialização e shutdown também
  acontecem na thread de renderização.
- **Bridges QML** (`Q_OBJECT` + `Q_PROPERTY`/`Q_INVOKABLE`), uma por domínio:
  `MaterialsBridge`, `LightsBridge`, `SceneOutlinerBridge`, `TimeOfDayBridge`,
  `RenderSettingsBridge`, `CameraBookmarksBridge`, `CaptureBridge`, `MenuBridge`,
  `StatusBridge`, `LogBridge` e `WindowBridge`.
- **Controle externo de render:** `RenderSettingsController` concentra o acesso sincronizado que
  não pertence a uma tela. `McpBridge` traduz JSON/named pipe e usa snapshots tipados do
  controlador; depois de uma mutação externa, os sinais do controlador invalidam os mesmos
  bindings QML usados pelos setters da UI.
- **QmlHost** carrega os `.qml` do source dir em dev (hot-reload) e do lado do exe em deploy.
  `Theme.qml` é a fonte única de cores/tipografia; a fonte Inter é empacotada.
- **Logger → UI:** `Smile::SetLogSink` redireciona `LogInfo/Warning/Error` para o `ConsolePanel`.

---

## 11. Convenções de código

- **Comentários em PT-BR**, identificadores em **inglês**.
- Parâmetros de função em definições usam prefixo `_` (ex.: `_Device`, `_Width`); membros não.
  *Predominante, não universal:* ~35 arquivos seguem, ~11 usam o nome sem prefixo.
- `F`-prefix para tipos value/engine; sistemas grandes sem prefixo (`Renderer`).
- `static constexpr` para tamanhos de LUT/heap/config em vez de magic numbers.
- `SMILE_HR(expr)` para checar `HRESULT` (loga e lança `HResultException`).
- Tudo em `namespace Smile` (engine) / `SmileEditor` (editor).
- Recursos D3D via `ComPtr<T>` (`Smile::ComPtr` = `Microsoft::WRL::ComPtr`).
- Comentário explica **por quê**, não o quê — boa parte dos comentários longos do código
  registra uma decisão medida ou um bug já pago. Não os remova ao refatorar.

---

## 12. Guia de portabilidade

Mapeamento de conceitos de outras engines para onde encaixam na Smile.

### Tabela de equivalências

| Conceito (Unreal / Cry / Flax) | Equivalente Smile | Onde tocar |
|--------------------------------|-------------------|------------|
| RHI / `FRHICommandList` | `FCommandQueue` + `ID3D12GraphicsCommandList` | `Graphics/CommandQueue.*` |
| `FRDGBuilder` / frame graph | **manual** em `Renderer::RenderFrame` (barriers explícitos) | `Graphics/Renderer.cpp` |
| `UMaterial` / material graph | `FMaterial` + `MaterialConstants` (uber-shader, sem grafo) | `Graphics/Material.*`, `Shaders/MaterialCB.hlsli`, `Shaders/GBuffer.ps.hlsl` |
| Lumen (difuso) | `FDDGI` + `FReSTIRGI` | `Graphics/DDGI.*`, `Graphics/ReSTIRGI.*` |
| Lumen Reflections | `FReflections` | `Graphics/Reflections.*` |
| RTXDI / ReSTIR DI | `FReSTIRDI` + `FReGIR` | `Graphics/ReSTIRDI.*`, `Graphics/ReGIR.*` |
| Sky Atmosphere component | `FAtmosphere` (modelo Hillaire) | `Graphics/Atmosphere.*` |
| Volumetric Cloud component | `FCloudNoise` + `FVolumetricClouds` | `Graphics/VolumetricClouds.*` |
| Volumetric Fog | `FVolumetricFogPass` (froxel) + `FFogPass` (height) | `Graphics/VolumetricFog.*`, `Graphics/Fog.*` |
| Reflection capture / IBL | `FHDREnvironment` | `Graphics/HDREnvironment.*` |
| Cascaded Shadow Maps | `FSunShadows` | `Graphics/SunShadows.*` (§15) |
| Post Process Volume | `FPostProcessor` (bloom + ACES) | `Graphics/PostProcess.*` |
| TAA / TSR / DLSS | `FTemporalAA` + `IUpscaler` (FSR/DLSS/RR) | `Graphics/Upscaler.h` e implementações |
| Bindless / descriptor heap | `FTextureSRVHeap` (16384 slots) + `InstanceGeo` no RT | `Graphics/TextureSRVHeap.*`, `Graphics/RaytracingScene.*` |
| `SCENE_VIEW` / view uniforms | `FrameConstants` (b0) | `Graphics/Renderer.h` |
| `r.ShowRenderTarget` / debug views | `DebugTargets` + `FDebugView` | `Graphics/DebugTargets.*` |

### Receita para portar um *novo subsistema de rendering*

Siga a convenção existente (copie `FAmbientOcclusion` ou `FVolumetricClouds` como molde):

1. **Header** com:
   - `struct alignas(256) XxxConstants` casando o `cbuffer` HLSL (campo novo **no fim**).
   - `Initialize(device, ...)` — cria PSOs e o CB persistente.
   - `SetupForResize(device, srvHeap, w, h, ...)` + `ReleaseSizedResources(srvHeap)`.
   - `UpdatePerFrame(frameSlot, ...)` (escreve no CB mapeado do slot).
   - `Record*/Execute(commandList, srvHeap, ...)`.
   - `Recreate*(device, ...)` p/ hot-reload · `IsReady()`/`IsInitialized()` ·
     `InvalidateHistory()` se acumular entre frames.
2. **Recursos:** crie via `GpuResources` (§4) — nunca `CreateCommittedResource` direto, senão
   o recurso fica fora do breakdown de VRAM. Aloque SRV/UAV no `FTextureSRVHeap` e **libere** no
   `ReleaseSizedResources`; monte tabelas contíguas com `FTextureSRVHeap::CopyTable`. Use o
   overload parametrizável de `FComputePipeline`.
3. **Constant buffer:** `GpuResources::CreateUploadBuffer(dev, sizeof(XxxConstants),
   FCommandQueue::kFramesInFlight)` — um slice por frame em voo, já alinhado a 256 B.
4. **Barreiras:** `TransitionResource` para uma, `FBarrierBatch` para 2+. Nunca
   `ResourceBarrier` avulso montado na mão.
5. **Shaders:** registre nos **dois** pontos do `Shaders/CMakeLists.txt` (§2) e carregue
   `<nome>.<perfil>.cso`.
6. **Integração no `Renderer`:**
   - membro por valor + flag `UseXxx` + getter para o editor;
   - `Xxx.Initialize(...)` em `Renderer::Initialize`; `SetupForResize` em `RecreateInternalTargets`;
   - `UpdatePerFrame` no bloco de topo de `RenderFrame`; `Record*` na posição certa (§5.2);
   - registre o `Recreate*` na tabela de `ReloadShaders` **e** em `RecreateAllPSOs`;
   - se acumular histórico, **entre nas listas de invalidação da §5.4**;
   - publique os RTs interessantes em `DebugTargets::Register` dentro de `RegisterDebugTargets()`.
7. **Editor:** exponha setters no `Renderer` e ligue numa bridge QML (modelo do
   `TimeOfDayBridge`). Setters que sujam estado de RT devem usar as versões **coalescidas**
   (`MarkMaterialRTStateDirty`/`MarkIndirectLightingDirty`).

### Pegadinhas ao portar

- **Tudo é HDR linear** até o tonemap. RTs intermediários devem ser `R16G16B16A16_FLOAT`;
  não escreva sRGB antes do `FinalTonemap`. (O `GBufferA` é a exceção deliberada: sRGB de
  8 bits para ganhar precisão no albedo escuro — os shaders seguem 100% em linear.)
- **Reverse-Z:** near→1, far→0, clear em 0.0, comparação `GREATER`. Só a câmera principal;
  as CSM são forward-Z. Ao portar código de outra engine, **inverta as comparações**.
- **Sem MSAA:** todo o pipeline é single-sample (`SampleDesc.Count = 1`); o AA vem do
  upscaler/TAA. Não reintroduza `SampleDesc.Count > 1`.
- **Render-res ≠ display-res.** Passes de cena vivem em `RenderWidth/Height`; só o pós e o
  backbuffer estão em resolução nativa. Não misture os dois domínios ao alocar RTs.
- **Jitter:** a `Projection` do frame é jitterada; a *não* jitterada (`ProjUnjittered`) é a que
  vale para motion vectors e reprojeção. Escolha a certa.
- **Profundidade como SRV:** o depth é `R32_TYPELESS` (DSV `D32` + SRV `R32`) e vive no 4º slot
  contíguo ao G-buffer (`FGBuffer::DepthSRVSlot()`).
- **2 frames in-flight:** CBs e uploads precisam de um slice por slot. Reescrever o mapeado sem
  versionar por `FrameSlot` corrompe o que o frame em voo está lendo.
- **Frame de km da atmosfera/nuvens** é separado das unidades da cena; use `InvViewProjNoTrans`
  para raios de mundo.
- **Matrizes LH, row-major**; multiplicação é `Model*View*Proj`.

---

## 13. Limitações atuais & dívida técnica

> **Revisado parcialmente em 2026-08-19.** Esta seção descreve restrições observáveis no código
> atual. Planos e auditorias vinculados preservam o histórico das decisões, mas seus números
> pontuais devem ser lidos na data registrada em cada documento.

### Estrutural

- **`Renderer` ainda concentra estado e coordenação.** O arquivo `Renderer.cpp` possui cerca de
  5.700 linhas e o header cerca de 1.260. A extração já criou `FRenderSettings`,
  `FSceneTargets`, `FrameContext`, captura dedicada e o contrato de passes; `RenderFrame()` caiu
  para aproximadamente 370 linhas, mas o `Renderer` continua sendo o ponto de integração de
  muitos subsistemas. O histórico da migração está em [KNOBS-AUDIT.md](KNOBS-AUDIT.md).
- **O grafo de invalidação virou dado, mas exige disciplina.** `HistoryDomain.h` mantém 17
  `EHistoryTarget` e máscaras nomeadas pelo motivo da invalidação. Um acumulador novo ainda
  precisa receber um bit e entrar nos domínios corretos; esquecer esse cadastro produz histórico
  que sobrevive além do contrato.
- **A migração para `FRenderPass` é incremental.** `FPassRegistry` já centraliza resize,
  invalidação, debug targets e recriação de pipelines dos passes migrados. A execução continua
  explícita por design — não existe um `virtual Execute()` genérico — e subsistemas ainda não
  migrados conservam assinaturas de ciclo de vida diferentes.
- **Boilerplate DX12 duplicado** — *funil de recursos resolvido*. `GpuResources.h` (§4) é o
  caminho único da engine para DEFAULT/UPLOAD/READBACK e aplica tracking, instrumentação e a
  política D3D12MA num só ponto. ~60 root signatures ainda seguem montadas campo a campo.
- **`ViewportWidget` ainda concentra responsabilidades do editor:** host do HWND, input,
  telemetria, visualizador de alvos e fila de jobs do renderer continuam no mesmo tipo. Os knobs
  já saíram para `RenderSettingsBridge`, e configuração/snapshot externos passam por
  `RenderSettingsController`; a separação restante é sobretudo de telemetria e apresentação.
- **Acoplamento de compilação:** 59 dos 73 headers públicos de `Graphics/` puxam
  `<d3d12.h>`/`<Windows.h>`; `Renderer.h` puxa 69 headers e é incluído por 10 TUs.
- **Inversão de dependência menor:** `Engine/Source/Scene/SceneLoader.cpp` inclui
  `Graphics/Renderer.h` — a camada de cena depende do renderer.
- **Submissão de draws.** O Z-prepass continua front-to-back (Hi-Z). O G-buffer, depois do
  depth EQUAL, agrupa por PSO/material/mesh e o `FDrawSubmitCache` pula Bind/IA repetidos —
  o CSM já fazia o equivalente. Translúcidos seguem back-to-front na lista original.

### Build e ferramentas

- ~~**Lista de shaders duplicada** no `Shaders/CMakeLists.txt`~~ — **resolvido em 2026-08-04**:
  a lista do IDE é derivada de `smile_compile_shader` (§2).
- ~~**Dependência de shader por GLOB total**~~ — **resolvido em 2026-08-04**: grafo de
  `#include` calculado no configure (§2). O DXC do SDK não suporta depfile; se um dia a engine
  passar a usar um DXC ≥ 1.7, trocar por `-MF` + `DEPFILE` elimina o reconfigure de ~1,2 s.
- ~~**Tabelas duplicadas de hot-reload**~~ — **resolvido** pelo `FPassRegistry`: stems e
  recriação ficam no dono do pipeline. A dívida remanescente é garantir que todo pipeline novo
  seja registrado; stems sem dono falham de forma explícita no log.
- **Caminhos absolutos de máquina:** os defaults dos 4 SDKs apontam para `D:/Engines/...`
  (são cache vars, então sobrescrevíveis). O DXC segue procurado em dois caminhos fixos com
  `NO_DEFAULT_PATH`. *O `-I "D:/Engines/NRD/Shaders"` hardcoded 3× foi resolvido em
  2026-08-04 — agora sai de `SMILE_NRD_ROOT`.*
- **Lista manual de headers no CMake:** ainda há headers públicos fora de `ENGINE_HEADERS`.
  Isso não quebra a compilação por si só, mas prejudica a navegação e os filtros do Visual Studio.

### Cobertura e correção

- **Sem header compartilhado C++/HLSL.** 89 arquivos com `cbuffer`, todo layout espelhado à mão
  com comentários "manter em sincronia". Classe de bug silenciosa e cara; a solução usual é um
  `.hlsli` com `#ifdef __cplusplus` incluído dos dois lados.
- **Testes: 3 executáveis** (`OceanSpectrum`, `TimeOfDay`/lua e identidade de `FScene`).
  Ainda não há cobertura dedicada para `Mat44`/`Vec*`, versionamento do `CookedFormat` ou
  culling; os caminhos completos de renderização também dependem de validação em GPU.
- **`FScene` é uma lista plana** (sem hierarquia/parentesco); o editor faz `push_back` direto e
  `Renderables()` devolve referência mutável. A encapsulação é por convenção.
- **Sem serialização de cena / undo-redo / asset DB** no editor. Persistência existe só por
  sidecars (`<cena>.materials.json`, `<cena>.terrain.json`).
- **Material sem grafo** (uber-shader parametrizado por `MaterialConstants`).
- **Convenções divergentes:** prefixo `_` em parâmetros (~35 arquivos sim, ~11 não); sufixo `_`
  em membros em 15 pontos. Um `.clang-format` fecharia isso.

---

## 14. Oceano FFT

O oceano combina três cascatas espectrais físicas, IFFT compute, clipmap GPU-driven,
shading forward e reflexão hierárquica posterior à escrita de depth/G-buffer:
SSR de contato sobre as cópias sem água, DXR no miss ou na cobertura parcial e céu no
miss final.
A IFFT empacota altura e deslocamento horizontal no mesmo `float4` (dois dispatches
por cascata) e o mapa de deslocamento ping-ponga entre dois alvos — o frame anterior
não é copiado.
O `SceneColorCopy` é HDR e possui cadeia completa de mips gerada antes da água; o SSR
seleciona um LOD contínuo pelo footprint GGX em vez de refletir sempre o mip 0.
Normal, Sol e reflexão compartilham momentos de slope anisotrópicos, e o histórico
dedicado escreve hit distance válido para Ray Reconstruction. O contrato matemático,
a ordem dos passes, a validação e os limites atuais estão documentados em
[`OCEAN-AUDIT.md`](OCEAN-AUDIT.md).

---

## 15. Sombra do sol (CSM)

Quatro cascatas 2048² num `Texture2DArray` D32, splits pela progressão geométrica da
Unreal (expoente 3.0), fitting por esfera da fatia do frustum com snapping de texel e
raio quantizado, pancaking por hardware (`DepthClipEnable = FALSE`), PCF de 16 taps
Poisson rotados por ruído, PCSS nas cascatas 0 e 1, crossfade por profundidade de view
com sobreposição geométrica e cache round-robin nas cascatas distantes. O froxel de
volumetric fog e os sun shafts consomem o mesmo mapa por um caminho de tap único.

A revisão completa — geometria medida por cascata, defeitos encontrados com severidade,
comparação linha a linha com Unreal, Cry e Flax, o que a fase 1 corrigiu e a fila que
resta — está em [`CSM-AUDIT.md`](CSM-AUDIT.md).

---

*Escrito a partir da leitura do código em `Engine/`, `Editor/`, `Shaders/`, `Tools/` e do CMake.
Para detalhes de implementação, os headers em `Engine/Include/Smile/Graphics/` são a fonte de
verdade da API de cada subsistema. Ao mexer em algo que este documento descreve — sobretudo
§4, §5, §6 e §13 — atualize a seção junto com o commit.*
