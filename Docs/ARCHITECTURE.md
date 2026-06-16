# SmileEngine — Documentação de Arquitetura

> Referência rápida da engine para consulta e como base ao **portar recursos** de
> outras engines (Unreal, CryEngine, Flax, papers/SIGGRAPH, etc.) para a SmileEngine.
>
> **Versão:** 0.1.0 · **Stack:** C++20 · DirectX 12 · Qt 6 (Widgets) · HLSL SM 6.0 (DXC)
> **Plataforma:** Windows x64 (MSVC 2022) · **Build:** CMake + Visual Studio 17 2022

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
10. [Arquitetura do Editor (Qt)](#10-arquitetura-do-editor-qt)
11. [Convenções de código](#11-convenções-de-código)
12. [Guia de portabilidade (Unreal/Cry/Flax → Smile)](#12-guia-de-portabilidade)
13. [Limitações atuais & dívida técnica](#13-limitações-atuais--dívida-técnica)

---

## 1. Visão geral

A SmileEngine é uma engine pessoal de aprendizado/render, reconstruída do zero sobre
**DirectX 12** (substituiu um setup anterior em DX11). É dividida em três artefatos:

| Artefato | Pasta | Namespace | Tipo | Descrição |
|----------|-------|-----------|------|-----------|
| **Engine** | `Engine/` | `Smile` | Biblioteca **estática** | RHI DX12 + subsistemas de rendering |
| **Editor** | `Editor/` | `SmileEditor` | Executável **Qt 6** | Host do viewport, painéis, tema dark |
| **Shaders** | `Shaders/` | — | Alvo de build (DXC) | HLSL compilado para `.cso` em build time |

Princípios de design observados no código:

- **Composição, não singletons.** Cada `ViewportWidget` cria seu próprio `Smile::Renderer`.
  O `Renderer` é dono (por valor) de todos os subsistemas (`FAtmosphere`, `FVolumetricClouds`, …).
- **Prefixos por tipo:** `F` para tipos "engine/value-like" (`FD3D12Device`, `FMaterial`,
  `FAtmosphere`), classes "sistema" sem prefixo (`Renderer`, `Camera` via `FCamera`).
- **Padrão de subsistema uniforme.** Quase todo subsistema segue o ciclo:
  `Initialize(...)` → `UpdatePerFrame(...)` → `Record*Pass(CommandList, SRVHeap)` →
  `Recreate*(...)` (hot-reload de shader) → `Resize(...)`.
- **Um único heap CBV/SRV/UAV compartilhado** (`FTextureSRVHeap`, 512 slots) shader-visible,
  usado por toda a engine. Slots são alocados de forma *bump-pointer* e nunca liberados.
- **Bakes únicos no startup** para LUTs caras (IBL, atmosfera, ruído de nuvens), seguindo
  todos o mesmo padrão de compute → barrier → estado de leitura.

```
  ┌──────────────────────────────────────┐     ┌────────────────────────────────────────┐
  │      Editor (Qt 6 · SmileEditor)      │     │         Engine (DX12 · Smile)          │
  │                                       │     │                                        │
  │            ┌─────────────┐            │     │            ┌──────────┐                │
  │            │ MainWindow  │            │     │   ┌───────►│ Renderer │◄──────┐        │
  │            └──────┬──────┘            │     │   │        └──────────┘       │        │
  │          ┌────────┴────────┐          │     │   │                           │        │
  │          ▼                 ▼          │     │   ▼                           ▼        │
  │  ┌──────────────┐  ┌──────────────┐   │     │ ┌──────────────────┐  ┌──────────────┐│
  │  │ ViewportWidget│  │ Painéis:     │  │     │ │ RHI: Device ·    │  │ Subsistemas: ││
  │  │ QWidget HWND  │  │ Material ·   │  │     │ │ CmdQueue ·       │  │ Atmosphere · ││
  │  │ nativo        │  │ Environment ·│  │     │ │ SwapChain ·      │  │ Clouds ·     ││
  │  └───────┬───────┘  │ SkyCloud     │  │     │ │ DescriptorHeap · │  │ HDR/IBL ·    ││
  │          │          └──────┬───────┘  │     │ │ SRVHeap · PSO    │  │ Skybox ·     ││
  │          │ owns            │ setters   │     │ └────────┬─────────┘  │ PostProcess  ││
  │          │ unique_ptr      │           │     │          ┊            │              ││
  └──────────┼─────────────────┼──────────┘     └──────────┼────────────└──────────────┘┘
             │                 │                            ┊ carrega em runtime
             └─────────────────┴────────► Renderer          ▼
                                              ┌───────────────────────────────┐
                                              │ Shaders .cso (compilados DXC)  │
                                              └───────────────────────────────┘
```

---

## 2. Topologia do projeto / build

### Hierarquia CMake

```
CMakeLists.txt (raiz)                  ← project, C++20, x64, flags MSVC /W4, acha Qt6
├── cmake/CompileShaders.cmake         ← função smile_compile_shader() (acha dxc.exe)
├── Engine/CMakeLists.txt              ← add_library(SmileEngine STATIC ...)
├── Shaders/CMakeLists.txt             ← add_custom_target(Shaders) (lista todos os .hlsl)
└── Editor/CMakeLists.txt              ← qt_add_executable(SmileEditor ...)
```

### Decisões de build relevantes

- **C++20**, `/W4 /permissive- /Zc:__cplusplus /MP /utf-8`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`,
  `UNICODE`. x64 forçado.
- **Qt** padrão em `C:/Qt/6.10.2/msvc2022_64` (sobrescrevível via `-DCMAKE_PREFIX_PATH`).
  Componentes: `Core Gui Widgets`. AUTOMOC ligado — headers com `Q_OBJECT` **devem ser
  listados explicitamente** no `qt_add_executable`.
- **Shaders** compilados em build time por **DXC** (`dxc.exe` do Windows SDK `10.0.22621.0`).
  Saída: `${CMAKE_BINARY_DIR}/bin/Shaders/<nome>.<perfil>.cso`. O caminho é exposto ao C++
  via `target_compile_definitions(SmileEngine PUBLIC SMILE_SHADER_DIR="...")`.
- **Saída unificada** em `build/bin` (facilita deploy das DLLs do Qt junto do `.exe`).
- `VersionInfo.h` é **gerado** de `VersionInfo.h.in` via `configure_file` (versão + data de build).
- Debug: `-Zi -Od -Qembed_debug`; Release: `-O3`.

### Como adicionar um shader novo

1. Crie o `.hlsl` em `Shaders/<Categoria>/`.
2. Adicione em `Shaders/CMakeLists.txt`:
   `smile_compile_shader(Categoria/Nome.cs.hlsl cs_6_0 main SHADER_OUTPUTS)` e na lista
   `SOURCES` do `add_custom_target(Shaders ...)` (e o `.hlsli` incluído, se houver).
3. No C++, carregue `Nome.cs_6_0.cso` (sufixo = perfil) a partir de `SMILE_SHADER_DIR`.

---

## 3. Mapa de módulos

```
Engine/Include/Smile/
├── Core/
│   ├── Types.h          u8..u64, i8..i64, f32/f64, ComPtr<T> alias
│   ├── Logger.h         LogInfo/Warning/Error + SetLogSink (callback p/ o Editor)
│   ├── HResultCheck.h   macro SMILE_HR(...) → throw em FAILED(hr)
│   └── VersionInfo.h.in template gerado (versão/data)
├── Math/                Vec2/3/4, Mat44 (row-major, LH), MathUtils, ToRad/ToDeg
├── Input/
│   └── CameraInput.h    struct { Vec3 Move; Vec2 Look; f32 Speed; }
└── Graphics/
    ├── ── RHI / núcleo ──
    │   ├── D3D12Device       device + DXGI factory6 + adapter + tearing
    │   ├── CommandQueue      queue + allocator + list + fence (1 frame in-flight)
    │   ├── SwapChain         2 buffers, R8G8B8A8_UNORM, tearing opcional
    │   ├── DescriptorHeap    wrapper genérico (RTV/DSV/etc, não-shader-visible)
    │   ├── TextureSRVHeap    heap CBV/SRV/UAV compartilhado (512, shader-visible)
    │   ├── PipelineState     root signature PBR + PSO da cena
    │   ├── ComputePipeline   PSO de compute fixo (5 passes de IBL)
    │   └── VolumetricPipeline PSO de compute parametrizável (atmosfera/nuvens)
    ├── ── recursos ──
    │   ├── Texture / CubeTexture / VolumeTexture   wrappers + defaults (white/normal/ORM/black)
    │   ├── Mesh              Vertex {pos,normal,uv}, CreateCube/CreateSphere
    │   └── Material          FMaterial (8 slots de textura + MaterialConstants 256B)
    ├── ── ambiente / céu ──
    │   ├── HDREnvironment    IBL: equirect→cube, irradiance, specular prefilter, BRDF LUT
    │   ├── Skybox            desenho do cubemap HDR de fundo
    │   ├── Atmosphere        Hillaire: transmittance/multiscatter/sky-view LUTs + sky PS
    │   ├── CloudNoise        bake de volumes 3D (Perlin-Worley base + Worley detail) + weather 2D
    │   └── VolumetricClouds  raymarch (compute) → composite (over) com depth gate
    ├── ── pós ──
    │   └── PostProcess       Bloom (extract→blur H/V) + ACES filmic tonemap → swapchain
    ├── Camera                FCamera fly-cam (pos/pitch/yaw)
    └── Renderer              maestro: cria tudo, monta o frame, expõe setters p/ o Editor
```

---

## 4. Camada de abstração DX12 (RHI)

A "RHI" é deliberadamente fina — wrappers diretos sobre objetos DX12, sem indireção.

### `FD3D12Device`
Cria `ID3D12Device` + `IDXGIFactory6` + escolhe o `IDXGIAdapter1` (com descrição e VRAM).
Detecta suporte a **tearing** (vsync-off). Debug layer ligada em `_DEBUG`.

### `FCommandQueue`
Modelo de sincronização **mais simples possível: 1 frame in-flight**. Um único
allocator + uma única command list + um fence.
- `ResetForRecording()` — reseta allocator/list para gravar o frame.
- `ExecuteAndSync(lists, n)` — executa, **sinaliza e espera o fence** (flush total).
- `Flush()` — bloqueia até a GPU drenar (usado antes de recriar recursos).

> Ponto de evolução nº1: para mais throughput, migrar para N allocators/lists +
> ring de fence por frame (frames-in-flight ≥ 2).

### `FSwapChain`
2 buffers, formato **`R8G8B8A8_UNORM`** (o backbuffer final é LDR; o HDR vive em RTs
separados — ver §5). RTV heap próprio. `Present()` respeita tearing.

### `FDescriptorHeap` vs `FTextureSRVHeap`
- `FDescriptorHeap` — wrapper genérico, usado para heaps **não shader-visible**
  (RTV, DSV) e pequenos (1–2 slots).
- `FTextureSRVHeap` — **o** heap CBV/SRV/UAV global, shader-visible, **512 slots**.
  `Allocate(count)` faz bump-pointer (retorna o slot inicial). `CreateSRV/CreateUAV`
  escrevem a view num slot. Tabelas contíguas são montadas via `CopyDescriptors`
  (ex.: a tabela IBL t8..t10 no `Renderer::CreateIBLDescriptorTable`).

> ⚠️ Slots **nunca são liberados**. Swaps de HDR/recriações realocam novos slots.
> Em 512 slots cabe a fase atual; um free-list/alocador por geração é evolução futura.

### Pipelines de compute: dois sabores
| Classe | Forma do root sig | Usado por |
|--------|-------------------|-----------|
| `FComputePipeline` | **fixa** (não documentada como parametrizável) | os 5 passes de IBL |
| `FVolumetricPipeline` | **parametrizável**: `b0` CBV + tabela SRV `t0..t(N-1)` + tabela UAV `u0..u(M-1)` + `s0` linear-clamp + `s1` linear-wrap | atmosfera, nuvens |

`FVolumetricPipeline::Initialize(device, csoName, numSRVs, numUAVs)` — as *dimensões*
das views (2D/3D/cube) ficam no shader, então a mesma root sig serve a tudo.

---

## 5. O frame: render loop e frame graph

O loop é dirigido pelo Editor: `ViewportWidget` tem um `QTimer` (`OnRenderTimer`) que
chama `Renderer::UpdateCamera(...)` + `Renderer::RenderFrame()` e emite `FrameReady`.

### `RenderFrame()` — sequência completa (Renderer.cpp)

```
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 1. Atualiza FrameConstants (MVP, câmera, sol, IBL, tempo, ambiente hemisf.)│
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 2. UpdatePerFrame dos subsistemas  (Atmosphere · Clouds)                  │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 3. CommandQueue.ResetForRecording                                          │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 4. Bind HDR RT + Depth · clear (cor 0.094 / depth=1)                      │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────── Céu de fundo ─────────────────────────────────┐
  │  atmosfera ON ─► Bake Sky-View LUT (compute) + RenderSky (fullscreen)     │
  │  senão (HDR)  ─► Skybox.Render (cubemap)                                  │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 5. Cena PBR  (root sig PBR · material bind · IBL table · DrawIndexed)     │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 6. [Nuvens]  RecordRaymarch (compute) → Composite "over" com depth gate   │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 6b. Barrier HDR → PIXEL_SHADER_RESOURCE                                    │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 7. Backbuffer → RENDER_TARGET                                              │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 8. PostProcess.Execute  (Bloom extract→blur H/V + ACES tonemap → swap)    │
  └───────────────────────────────────┬───────────────────────────────────────┘
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ 9. Backbuffer → PRESENT · Close · ExecuteAndSync (flush) · Present        │
  └─────────────────────────────────────────────────────────────────────────┘
```

### Render targets e formatos

| Buffer | Formato | Papel |
|--------|---------|-------|
| `HDRColorBuffer` | `R16G16B16A16_FLOAT` | alvo HDR único (cena toda renderiza aqui) |
| `DepthBuffer` | `R32_TYPELESS` (DSV `D32_FLOAT` + SRV `R32_FLOAT`) | depth, **também exposto como SRV** p/ atmosfera/nuvens lerem profundidade |
| Backbuffer | `R8G8B8A8_UNORM` | saída final LDR após tonemap |

**Sem MSAA:** o anti-aliasing é feito por TAA (single-sample em todo o pipeline). Toda a
cena renderiza em float16 linear → Bloom + tonemap ACES escrevem o LDR direto no
swapchain. `NearZ=0.1`, `FarZ=20000`
(far estendido para alcançar o horizonte). FOV 60°, perspectiva **LH**.

### Frame de coordenadas da atmosfera/nuvens
Atmosfera e nuvens vivem num **frame em km**, desacoplado das unidades da cena. A câmera
é colocada em `(0, viewHeight, 0)` com `viewHeight = 6360 + kGroundAltitudeKm(0.5)`. A
reconstrução de raio do mundo usa `InvViewProjNoTrans` (view sem translação · proj)⁻¹.

---

## 6. Modelo de binding (root signatures & descriptors)

### Root signature da cena PBR (`FPipelineState`)

```
[0] CBV   b0           FrameConstants            (visível a todos)
[1] CBV   b1           MaterialConstants         (PS)
[2] Table SRV t0..t7   8 texturas do material    (PS)
[3] Table SRV t8..t10  IBL: irradiance, specular prefiltered, BRDF LUT  (PS)
Static s0  ANISOTROPIC WRAP    (materiais, MaxAniso 16)
Static s1  LINEAR CLAMP        (cubemaps + LUT)
```

Slots de textura do material (`kMaterialTextureSlots = 8`, ordem em `FMaterial`):

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

Cada `FMaterial` aloca **8 slots contíguos** no SRV heap (`SRVTableStart`) + um CBV de
256B (`MaterialConstants`). `Finalize()` cria o CBV e preenche a tabela; `Bind()` seta
o root CBV b1 e a tabela t0..t7. Flags `HasXMap` no CB dizem ao shader quais usar.

### Root sig do compute volumétrico
`b0` CBV · tabela `t0..t(N-1)` · tabela `u0..u(M-1)` · `s0` linear-clamp · `s1` linear-wrap.

### Convenção de descriptors
Tabelas contíguas (IBL, atmosfera, nuvens) são montadas copiando SRVs dispersos
para um bloco contíguo via `ID3D12Device::CopyDescriptors`. Exemplos:
- IBL: `[irradiance, specular, BRDF]` (Renderer).
- Atmosfera bake sky-view: `[transmittance(t0), multiscatter(t1)]`.
- Atmosfera render: `[skyview(t0), transmittance(t1)]`.

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

Sizes em `static constexpr`: `kEnvCubeSize=1024`, `kSpecularMips=7`, `kSpecularSampleCount=512`.
Swap de HDR em runtime re-roda a cadeia (`LoadFromFile`). Antes de qualquer HDR carregado,
um 1×1 preto neutro mantém os SRVs válidos. `kSpecularMips-1` vai no `IBLParams.z` do FrameCB.

### 7.2 Skybox — `FSkybox`
Desenha o `EnvCube` como fundo (depth no far-plane) quando a atmosfera está desligada e
há HDR carregado. Recebe `InvVPNoTrans`, intensidade e rotação IBL.

### 7.3 Atmosfera física — `FAtmosphere` (Hillaire)
"Scalable and Production Ready Sky and Atmosphere". LUTs:

| LUT | Tamanho | Quando | Conteúdo |
|-----|---------|--------|----------|
| Transmittance | 256×64 | bake 1x no startup (re-bake se `Dirty`) | extinção ao longo do raio |
| Multi-Scatter | 32×32 | bake 1x no startup | espalhamento múltiplo |
| Sky-View | 192×104 | **bake por frame** (compute) | céu visto da câmera |

Parâmetros físicos em `AtmosphereConstants` (Rayleigh/Mie/Ozônio em km⁻¹, raios do planeta
em km — `kGroundAltitudeKm=0.5`). `RenderSky` faz um draw fullscreen reconstruindo o raio
via `InvViewProjNoTrans`. Disco solar e glare controláveis sem re-bake. A atmosfera tem
precedência sobre o skybox HDR quando `UseAtmosphereSky` (default **on**).

**Ambiente hemisférico derivado (A4):** o `Renderer` computa, na CPU, cor de céu (zênite
Rayleigh-blue) e chão a partir da altura do sol e injeta em `SkyAmbientColor`/
`GroundAmbientColor` do FrameCB — ambiente coerente com o céu físico.

### 7.4 Nuvens volumétricas — `FCloudNoise` + `FVolumetricClouds`
- **Bake (startup):** `FCloudNoise` gera volume **base 128³** (Perlin-Worley), volume
  **detail 32³** (Worley/erosão) e **weather 2D 512²** (R=coverage, G=type, B=wetness).
- **Por frame:** `RecordRaymarch` (compute) marcha um shell esférico de nuvem para um RT
  em resolução de tela; `Composite` mistura "over" sobre o céu com **depth gate** (lê a
  SRV de profundidade da cena). Single-scatter com phase dual-lobe (g1/g2), powder e erosão.
- **Controles ao vivo** (sem re-bake): coverage, density, altitude (bottom/thickness km),
  wind, phase g, powder, erosion — todos mutam `CPUConstants`, propagados em `UpdatePerFrame`.

### 7.5 Pós-processamento — `FPostProcessor`
`Execute` recebe o HDR resolvido e escreve direto no swapchain:
1. **Bloom Extract** (threshold de brilho) → `BloomBuffer`.
2. **Blur separável** H e V → `BloomBlurBuffer` (CB de direção `(1/w,0)` / `(0,1/h)`).
3. **Final Tonemap** — ACES filmic + exposição, combina cena + bloom → LDR swapchain.

`PostParams` (256B): `BloomIntensity`, `Exposure`.

---

## 8. Layouts de constant buffers

> Todos `alignas(256)`. Campos devem casar field-for-field com os `cbuffer` HLSL.

### `FrameConstants` (b0) — 240/256 B
```cpp
Mat44 MVP;                 // model*view*proj
Mat44 ModelMatrix;
Vec4  CameraPosition;      // xyz, w=1
Vec4  IBLParams;           // x=intensity y=rotation(rad) z=maxMip w=enabled
Vec4  Time;                // x=elapsed y=delta z=frameIndex
Vec4  SunDirection;        // xyz=dir TO sun (norm), w=intensity
Vec4  SunColor;            // rgb
Vec4  SkyAmbientColor;     // rgb=zênite, w=enabled(0/1)
Vec4  GroundAmbientColor;  // rgb=nadir,  w=intensity
```

### `MaterialConstants` (b1) — 256 B (static_assert)
Factors (baseColor, metallic, roughness, AO, emissive) · flags `HasXMap` · `NormalStrength`
/`NormalFlipY` (GL vs DX) · bloco **POM** (`HeightScale`, min/max steps, self-shadow,
fade start/range em mips, refine binário) · flags metalness/roughness separados.

### Outros
- `AtmosphereConstants` — params físicos + tamanhos de LUT + sun + `InvViewProjNoTrans`.
- `CloudConstants` — radii do shell, coverage/density/erosion/detail, vento, march steps,
  phase dual-lobe, powder, screen params.
- `PostParams` — bloom + exposição.

---

## 9. Pipeline de shaders

```
Shaders/
├── Triangle.{vs,ps}     ← cena PBR principal (apesar do nome legado)
├── Skybox.{vs,ps}
├── IBL/        EquirectToCube · MipGen · IrradianceConvolution · SpecularPrefilter · BRDFIntegration (+Common.hlsli)
├── Atmosphere/ BakeTransmittance · BakeMultiScatter · BakeSkyView · SkyAtmosphere.{vs,ps} (+AtmosphereCommon.hlsli)
├── Clouds/     BakeBaseNoise · BakeDetailNoise · BakeWeather · CloudRaymarch · CloudComposite.{vs,ps}
│               (+CloudNoiseCommon · CloudDensity · CloudLighting .hlsli)
└── Tonemap/    PostProcess.vs · BloomExtract.ps · BloomBlur.ps · FinalTonemap.ps
```

- **Perfis:** `vs_6_0` / `ps_6_0` / `cs_6_0` (DXC, SM 6.0).
- **Hot-reload:** `MainWindow` observa todos os `.hlsl`/`.hlsli` via `QFileSystemWatcher`;
  ao mudar, recompila (`cmake --target Shaders`) e chama `Renderer::ReloadShaders(stem)`,
  passando o nome do `.cso` alterado (ex.: `WaterSurface.ps`). O `Renderer` mantém uma
  tabela `stem → recriação` e recria apenas o PSO afetado; stem não mapeado ou `.hlsli`
  (include compartilhado) cai em reload completo (`RecreateAllPSOs`).
  Stylesheets `.qss` do Editor também têm hot-reload.
- O `.cso` é nomeado `<nome>.<perfil>.cso` e carregado por `LoadShaderBlob` a partir de
  `SMILE_SHADER_DIR`.

---

## 10. Arquitetura do Editor (Qt)

```
  ┌────────────────────────────────────────┐
  │ main.cpp (QApplication + ApplyDarkTheme)│
  └────────────────────┬───────────────────┘
                       ▼
  ┌────────────────────────────────────────┐
  │              MainWindow                 │
  └─┬────────┬──────────┬──────────┬────────┘
    │central │ QDock    │ QDock    │ status/log
    ▼        ▼          ▼          ▼          
 ┌───────┐ ┌──────────┐ ┌─────────┐ ┌──────────┐ ┌─────────────────────┐
 │Viewport│ │Material  │ │Environ- │ │SkyCloud │ │ QTextEdit+SetLogSink │
 │Widget  │ │EditorPan.│ │mentPanel│ │Panel    │ └─────────────────────┘
 └───┬───┘ └────┬─────┘ └────┬────┘ └────┬────┘
     │unique_ptr│SetMaterial │LoadHDR    │sol · atmosfera ·
     │          │/ texturas  │/ IBL      │nuvens
     ▼          ▼            ▼           ▼
  ┌────────────────────────────────────────────┐
  │              Smile::Renderer                │
  └────────────────────────────────────────────┘
```

- **Viewport nativo:** `ViewportWidget` é um `QWidget` com `WA_NativeWindow` +
  `WA_PaintOnScreen` e `paintEngine() == nullptr` — entrega um `HWND` real ao DX12.
  Inicializa o `Renderer` lazy no primeiro `showEvent`/`paintEvent`; emite
  `RendererInitialized` quando pronto.
- **Render loop:** `QTimer` (`OnRenderTimer`) → coleta input (`HeldKeys`, `MouseDelta`,
  mouse-look) → monta `CameraInput` → `UpdateCamera` + `RenderFrame` → mede FPS → `FrameReady`.
- **Painéis** chamam **setters** do `Renderer` diretamente (não há sistema de
  reflection/property ainda). `MaterialEditorPanel` edita o material ativo + slots de
  textura; `EnvironmentPanel` faz IBL/HDR; `SkyCloudPanel` controla sol/atmosfera/nuvens.
- **Logger → UI:** `Smile::SetLogSink` redireciona `LogInfo/Warning/Error` para o
  `QTextEdit` com cores por nível.
- **Tema:** paleta Fusion dark (estilo VSCode) + `.qss` mínimos, com hot-reload.

---

## 11. Convenções de código

- **Comentários em PT-BR**, identificadores em **inglês**.
- Parâmetros de função em definições usam prefixo `_` (ex.: `_Device`, `_Width`); membros não.
- `F`-prefix para tipos value/engine; sistemas grandes sem prefixo (`Renderer`).
- `static constexpr` para tamanhos de LUT/heap/config em vez de magic numbers.
- `SMILE_HR(expr)` para checar `HRESULT` (lança em falha).
- Tudo em `namespace Smile` (engine) / `SmileEditor` (editor).
- Recursos D3D via `ComPtr<T>` (`Smile::ComPtr` = `Microsoft::WRL::ComPtr`).

---

## 12. Guia de portabilidade

Mapeamento de conceitos de outras engines para onde encaixam na Smile.

### Tabela de equivalências

| Conceito (Unreal / Cry / Flax) | Equivalente Smile | Onde tocar |
|--------------------------------|-------------------|------------|
| RHI / `FRHICommandList` | `FCommandQueue` + `ID3D12GraphicsCommandList` | `Graphics/CommandQueue.*` |
| `FRDGBuilder` / frame graph | **manual** em `Renderer::RenderFrame` (barriers explícitos) | `Graphics/Renderer.cpp` |
| `UMaterial` / material graph | `FMaterial` + `MaterialConstants` + `Triangle.ps` | `Graphics/Material.*`, `Shaders/Triangle.ps.hlsl` |
| Sky Atmosphere component | `FAtmosphere` (já é o modelo Hillaire) | `Graphics/Atmosphere.*` |
| Volumetric Cloud component | `FCloudNoise` + `FVolumetricClouds` | `Graphics/VolumetricClouds.*` |
| Reflection capture / IBL | `FHDREnvironment` | `Graphics/HDREnvironment.*` |
| Post Process Volume | `FPostProcessor` (bloom + ACES) | `Graphics/PostProcess.*` |
| Bindless / descriptor heap | `FTextureSRVHeap` (512 slots, manual) | `Graphics/TextureSRVHeap.*` |
| `SCENE_VIEW` / view uniforms | `FrameConstants` (b0) | `Graphics/Renderer.h` |

### Receita para portar um *novo subsistema de rendering*

Siga o "contrato de subsistema" existente (copie `FAtmosphere`/`FVolumetricClouds` como molde):

1. **Header** com:
   - `struct alignas(256) XxxConstants` casando o `cbuffer` HLSL.
   - `Initialize(device, cmdQueue, srvHeap, rtFormat, dsFormat, ...)`.
   - `UpdatePerFrame(...)` (escreve em `CPUConstants`, copia para o CB mapeado).
   - `Record*Pass(commandList, srvHeap)` (compute) e/ou `Render*(commandList, ...)` (gráfico).
   - `Recreate*(device, rt, ds)` p/ hot-reload de shader + `Resize(...)`.
   - `bool IsInitialized()`.
2. **Recursos:** aloque SRV/UAV no `FTextureSRVHeap` compartilhado; monte tabelas
   contíguas com `CopyDescriptors`. Use `FVolumetricPipeline` para compute parametrizável.
3. **Constant buffer:** upload heap mapeado persistente (`Map` uma vez, escreve por frame).
4. **Shaders:** registre no `Shaders/CMakeLists.txt` (perfil correto) e carregue
   `<nome>.<perfil>.cso`.
5. **Integração no `Renderer`:**
   - membro por valor + flag `UseXxx`;
   - chamar `Xxx.Initialize(...)` em `Renderer::Initialize` (passando formatos HDR
     `R16G16B16A16_FLOAT` / `D32_FLOAT`);
   - `Xxx.UpdatePerFrame(...)` no topo de `RenderFrame`;
   - inserir os `Record*/Render*` na posição certa do frame graph (§5);
   - se tiver PSO gráfico, expor `Recreate*` e registrá-lo na tabela de hot-reload de
     `Renderer::ReloadShaders` (e em `RecreateAllPSOs`); propagar `Resize` em `Resize`.
6. **Editor:** adicione setters no `Renderer` e ligue num painel Qt (modelo dos painéis
   existentes), ou estenda `SkyCloudPanel`.

### Pegadinhas ao portar

- **Tudo é HDR linear** até o tonemap. RTs intermediários devem ser `R16G16B16A16_FLOAT`;
  não escreva sRGB antes do `FinalTonemap`.
- **Sem MSAA:** todo o pipeline é single-sample (`SampleDesc.Count = 1`); o anti-aliasing
  fica por conta do TAA. Não reintroduza `SampleDesc.Count > 1` em PSOs/RTs.
- **Profundidade como SRV:** o depth é `R32_TYPELESS` (DSV `D32` + SRV `R32`). Para passes
  que precisam de depth (oclusão volumétrica), use `Renderer::GetDepthSRVSlot()`.
- **Frame de km da atmosfera/nuvens** é separado das unidades da cena — cuidado ao
  compartilhar matrizes; use `InvViewProjNoTrans` para raios de mundo.
- **1 frame in-flight:** `ExecuteAndSync` faz flush total. Não assuma double-buffering de
  CBs/uploads; é seguro reescrever o mapeado por frame justamente porque há flush.
- **Matrizes LH, row-major**; multiplicação é `Model*View*Proj` (ver `MVP`).

---

## 13. Limitações atuais & dívida técnica

Atalhos **intencionais** da fase de fundação (substituir em iterações futuras):

- **1 frame in-flight** + flush por Present (sem frames-in-flight / paralelismo CPU-GPU).
- **Vertex/Index buffers em upload heap** (sem cópia para default heap).
- **Cena = uma esfera** demo (`CreateSphere`); não há sistema de cena/ECS/entidades ainda.
- **Sem free-list** no `FTextureSRVHeap` (slots vazam ao recriar; 512 slots no total).
- **Sem shadow maps** (sol ilumina PBR + atmosfera, mas sem sombras projetadas).
- **Material binding manual** (sem material graph / parametrização por reflection).
- **Editor → engine via setters diretos** (sem serialização de cena / undo-redo / asset DB).
- **Shader hot-reload** só cobre `Triangle.vs/ps` (a cena), não os demais passes.

> Antes de propor refactors, verificar se o item ainda faz parte desses atalhos
> intencionais — muitos são propositais para manter a base mínima.

---

*Gerado a partir da leitura do código em `Engine/`, `Editor/`, `Shaders/` e do CMake.
Para detalhes de implementação, os headers em `Engine/Include/Smile/Graphics/` são a
fonte de verdade da API de cada subsistema.*
