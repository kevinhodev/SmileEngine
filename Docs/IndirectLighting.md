# Iluminação Indireta — SmileEngine

A SmileEngine possui três sistemas de iluminação indireta complementares:

- **IBL** (Image-Based Lighting) — ambiente estático pré-filtrado a partir de um HDR; difuso + especular via split-sum de Karis.
- **DDGI** (Dynamic Diffuse Global Illumination) — grade de probes com ray tracing inline DXR; iluminação indireta difusa dinâmica de múltiplos bounces.
- **ReSTIR GI** — final-gather por pixel sobre o cache DDGI, com reuso temporal e espacial de reservatórios (Ouyang 2021); denoising opcional via NVIDIA NRD RELAX.

IBL e o ambient atmosférico da engine Hillaire vivem na branch principal. DDGI e ReSTIR GI estão na branch `feature/FFT-Oceanv2`.

---

## Sumário

1. [Visão Geral — IBL](#1-visão-geral--ibl)
2. [Pipeline de Pré-processamento IBL](#2-pipeline-de-pré-processamento-ibl)
3. [Recursos GPU — IBL](#3-recursos-gpu--ibl)
4. [Shaders de Pré-processamento IBL](#4-shaders-de-pré-processamento-ibl)
5. [Aplicação IBL em Runtime (Pixel Shader)](#5-aplicação-ibl-em-runtime-pixel-shader)
6. [Ambient Hemisférico da Atmosfera](#6-ambient-hemisférico-da-atmosfera)
7. [Integração IBL com o Renderer](#7-integração-ibl-com-o-renderer)
8. [Estruturas de Dados — IBL](#8-estruturas-de-dados--ibl)
9. [API Pública — IBL](#9-api-pública--ibl)
10. [Constantes de Configuração — IBL](#10-constantes-de-configuração--ibl)
11. [DDGI — Visão Geral](#11-ddgi--visão-geral)
12. [DDGI — Pipeline e Shaders](#12-ddgi--pipeline-e-shaders)
13. [DDGI — Estruturas e Recursos GPU](#13-ddgi--estruturas-e-recursos-gpu)
14. [DDGI — API e Configuração](#14-ddgi--api-e-configuração)
15. [ReSTIR GI — Visão Geral](#15-restir-gi--visão-geral)
16. [ReSTIR GI — Pipeline e Shaders](#16-restir-gi--pipeline-e-shaders)
17. [ReSTIR GI — Estruturas e Recursos GPU](#17-restir-gi--estruturas-e-recursos-gpu)
18. [ReSTIR GI — API e Configuração](#18-restir-gi--api-e-configuração)
19. [Mapa de Arquivos](#19-mapa-de-arquivos)

---

## 1. Visão Geral — IBL

A iluminação indireta aproxima a contribuição de toda a luz ambiental do ambiente (difusa + especular) sobre uma superfície PBR sem rastrear raios explicitamente. A abordagem usa a **aproximação split-sum**:

```
L_IBL = L_diffuse_IBL + L_specular_IBL

L_diffuse_IBL  = (1 - F) * (1 - metallic) * albedo * IrradianceMap(N)

L_specular_IBL = PrefilteredMap(R, roughness * maxMip)
               * (F * BRDF_LUT(NoV, roughness).r
               +      BRDF_LUT(NoV, roughness).g)
```

Cada term é pré-calculado uma vez ao carregar o arquivo `.hdr`:

| Term | Recurso | Tamanho |
|---|---|---|
| Difuso | `IrradianceCube` | 32³ × 1 mip |
| Especular pré-filtrado | `SpecularCube` | 128³ × 7 mips |
| Integral Fresnel | `BRDFLut` | 128² × 1 mip (RG16F) |

O `BRDFLut` é gerado apenas uma vez na inicialização (independente do arquivo `.hdr`).

---

## 2. Pipeline de Pré-processamento IBL

```
Arquivo .hdr (RGBE / Radiance)
        │
        ▼ stb_image decode → float RGBA
[R32G32B32A32_FLOAT] Equirect2DResource
        │
        ▼ EquirectToCubePSO (EquirectToCube.cs.hlsl)
EnvCube mip 0  [1024³, R16G16B16A16_FLOAT]
        │
        ▼ MipGenPSO  (MipGen.cs.hlsl)  — 1 dispatch por mip
EnvCube mips 1-10  (cadeia completa para correção de ângulo sólido)
        │
        ├──▶ IrradiancePSO (IrradianceConvolution.cs.hlsl)
        │    └─▶ IrradianceCube [32³, 1 mip]
        │
        └──▶ SpecularPSO (SpecularPrefilter.cs.hlsl)  — 1 dispatch por mip
             └─▶ SpecularCube [128³, 7 mips]
                   mip 0 → roughness 0.0
                   mip 6 → roughness 1.0

(Inicialização — executado uma única vez)
        ▼ BRDFLutPSO (BRDFIntegration.cs.hlsl)
BRDFLut [128², RG16F]
```

---

## 3. Recursos GPU — IBL

### EnvCube
- **Resolução:** 1024 × 1024 × 6  
- **Mips:** 11 (cadeia completa: log₂(1024) + 1)  
- **Formato:** `R16G16B16A16_FLOAT`  
- **Uso:** Fonte para os passes de convolution; skybox background  
- **Por que 11 mips?** O shader `SpecularPrefilter` precisa da cadeia completa para calcular o mip bias por ângulo sólido de Karis. Sem ela, amostras de baixa rugosidade concentrariam o disco solar e gerariam fireflies.

### IrradianceCube
- **Resolução:** 32 × 32 × 6  
- **Mips:** 1  
- **Formato:** `R16G16B16A16_FLOAT`  
- **Conteúdo:** Integral do hemisfério ponderada por cosseno (irradiância difusa)  
- **Amostragem no PS:** `IrradianceMap.SampleLevel(s, RotN, 0.0)`

### SpecularCube
- **Resolução:** 128 × 128 × 6  
- **Mips:** 7  
- **Formato:** `R16G16B16A16_FLOAT`  
- **Conteúdo:** Mip 0 = espelho perfeito (roughness 0), Mip 6 = completamente difuso (roughness 1)  
- **Amostragem no PS:** `PrefilteredMap.SampleLevel(s, RotR, roughness * maxMip)`

### BRDFLut
- **Resolução:** 128 × 128  
- **Formato:** `R16G16_FLOAT`  
- **Eixos:** U = NoV (0→1), V = roughness (0→1)  
- **Conteúdo:** R = escala Fresnel (`A`), G = bias Fresnel (`B`)  
- **Amostragem no PS:** `BRDFLut.SampleLevel(s, float2(NoV, roughness), 0.0)`

### Equirect2DResource (temporário)
- **Formato:** `R32G32B32A32_FLOAT` (precisão total para a decodificação RGBE)  
- **Vida útil:** Recriado em cada `LoadFromFile`; mantido vivo até a próxima troca para que o slot SRV não invalide

---

## 4. Shaders de Pré-processamento IBL

Todos os shaders vivem em `Shaders/IBL/`. Compartilham os utilitários matemáticos de `Common.hlsli`.

### 4.1 `Common.hlsli`

Utilitários compartilhados por todos os compute shaders IBL.

**`CubeFaceToDirection(face, uv)`**  
Converte coordenadas texel `(u,v) ∈ [0,1]` de uma face do cubemap para direção world-space, seguindo o layout D3D12 (`+X, -X, +Y, -Y, +Z, -Z`). Inverte Y para que V=0 corresponda ao topo da face.

**`Hammersley(i, N)`**  
Sequência quasi-aleatória de baixa discrepância 2D (Van der Corput na base 2). Usada para gerar amostras distribuídas uniformemente no hemisfério.

```hlsl
float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}
```

**`ImportanceSampleGGX(Xi, N, roughness)`**  
Amostragem por importância GGX: dado um ponto `Xi` no espaço 2D de Hammersley e uma normal `N`, retorna um vetor half-vector `H` com distribuição proporcional ao NDF GGX. Concentra amostras onde a BRDF tem mais energia.

**`D_GGX(NoH, roughness)`**  
NDF de Trowbridge-Reitz (GGX). Usado no cálculo do mip bias de ângulo sólido do prefilter.

**`G_Smith_IBL(NoV, NoL, roughness)`**  
Fator de geometria Smith-Schlick com `k = α² / 2` (forma IBL). Usado na integração do BRDF LUT.

---

### 4.2 `EquirectToCube.cs.hlsl`

**Dispatch:** `⌈size/8⌉ × ⌈size/8⌉ × 6`, grupo `8×8×1`

Converte a textura equiretangular (latitude-longitude) para as 6 faces do cubemap usando:

```hlsl
float2 DirectionToEquirectUV(float3 dir) {
    float u = atan2(dir.z, dir.x) * InvTwoPI + 0.5f;
    float v = 0.5f - asin(dir.y) * InvPI;
    return float2(u, v);
}
```

Cada thread calcula a direção world-space do seu texel via `CubeFaceToDirection`, projeta de volta para UV equiretangular e amostra a textura de origem.

---

### 4.3 `MipGen.cs.hlsl`

**Dispatch:** `⌈size/8⌉ × ⌈size/8⌉ × 6` por nível, grupo `8×8×1`

Gera a cadeia de mips do `EnvCube` via filtragem box 2×2 com `SampleLevel` trilinear. O caller executa um dispatch separado por mip, passando o mip de origem e o de destino via root constants.

A cadeia de mips completa é necessária para que o shader de prefilter especular possa calcular o mip bias correto para cada amostra GGX (correção de ângulo sólido de Karis).

---

### 4.4 `IrradianceConvolution.cs.hlsl`

**Dispatch:** `⌈32/8⌉ × ⌈32/8⌉ × 6`, grupo `8×8×1`

Para cada texel da `IrradianceCube` (direção N), integra numericamente o hemisfério orientado em N usando força bruta (passo `0.025 rad`):

```hlsl
for (float phi = 0; phi < 2π; phi += 0.025) {
    for (float theta = 0; theta < π/2; theta += 0.025) {
        float3 sampleVec = tangent * sin(θ)cos(φ)
                         + bitangent * sin(θ)sin(φ)
                         + N * cos(θ);
        irradiance += EnvCube.SampleLevel(s, sampleVec, 0) * cos(θ) * sin(θ);
    }
}
irradiance = π * irradiance / sampleCount;
```

O peso `cos(θ) * sin(θ)` é o elemento de ângulo sólido para integração em coordenadas esféricas. O fator `π` normaliza para a integral de Lambert.

A resolução 32² é suficiente porque a irradiância difusa é de baixa frequência.

---

### 4.5 `SpecularPrefilter.cs.hlsl`

**Dispatch:** `⌈mipSize/8⌉ × ⌈mipSize/8⌉ × 6` por mip, grupo `8×8×1`

Para cada mip (roughness fixo), amostra 512 direções GGX no hemisfério de N usando `Hammersley + ImportanceSampleGGX` e acumula a média ponderada por `NoL`:

```hlsl
for (uint i = 0; i < NumSamples; ++i) {
    float2 Xi = Hammersley(i, NumSamples);
    float3 H  = ImportanceSampleGGX(Xi, N, Roughness);
    float3 L  = normalize(2 * dot(V, H) * H - V);  // V = N = R (assunção Karis)
    float  NoL = saturate(dot(N, L));
    if (NoL <= 0) continue;

    // Mip bias por ângulo sólido (Krivanek / Karis)
    float D      = D_GGX(dot(N,H), Roughness);
    float pdf    = D * dot(N,H) / max(4 * dot(H,V), 1e-4);
    float saSamp = 1.0 / (NumSamples * max(pdf, 1e-4));
    float saTexel = 4π / (6 * SourceSize²);
    float mip    = 0.5 * log2(saSamp / saTexel);   // 0 para roughness ≈ 0

    // Karis HDR clamp: limita energia para suprimir fireflies do disco solar
    float3 sample = min(EnvCube.SampleLevel(s, L, mip).rgb, 50.0);
    prefiltered += sample * NoL;
    weight      += NoL;
}
prefiltered /= max(weight, 1e-4);
```

**Assunção V=N=R:** simplificação de Karis que permite pré-computar o prefilter sem conhecer V em runtime. Introduce um erro para reflexos de ângulo rasante, mas é padrão na indústria (Unreal Engine 4+).

**Mip bias de ângulo sólido:** evita fireflies ao amostrar mips mais suaves do ambiente quando o pdf é alto (amostras concentradas próximas do pico GGX). Sem esse bias, o disco solar aparece como pontos brilhantes nos mips de baixa rugosidade.

**Karis HDR clamp (`min(sample, 50.0)`):** caps de energia para pixels extremamente brilhantes (disco solar num HDR de pôr do sol). Evita que uma única amostra domine o resultado.

---

### 4.6 `BRDFIntegration.cs.hlsl`

**Dispatch:** `⌈128/8⌉ × ⌈128/8⌉ × 1`, grupo `8×8×1`  
**Executado uma única vez na inicialização** (independe do HDR carregado)

Calcula a LUT 2D `(NoV, roughness)` pré-integrando os termos Fresnel da BRDF especular:

```hlsl
float2 IntegrateBRDF(float NoV, float roughness) {
    float A = 0, B = 0;
    for (uint i = 0; i < NumSamples; ++i) {
        float3 H  = ImportanceSampleGGX(Hammersley(i, N), N, roughness);
        float3 L  = reflect(-V, H);
        float  G     = G_Smith_IBL(NoV, NoL, roughness);
        float  G_Vis = (G * VoH) / (NoH * NoV);
        float  Fc    = pow(1 - VoH, 5);
        A += (1 - Fc) * G_Vis;   // escala Fresnel
        B += Fc * G_Vis;          // bias Fresnel
    }
    return float2(A, B) / NumSamples;
}
```

No pixel shader a LUT é usada como:
```hlsl
float2 BRDF = BRDFLut.SampleLevel(s, float2(NoV, Roughness), 0).rg;
float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y);
```

---

## 5. Aplicação IBL em Runtime (Pixel Shader)

**Arquivo:** `Shaders/Triangle.ps.hlsl`, linhas 437–456

### Bindings de textura IBL

| Slot | Tipo | Recurso |
|---|---|---|
| `t8` | `TextureCube` | `IrradianceMap` (difuso, 32³) |
| `t9` | `TextureCube` | `PrefilteredMap` (especular, 128³ × 7 mips) |
| `t10` | `Texture2D` | `BRDFLut` (128², RG16F) |

### Código de aplicação IBL

```hlsl
if (IBLParams.w > 0.5f) {
    // Rotação Y aplicada à normal e ao vetor de reflexão
    float3 RotN = RotateY(N, IBLParams.y);
    float3 R    = reflect(-V, N);
    float3 RotR = RotateY(R, IBLParams.y);

    // Fresnel com atenuação por roughness (energia conservada)
    float3 F     = F_SchlickRoughness(SpecularColor, NoV, Roughness);
    float3 KdIBL = (1.0 - F) * (1.0 - Metallic);

    // Difuso
    float3 Irradiance = IrradianceMap.SampleLevel(s, RotN, 0.0).rgb;
    float3 DiffuseIBL = KdIBL * DiffuseColor * Irradiance;

    // Especular
    float  Mip         = Roughness * IBLParams.z;        // z = maxMip = 6
    float3 Prefiltered = PrefilteredMap.SampleLevel(s, RotR, Mip).rgb;
    float2 BRDF        = BRDFLut.SampleLevel(s, float2(NoV, Roughness), 0.0).rg;
    float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y);

    // Quando o ambient atmosférico está ativo, só adiciona o especular IBL
    // (o difuso já vem da atmosfera — evita double-counting)
    float3 IBLContrib = UseAtmoAmbient ? SpecularIBL : (DiffuseIBL + SpecularIBL);
    Ambient += IBLContrib * AO * IBLParams.x;
}
```

### `F_SchlickRoughness`

Fresnel-Schlick modificado por roughness. Em vez de interpolar para branco puro a ângulos rasantes, interpola para `max(1 - roughness, F0)`, o que conserva energia em superfícies rugosas:

```hlsl
float3 F_SchlickRoughness(float3 F0, float cosTheta, float roughness) {
    float r1 = 1.0 - roughness;
    return F0 + (max(float3(r1,r1,r1), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}
```

### Rotação do ambiente

A rotação Y é aplicada tanto à normal difusa (`RotN`) quanto ao vetor de reflexão (`RotR`), garantindo que difuso e especular girem juntos com o skybox.

---

## 6. Ambient Hemisférico da Atmosfera

Além do IBL, o sistema suporta **ambient derivado da atmosfera física** (`Shaders/Triangle.ps.hlsl`, linhas 429–435). Quando ativado (`SkyAmbientColor.w > 0.5`):

- O difuso IBL é substituído pelo ambient atmosférico hemisférico
- O especular IBL continua sendo aplicado normalmente (reflexos do ambiente HDR)

```hlsl
bool UseAtmoAmbient = SkyAmbientColor.w > 0.5f;
if (UseAtmoAmbient) {
    float  hemi       = saturate(N.y * 0.5f + 0.5f);  // 0 = nadir, 1 = zenith
    float3 ambientCol = lerp(GroundAmbientColor.rgb, SkyAmbientColor.rgb, hemi);
    Ambient += (1.0 - Metallic) * DiffuseColor * ambientCol * AO * GroundAmbientColor.w;
}
```

O renderer deriva `SkyAmbientColor` e `GroundAmbientColor` em CPU a partir das LUTs da atmosfera de Hillaire. O controle é via `Renderer::SetUseAtmosphereAmbient(bool)`.

**Coexistência IBL + Atmosfera:**

| Estado | Difuso | Especular |
|---|---|---|
| Só IBL | `IrradianceMap` | `PrefilteredMap` + BRDF LUT |
| Só Atmosfera | Hemisférico Hillaire | — |
| Atmosfera + IBL | Hemisférico Hillaire | `PrefilteredMap` + BRDF LUT |

---

## 7. Integração IBL com o Renderer

### Inicialização (`Renderer::Initialize`)

```
Renderer::Initialize
  └─▶ HDREnv.Initialize(Device, CmdQueue, SRVHeap)
        ├─ Aloca EnvCube, IrradianceCube, SpecularCube (FCubeTexture)
        ├─ Aloca BRDFLutResource (R16G16_FLOAT, 128²)
        ├─ Cria os 5 compute PSOs
        ├─ Gera BRDFLut (uma vez)
        └─ Faz upload de um cubemap preto 1×1 para SRVs válidos antes do HDR carregar
  └─▶ Renderer::CreateIBLDescriptorTable
        └─ Aloca 3 slots contíguos no SRVHeap:
             [IBLTableStart + 0] = IrradianceCube SRV  → t8
             [IBLTableStart + 1] = SpecularCube SRV    → t9
             [IBLTableStart + 2] = BRDFLut SRV         → t10
  └─▶ Skybox.Initialize(...)
```

### Carregamento de HDR (`Renderer::LoadHDREnvironment`)

```
LoadHDREnvironment(Path)
  └─▶ HDREnv.LoadFromFile(Device, CmdQueue, SRVHeap, Path)
        ├─ stb_image: decodifica RGBE → float RGBA
        ├─ UploadEquirect2D → Equirect2DResource
        └─▶ RunIBLChain
              ├─ EquirectToCubePSO  → EnvCube mip 0
              ├─ MipGenPSO × 10    → EnvCube mips 1-10
              ├─ IrradiancePSO     → IrradianceCube
              └─ SpecularPSO × 7  → SpecularCube mips 0-6
  └─▶ Renderer::CreateIBLDescriptorTable  (reconstrói a tabela com os novos SRVs)
```

### Render Frame

```
Renderer::RenderFrame
  ├─ Atualiza FrameConstants.IBLParams:
  │    x = IBLIntensity
  │    y = IBLRotation (rad)
  │    z = kSpecularMips - 1  (= 6)
  │    w = HDREnv.HasHDRLoaded() ? 1.0 : 0.0
  ├─ Se UseAtmosphereSky: Atmosphere.Render(...)
  ├─ Se ShowSkybox && HDRLoaded: Skybox.Render(...)  [EnvCube, mip 0]
  ├─ SetGraphicsRootDescriptorTable(3, IBLTableStart)  → liga t8/t9/t10
  └─ DrawIndexedInstanced(...)  → Triangle.ps.hlsl aplica IBL
```

### Layout do SRV Heap

```
SRVHeap
 ├─ ...                    sltos de material (t0-t7)
 ├─ [IBLTableStart + 0]    IrradianceCube  (t8)
 ├─ [IBLTableStart + 1]    SpecularCube    (t9)
 ├─ [IBLTableStart + 2]    BRDFLut         (t10)
 ├─ DepthSRVSlot           Profundidade (R32_FLOAT)
 └─ HDRSRVSlot             Buffer HDR pós-processamento
```

---

## 8. Estruturas de Dados — IBL

### `FrameConstants` (`Engine/Include/Smile/Graphics/Renderer.h`)

```cpp
struct alignas(256) FrameConstants {
    Mat44 MVP;                 // 64 bytes — Model-View-Projection
    Mat44 ModelMatrix;         // 64 bytes — para TBN e iluminação
    Vec4  CameraPosition;      // 16 bytes — posição da câmera world-space
    Vec4  IBLParams;           // 16 bytes — x=intensidade, y=rotação(rad), z=maxMip, w=ativado
    Vec4  Time;                // 16 bytes — x=segundos, y=delta, z=frameIndex
    Vec4  SunDirection;        // 16 bytes — xyz=direção ao sol, w=intensidade
    Vec4  SunColor;            // 16 bytes — rgb=cor do sol
    Vec4  SkyAmbientColor;     // 16 bytes — rgb=ambient zenith, w=ativado(0/1)
    Vec4  GroundAmbientColor;  // 16 bytes — rgb=ambient nadir, w=intensidade
};                             // Total: 240 bytes de 256
```

### `FHDREnvironment` (`Engine/Include/Smile/Graphics/HDREnvironment.h`)

```cpp
class FHDREnvironment {
    // Constantes de configuração
    static constexpr u32 kEnvCubeSize         = 1024;
    static constexpr u32 kEnvCubeMips         = 11;
    static constexpr u32 kIrradianceSize      = 32;
    static constexpr u32 kSpecularSize        = 128;
    static constexpr u32 kSpecularMips        = 7;
    static constexpr u32 kBRDFLutSize         = 128;
    static constexpr u32 kSpecularSampleCount = 512;

    // Recursos GPU
    FCubeTexture EnvCube;          // 1024², 11 mips
    FCubeTexture IrradianceCube;   // 32², 1 mip
    FCubeTexture SpecularCube;     // 128², 7 mips

    ComPtr<ID3D12Resource> BRDFLutResource;     // 128², RG16F
    ComPtr<ID3D12Resource> Equirect2DResource;  // temporário, RGBA32F

    // Compute PSOs
    FComputePipeline EquirectToCubePSO;
    FComputePipeline MipGenPSO;
    FComputePipeline IrradiancePSO;
    FComputePipeline SpecularPSO;
    FComputePipeline BRDFLutPSO;
};
```

### `FCubeTexture` (`Engine/Include/Smile/Graphics/CubeTexture.h`)

```cpp
class FCubeTexture {
    ID3D12Resource*            GpuResource;
    u32                        FaceSRVSlot;      // SRV para toda a cadeia de mips
    std::vector<u32>           UAVSlots;         // 1 UAV por mip (para escrita compute)
    u32                        TexSize;
    u32                        MipLevels;
    std::vector<D3D12_RESOURCE_STATES> MipStates; // [mip * 6 + face]
};
```

O rastreamento por sub-recurso (`MipStates`) permite barreiras precisas no pipeline de geração de mips e prefilter, evitando transições desnecessárias de estado.

---

## 9. API Pública — IBL

### `Renderer` (`Engine/Include/Smile/Graphics/Renderer.h`)

```cpp
// Carrega um arquivo .hdr e executa toda a cadeia IBL.
// Retorna false em falha de I/O (loga o motivo).
bool LoadHDREnvironment(const std::wstring& Path);

// Escala a contribuição total IBL (difuso + especular).
void SetIBLIntensity(f32 Intensity);    // padrão: 1.0
f32  GetIBLIntensity() const;

// Rotação em torno do eixo Y, aplicada ao ambiente em runtime.
void SetIBLRotation(f32 Radians);       // padrão: 0.0
f32  GetIBLRotation() const;

// Exibe/oculta o skybox (EnvCube como fundo).
void SetShowSkybox(bool Show);          // padrão: true
bool GetShowSkybox() const;

// Troca entre ambient atmosférico e IBL difuso.
void SetUseAtmosphereAmbient(bool Use); // padrão: true
bool GetUseAtmosphereAmbient() const;
void SetAtmosphereAmbientIntensity(f32 I);
```

### `FHDREnvironment` (`Engine/Include/Smile/Graphics/HDREnvironment.h`)

```cpp
void Initialize(ID3D12Device*, FCommandQueue&, FTextureSRVHeap&);
bool LoadFromFile(ID3D12Device*, FCommandQueue&, FTextureSRVHeap&,
                  const std::wstring& Path);

u32  EnvCubeSRV()    const;  // para o skybox
u32  IrradianceSRV() const;
u32  SpecularSRV()   const;
u32  BRDFLutSRV()    const;
bool HasHDRLoaded()  const;
```

---

## 10. Constantes de Configuração — IBL

Definidas em `Engine/Include/Smile/Graphics/HDREnvironment.h`:

| Constante | Valor | Impacto ao alterar |
|---|---|---|
| `kEnvCubeSize` | 1024 | Aumentar melhora qualidade do prefilter; dobra memória GPU por octeto |
| `kEnvCubeMips` | 11 | Deve ser `log2(kEnvCubeSize) + 1`; alterar junto com o tamanho |
| `kIrradianceSize` | 32 | 32 é suficiente (sinal de baixa frequência); 64 é imperceptível |
| `kSpecularSize` | 128 | 256 melhora reflexos nítidos em superfícies polidas |
| `kSpecularMips` | 7 | Mais mips = mais níveis de roughness; `kSpecularSize` deve ser ≥ 2^(kSpecularMips-1) |
| `kBRDFLutSize` | 128 | Raramente precisa mudar; 256 reduz banding em NoV muito baixo |
| `kSpecularSampleCount` | 512 | Mais amostras = menos ruído no prefilter; impacta tempo de geração |

---

---

## 11. DDGI — Visão Geral

DDGI (Dynamic Diffuse Global Illumination) implementa uma grade 3D de **probes de irradiância** que propagam luz indireta difusa em tempo real usando **DXR Inline Ray Tracing**. Cada probe rastreia 64 raios por frame, acumula a radiância em um atlas octahedral e disponibiliza a irradiância para o pixel shader via interpolação trilinear.

### Fluxo por frame

```
DDGI::RecordUpdate
  ├─ DDGITrace     — 64 raios / probe (1 thread / raio); grava ProbesTrace
  ├─ DDGIUpdate    — acumula irradiância no IrradAtlas (6×6 px / probe)
  ├─ DDGIUpdateDist— acumula distância média + variância no DistAtlas (14×14 px / probe)
  └─ DDGIRelocate  — reposiciona probes ocultos; ajusta ray count adaptativo
```

### Características técnicas

- **Distribuição de raios:** Sequência de Fibonacci esférica rotacionada aleatoriamente por frame (`DDGI_RandomRotation`). A rotação garante que amostras acumuladas ao longo dos frames cubram o hemisfério uniformemente sem padrões fixos.
- **Real hit shading:** Ao acertar uma superfície, o shader traça um raio de sombra em direção ao sol e amostra o `IrradAtlas` para iluminação indireta do bounce — iluminação multi-bounce real em vez de simples albedo.
- **Chebyshev visibility:** O atlas de distância armazena `(mean, mean²)` por probe. Na amostragem, o teste de Chebyshev pondera a contribuição de cada probe pela probabilidade de visibilidade, eliminando bleeding de luz através de paredes.
- **Relocalização de probes:** Probes dentro de geometria detectam via razão backface > 25% e se deslocam até `spacing × 0.45` em direção à superfície visível mais próxima. Probes irreparavelmente ocultos (backface > `DeactivationThreshold`) são marcados como inativos (`ProbeData.w < 0`).
- **Ray count adaptativo:** Probes próximos a geometria recebem mais raios (até 256); probes em espaço aberto recebem menos (mínimo 8), economizando orçamento de trace.

---

## 12. DDGI — Pipeline e Shaders

### 12.1 `DDGICommon.hlsli`

Utilitários compartilhados por todos os shaders DDGI e pelo ReSTIR GI.

**`DDGI_SphericalFibonacci(i, n)`**
Gera a direção `i` de uma sequência de Fibonacci esférica com `n` amostras. Produz distribuição quasi-uniforme sobre a esfera sem clustering nos polos.

**`DDGI_RandomRotation(frame)`**
Constrói uma rotação 3D pseudo-aleatória por hash do índice de frame (3 ângulos independentes). Aplicada ao banco de raios Fibonacci a cada frame para convergência ao longo do tempo.

**`DDGI_OctEncode / DDGI_OctDecode`**
Codificação octahedral de normais: mapeia `float3` normalizado para `float2 ∈ [-1,1]²`. Usada como mapeamento UV para amostrar as tiles do atlas de irradiância e distância.

**`SampleDDGIIrradiance`** e **`SampleDDGIIrradianceCheb`**
Interpolação trilinear nos 8 probes vizinhos da grade. A versão Cheb adiciona:
- Peso pelo fator de visibilidade de Chebyshev (distância média vs. variância)
- Peso por `backface` para penalizar probes atrás da superfície
- Skip/fallback de probes inativos (`ProbeData.w < 0`)

**`DDGI_TileOrigin(coord, count, tile)`**
Calcula o offset em pixels da tile de um probe no atlas plano 2D. O atlas empacota os probes como `tileCol = x + z * countX`, `tileRow = y`.

---

### 12.2 `DDGITrace.cs.hlsl`

**Dispatch:** `numProbes × 1 × 1`, grupo `64×1×1` (1 grupo por probe, 1 thread por raio)

Cada thread traça 1 raio usando `RayQuery` (DXR Inline):

```hlsl
// Ray count adaptativo: threads "ociosas" gravam DDGI_RAY_UNUSED e saem.
int stride = DDGI_RAYS / max(rayCount, 1);
if ((rayIdx % stride) != 0) { ProbesTrace[...] = float4(0,0,0,DDGI_RAY_UNUSED); return; }

// Posição do probe com offset de relocalização aplicado.
float3 probePos = DDGI_ProbeWorldPos(pc, ...) + ProbeData[probeIdx].xyz;

// Direção Fibonacci rotacionada pelo frame.
float3 dir = DDGI_RayDirection(rayIdx, DDGI_RAYS, frameIndex);

// Trace inline.
RayQuery<RAY_FLAG_NONE> q;
q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
while (q.Proceed()) {}

// Hit → ShadeSurfaceHit (sol + DDGI bounce); Miss → ShadeSky.
ProbesTrace[int2(rayIdx, probeIdx)] = float4(radiance, signedDist);
// signedDist < 0 = backface hit (usado pelo Relocate e Update).
```

**`HitShading.hlsli` — `ShadeSurfaceHit`**
Shading completo no ponto de hit do raio de probe:
1. Interpola normal e UV da malha (buffer de vértices/índices via `InstanceGeo`)
2. Amostra textura de albedo com LOD fixo (`AlbedoLOD = 4`)
3. Traça raio de sombra para o sol (`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`)
4. Amostra `IrradAtlas` para iluminação indireta do bounce atual

Resultado: `albedo * (Edirect/π + indirect)` — a equação do renderizador difuso de 1 bounce.

---

### 12.3 `DDGIUpdate.cs.hlsl`

**Dispatch:** `numProbes × 1 × 1`, grupo `6×6×1` (1 grupo por probe, 1 thread por texel da tile)

Cada thread processa um texel da tile octahedral de irradiância (`6×6 px`):

```hlsl
// Decodifica a direção do texel.
float3 texelDir = DDGI_OctDecode(octUV * 2 - 1);

// Acumula raios com peso cosseno (dot produto com texelDir).
for (int r = 0; r < 64; ++r) {
    float w = max(0, dot(texelDir, DDGI_RayDirection(r, 64, frame)));
    sum += ProbesTrace[r, probe].rgb * w;
    wsum += w;
}

// Detecção de oclusão: probe dentro de geometria → apaga irradiância.
bool occluded = backfaceCount > realCount * 0.35;

// Blend temporal (hysteresis = 0.99 padrão → convergência lenta = estável).
blended = lerp(newResult, prev, hysteresis);
```

A irradiância é armazenada em gamma `DDGI_IRRADIANCE_GAMMA = 1.5` para comprimir a faixa dinâmica no atlas e expandir na amostragem. O valor 1.5 (entre linear e sRGB) é um trade-off entre precisão em sombras e qualidade em luzes brilhantes.

---

### 12.4 `DDGIUpdateDist.cs.hlsl`

**Dispatch:** `numProbes × 1 × 1`, grupo `14×14×1`

Mesma lógica do Update, mas armazena `(mean_dist, mean_dist²)` por texel da tile de distância (`14×14 px`). O peso angular usa `DDGI_DIST_SHARP = 50.0` (elevado a 50ª potência) para concentrar contribuição nos raios quase alinhados com o texelDir, produzindo um octmap de distância mais nítido.

Os valores `(μ, μ²)` permitem calcular a variância `σ² = μ² - μ²` para o teste de Chebyshev na amostragem.

---

### 12.5 `DDGIRelocate.cs.hlsl`

**Dispatch:** `⌈numProbes/64⌉ × 1 × 1`, grupo `64×1×1`

Por probe, analisa os raios do `ProbesTrace` do frame atual:

```
backRatio = backfaceCount / realCount

Se backRatio > 25%:
    move probe em direção ao hit front-face mais distante (sai da geometria)
Senão se closestFront < spacing * 0.30:
    recua do obstáculo próximo

Offset clampado a spacing * 0.45 (nunca ultrapassa a célula da grade).
Suavizado: lerp(offset, target, 0.25) por frame.

Inativação: backRatio > DeactivationThreshold && backfaceCount >= 6
    → ProbeData.w = -1 (sinaliza skip nas amostragens).

Ray count adaptativo:
    closestFront / spacing < 0.5  → 256 raios
    < 1.0                         → 128
    < 2.0                         →  64
    < 4.0                         →  32
    < 8.0                         →  16
    senão                         →   8
```

---

## 13. DDGI — Estruturas e Recursos GPU

### `DDGIConstants` (`Engine/Include/Smile/Graphics/DDGI.h`)

```cpp
struct alignas(256) DDGIConstants {
    Vec4 GridMinSpacing;  // xyz = origem do grid (world), w = espaçamento entre probes
    Vec4 GridCountRays;   // xyz = nº de probes por eixo (X,Y,Z), w = raios por probe
    Vec4 AtlasParams;     // x = tile size (6), y = atlasW, z = atlasH, w = numProbes total
    Vec4 SunDirIntensity; // xyz = direção ao sol (normalizado), w = intensidade
    Vec4 SunColorHyst;    // rgb = cor do sol, w = hysteresis (blend temporal, 0=instante/1=nunca)
    Vec4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = normalBias
    Vec4 DistAtlasParams; // x = dist tile size (14), y = dist atlasW, z = dist atlasH, w = realHitShading
    Vec4 MiscParams;      // x = relocationEnabled, y = deactivationThreshold, z = maxRays, w = minRays
};
```

### Recursos GPU

| Recurso | Tipo | Formato | Tamanho | Propósito |
|---|---|---|---|---|
| `IrradAtlas` | Texture2D | R16G16B16A16_FLOAT | `(countX*countZ*6) × (countY*6)` | Irradiância por probe (tile octahedral 6×6) |
| `DistAtlas` | Texture2D | R16G16_FLOAT | `(countX*countZ*14) × (countY*14)` | Distância média+variância (tile 14×14) |
| `ProbesTrace` | Texture2D | R16G16B16A16_FLOAT | `64 × numProbes` | Buffer de raios do frame atual (rgb=radiance, a=signedDist) |
| `ProbeDataBuf` | Buffer\<float4\> | R32G32B32A32_FLOAT | `numProbes` | xyz=offset de relocalização, w=status (≥0=ativo, <0=inativo) |
| `ProbeRayCountBuf` | Buffer\<uint\> | R32_UINT | `numProbes` | Ray count adaptativo por probe (8–256) |
| `MergedVertexBuf` | StructuredBuffer\<DDGIVertex\> | — | todos os meshes | Vértices fundidos da cena para hit shading |
| `MergedIndexBuf` | Buffer\<uint\> | R32_UINT | todos os tris | Índices fundidos |
| `InstanceGeoBuf` | StructuredBuffer\<InstanceGeo\> | — | num meshes | Metadados por instância (albedo, base vertex/index, etc.) |

### `InstanceGeo` e `DDGIVertex`

```hlsl
struct InstanceGeo {
    float4 BaseColor;   // cor base da instância
    uint   VertexBase;  // offset no MergedVertexBuf
    uint   IndexBase;   // offset no MergedIndexBuf
    uint   AlbedoIndex; // índice no ResourceDescriptorHeap (bindless)
    uint   HasAlbedo;   // se 0, usa só BaseColor
    uint   TwoSided;    // se 1, não inverte normal em backfaces
};

struct DDGIVertex {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
};
```

### Layout do Atlas

O atlas é um texture 2D plano com tiles de probes organizados por `(X + Z*countX, Y)`:

```
tileCol = probeX + probeZ * countX
tileRow = probeY

Pixel de um texel (oc) na tile do probe (x,y,z):
  px = tileCol * tileSize + oc.x
  py = tileRow * tileSize + oc.y
```

---

## 14. DDGI — API e Configuração

### API (`Engine/Include/Smile/Graphics/DDGI.h`)

```cpp
// Configura a grade a partir do AABB da cena. Calcula spacing, countXYZ e aloca os atlases.
void SetupForScene(Device, Queue, SRVHeap, Scene, AABBMin, AABBMax,
                   TlasSRVSlot, SkyViewSRVSlot);

// Atualiza o constant buffer antes de RecordUpdate.
void UpdatePerFrame(FrameSlot, DirToSun, SunIntensity, SunColor, FrameIndex);

// Grava os 4 compute passes no command list.
void RecordUpdate(CommandList, SRVHeap);

// Parâmetros de qualidade
void SetIntensity(f32);              // escala global da irradiância DDGI
void SetHysteresis(f32);            // blend temporal (padrão: 0.99)
void SetRealHitShading(bool);       // shading completo nos hits vs. albedo puro
void SetRelocation(bool);           // liga/desliga relocalização de probes
void SetDeactivationThreshold(f32); // razão backface para desativar probe (padrão: 0.20)
void SetAdaptiveRays(bool);         // ray count adaptativo por probe
void SetMaxRays(int);               // teto de raios quando adaptativo (padrão: 64)
void SetMinRays(int);               // piso de raios quando adaptativo (padrão: 16)
```

### Constantes fixas

| Constante | Valor | Significado |
|---|---|---|
| `kRaysPerProbe` | 64 | Banco de raios Fibonacci; o adaptativo usa subconjuntos |
| `kTileSize` | 6 | Resolução da tile de irradiância (px por probe) |
| `kDistTileSize` | 14 | Resolução da tile de distância (px por probe) |
| `kRelocateConvergeFrames` | 180 | Frames de relocalização após `SetRelocation(true)` |
| `kReclassifyFrames` | 6 | Frames de re-classificação após mudança de parâmetro |

---

## 15. ReSTIR GI — Para Leigos

Antes de entrar no código, esta seção explica o problema e a solução em linguagem simples.

### O problema: luz que ricocheteou

Quando a luz do sol entra por uma janela e ilumina o chão, parte dessa luz bate no chão e vai colorir as paredes próximas. Essa luz "de segundo bounce" é o que faz a cena parecer real — sem ela, as sombras ficam completamente negras e tudo parece render de videogame dos anos 90.

Para calcular isso corretamente, você precisaria rastrear cada raio de luz por todas as suas reflexões — o que é o que o path tracing faz. O problema: uma GPU rápida consegue lançar uns poucos raios por pixel por frame e ainda manter 60 FPS. Raios demais = imagem granulada (ruidosa). Poucos raios = ruído inaceitável.

### A sacada do ReSTIR: reutilizar o que os outros já descobriram

Imagine que você está tentando descobrir qual é o melhor restaurante da cidade. Você poderia visitar todos aleatoriamente — mas isso levaria uma vida. Ou você poderia **perguntar para os seus vizinhos** o que eles descobriram, e usar a resposta deles somada à sua própria pesquisa.

ReSTIR faz exatamente isso com amostras de luz:

- Cada pixel lança **apenas 1 raio** por frame e encontra uma fonte de luz indireta.
- Em vez de descartar isso após o frame, guarda num **reservatório** — uma memória compacta da "melhor amostra que vi até agora".
- No próximo frame, **herda** o reservatório do mesmo pixel no frame anterior (via motion vector), adicionando a nova amostra ao histórico.
- Além disso, **rouba** os reservatórios de 4 pixels vizinhos próximos, combinando todas as descobertas.

O resultado: cada pixel se comporta como se tivesse lançado dezenas de raios, mas o custo real é de 1–2 raios por pixel.

### O reservatório: o coração do sistema

Um reservatório guarda 6 campos:

```
x1  — onde EU estou no mundo (posição do pixel)
x2  — onde a luz indireta está vindo (o ponto iluminado que descobri)
n2  — normal da superfície em x2 (preciso para o Jacobiano)
Lo  — quanta luz está saindo de x2 em direção a x1
M   — quantas amostras contribuíram para este reservatório (histórico)
W   — peso final: quão "boa" é esta amostra para este pixel
```

O truque matemático é o **WRS (Weighted Reservoir Sampling)**: ao adicionar uma nova amostra, você faz uma aposta proporcional ao peso da nova candidata. Se ela ganhar, substitui a seleção atual. Isso garante que a seleção final seja sempre proporcional à distribuição de importância desejada — sem precisar guardar todas as amostras.

### O Jacobiano de reconexão: o problema de roubar vizinhos

Quando o pixel A quer usar a amostra do vizinho B, há um problema sutil: a amostra foi descoberta a partir de um ângulo diferente. Se B está a 1 metro de A no espaço da tela, mas o ponto iluminado `x2` está a 5 metros de distância, o ângulo que A vê `x2` é muito parecido com o que B vê — tudo ok. Mas se `x2` está logo atrás da parede, o ângulo muda drasticamente — a amostra vale muito menos para A do que valia para B.

O **Jacobiano de reconexão** é um fator de correção que ajusta o peso da amostra para compensar essa diferença de ângulo/distância:

```
J = (cos do ângulo de chegada em A / cos do ângulo de chegada em B)
  * (distância ao quadrado de B até x2 / distância ao quadrado de A até x2)
```

Se `J ≈ 1`, a amostra transfere bem. Se `J >> 1` ou `J << 1`, a geometria é muito diferente e a amostra não é boa candidata.

### Como o DDGI entra aqui

O ReSTIR GI usa o DDGI como um **cache de radiância multi-bounce**. Quando o raio do ReSTIR acerta uma superfície em `x2`, em vez de parar ali, o shader de shading no hit consulta o atlas de irradiância do DDGI para ter a luz indireta que já chegou em `x2` de bounces anteriores. Assim:

- **DDGI:** resolve bounces de baixa frequência de forma estável (grade de probes, atualiza a 64 raios/probe/frame)
- **ReSTIR GI:** resolve o first-gather por pixel com alta frequência e precisão espacial, usando o DDGI como base

---

## 16. ReSTIR GI — Visão Geral Técnica

### Pipeline completo

```
[GPU, todo frame]

Pass A — ReSTIRGITrace.cs.hlsl         (1 raio/pixel, 64 threads por tile 8×8)
  ├─ Trace 1 raio cosseno-hemisférico → hit em x2
  ├─ ShadeSurfaceHit: sol + DDGI bounce em x2 → Lo (radiância)
  ├─ Cria reservatório inicial {x1, x2, n2, Lo, M=1, W}
  ├─ Reuso temporal: funde com Res[frame-1] via motion vector
  └─ Grava Res[frame_atual] em 4 texturas ping-pong + GIOut (fallback)

Pass B — ReSTIRGISpatial.cs.hlsl       (sem raios extras, 64 threads por tile 8×8)
  ├─ Lê Res[frame_atual] do Pass A
  ├─ Funde k=4 vizinhos (raio 16px) com Jacobiano de reconexão
  ├─ Visibility ray opcional: testa oclusão de x1→x2 selecionado
  └─ Resolve gi = Lo * cos(θ) * W / π → sobrescreve GIOut

Fase C — ReSTIRNrdPack.cs.hlsl + NRD RELAX (opcional, UseNrd=false por padrão)
  ├─ Empacota GITexture + GBuffer + depth + velocity para NRD
  └─ NRD RELAX denoise → NrdOutSRV

Deferred Lighting — DeferredLighting.ps.hlsl
  └─ Lê GITexture (ou NrdOutSRV) em t16 → Kd * albedo * gi * AO
```

### Estrutura do reservatório

```cpp
// ReSTIRReservoir.hlsli
struct Reservoir {
    float3 x1;    // ponto visível (posição do pixel no mundo)
    float3 x2;    // ponto da amostra (hit do raio secundário)
    float3 n2;    // normal geométrica em x2 (para o Jacobiano de reconexão)
    float3 Lo;    // radiância em x2 na direção de x1
    float  M;     // contagem de amostras (histórico; clampado em MCap=20)
    float  W;     // peso não-enviesado = wSum / (M * pHat(x2 selecionado))
    float  wSum;  // acumulador interno (não persiste entre passes)
};
```

### Mapeamento nas 4 texturas ping-pong

| Textura | Formato | Conteúdo |
|---|---|---|
| `ResA[p]` | R32G32B32A32_FLOAT | `x1.xyz`, `M` |
| `ResB[p]` | R32G32B32A32_FLOAT | `x2.xyz`, `W` |
| `ResC[p]` | R16G16B16A16_FLOAT | `Lo.rgb` |
| `ResD[p]` | R16G16B16A16_FLOAT | `n2.xyz` |

`p = FrameParity = FrameIndex & 1`. Pass A lê `Res*[1-p]` (frame anterior) e escreve `Res*[p]`.

---

## 17. ReSTIR GI — Matemática e Shaders em Detalhe

### 17.1 `ReSTIRReservoir.hlsli` — A Fundação Matemática

#### PDF alvo: `TargetPHat`

O WRS precisa de uma função de importância para decidir quais amostras são valiosas. A PDF alvo do ReSTIR GI é:

```hlsl
float TargetPHat(float3 x1, float3 n1, float3 x2, float3 Lo) {
    float3 d = x2 - x1;
    float  l = length(d);
    float cosT = saturate(dot(n1, d / l));   // quanto x2 contribui para x1
    return luminance(Lo) * cosT;
}
```

Essa função é proporcional à irradiância esperada em `x1` vinda de `x2`: quanto mais brilhante `Lo` e mais alinhado com a normal, mais valiosa. O albedo *não* entra aqui — cancela na resolução final.

#### Inserção WRS: `ResUpdate`

```hlsl
void ResUpdate(inout Reservoir r, float3 x2, float3 n2, float3 Lo, float w, inout uint rng) {
    r.wSum += w;
    r.M    += 1.0;
    // A probabilidade de selecionar este candidato = w / wSum
    if (w > 0.0 && RngNext(rng) * r.wSum <= w)
        { r.x2 = x2; r.n2 = n2; r.Lo = Lo; }
}
```

Invariante matemático: após processar N candidatos, a probabilidade de qualquer candidato `i` estar selecionado é `w_i / sum(w_0..N-1)`. Isso é exatamente amostragem por importância sem precisar guardar todos os candidatos.

#### Fusão de reservatórios: `ResMerge`

Para combinar dois reservatórios (temporal ou espacial), convertemos o reservatório `other` em um candidato com peso equivalente:

```hlsl
void ResMerge(inout Reservoir r, Reservoir other, float pHatOther, float J, inout uint rng) {
    // Peso efetivo = pHat do candidato (no pixel atual) * W_other * M_other * J
    float w = pHatOther * other.W * other.M * J;
    r.wSum += w;
    r.M    += other.M;
    if (w > 0.0 && RngNext(rng) * r.wSum <= w)
        { r.x2 = other.x2; r.n2 = other.n2; r.Lo = other.Lo; }
}
```

O produto `other.W * other.M` reconstrói o `wSum` original de `other` de forma não-enviesada: `W = wSum / (M * pHat)` → `wSum = W * M * pHat`. Multiplicar pelo `pHat` do pixel atual (em vez do original) é o que habilita o reuso cross-pixel.

#### Jacobiano de reconexão: `ReconnectionJacobian`

O Jacobiano corrige a mudança de variável quando reutilizamos a amostra `x2` de um pixel `x1Src` em outro pixel `x1Dst`:

```hlsl
float ReconnectionJacobian(float3 x1Dst, float3 x1Src, float3 x2, float3 n2) {
    float3 dDst = x2 - x1Dst; float lDst2 = dot(dDst, dDst);
    float3 dSrc = x2 - x1Src; float lSrc2 = dot(dSrc, dSrc);
    float lDst = sqrt(lDst2), lSrc = sqrt(lSrc2);
    float cosDst = abs(dot(n2, -dDst / lDst));  // cos do ângulo de chegada em Dst
    float cosSrc = abs(dot(n2, -dSrc / lSrc));  // cos do ângulo de chegada em Src
    return (cosDst / cosSrc) * (lSrc2 / lDst2);
}
```

**Intuição:** A amostra foi gerada com probabilidade proporcional ao ângulo sólido de `x1Src`. Ao usá-la em `x1Dst`, o ângulo sólido mudou. O Jacobiano corrige essa diferença para manter o estimador não-enviesado. Clampado em `[0.1, 10]` para evitar divergência numérica.

No reuso **temporal**, `J = 1.0` porque assumimos que a superfície não se moveu entre frames — `x1Src ≈ x1Dst` após reprojeção.

#### Finalização do peso: `ResFinalizeW`

```hlsl
void ResFinalizeW(inout Reservoir r, float3 x1, float3 n1) {
    float pHatSel = TargetPHat(x1, n1, r.x2, r.Lo);
    r.W = (pHatSel > 0.0 && r.M > 0.0) ? (r.wSum / (r.M * pHatSel)) : 0.0;
}
```

`W` é o peso de Monte Carlo não-enviesado. Ele garante que `E[Lo * cosT * W] = E[Lo * cosT / pdf_real]` — o estimador converge para o valor correto independente de quais amostras foram fundidas.

#### Resolução da irradiância: `ResResolve`

```hlsl
float3 ResResolve(Reservoir r, float3 x1, float3 n1, float maxLuma) {
    float3 d    = r.x2 - x1;
    float  cosT = saturate(dot(n1, normalize(d)));
    float3 gi   = r.Lo * cosT * r.W / PI;  // = (1/π) * E
    // Firefly clamp na saída (não em Lo — preserva a cor, só limita o brilho)
    float gl = luminance(gi);
    if (maxLuma > 0.0 && gl > maxLuma) gi *= maxLuma / gl;
    return gi;
}
```

O fator `1/π` vem da integral da BRDF Lambertiana: `f_d = albedo/π`, então `L_o = f_d * E = (albedo/π) * E`. Como o albedo é aplicado no deferred (`KdGI * DiffuseColor * gi`), o `gi` aqui é `E/π` sem o albedo.

---

### 17.2 `ReSTIRGITrace.cs.hlsl` — Pass A: Trace + Temporal

**Dispatch:** `⌈W/8⌉ × ⌈H/8⌉ × 1`, grupo `8×8×1`

#### Passo 1: Reconstrução de posição no mundo

```hlsl
float deviceZ = Depth.Load(int3(px, 0)).r;
float4 wH     = mul(float4(ndc, deviceZ, 1.0), InvViewProj);
float3 x1     = wH.xyz / wH.w;          // posição world-space do pixel
float3 N      = DDGI_OctDecode(gb.rg * 2 - 1); // normal do GBuffer
```

#### Passo 2: Amostragem cosseno-hemisférica (método de Malley)

```hlsl
float2 E   = GGX_Rand2(px, frameIndex);   // 2D pseudo-aleatório por pixel+frame
float  rr  = sqrt(E.x);                   // raio no disco (CDF invertida)
float  phi = 2π * E.y;                    // azimute uniforme
float  cosT = sqrt(1.0 - E.x);           // cos(theta) = sqrt(1-r²)
float3 d   = float3(rr*cos(φ), rr*sin(φ), cosT); // tangent space
float3 dir = normalize(mul(d, TangentBasis(N)));  // world space
```

A distribuição resultante é proporcional a `cos(θ)` — a PDF é `cos(θ)/π`. Isso significa que raios mais próximos da normal têm mais probabilidade de ser lançados, o que é ótimo para difuso Lambertiano.

#### Passo 3: Ray trace + shading

```hlsl
ray.Origin    = x1 + N * normalBias;       // offset para evitar self-intersection
ray.Direction = dir;
ray.TMax      = maxRayDist;
RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
q.TraceRayInline(Scene, ...);

if (hit) {
    Lo = ShadeSurfaceHit(...);  // sol + DDGI bounce no hit (HitShading.hlsli)
    n2 = HitGeomNormal(...);    // normal geométrica para o Jacobiano
    x2 = origin + dir * hitDist;
} else {
    Lo = ShadeSky(dir, ...);    // sky view LUT da atmosfera
    x2 = origin + dir * maxRayDist;
    n2 = -dir;
}
```

**Firefly clamp** antes de criar o reservatório:
```hlsl
float lum = luminance(Lo);
if (fireflyMax > 0 && lum > fireflyMax) Lo *= fireflyMax / lum;
```
Isso previne que amostras de luz muito brilhante (janelas, lâmpadas) dominem o estimador e criem pontos brilhantes estocásticos. O clamp é aplicado em `Lo` (não na saída) para preservar a cor.

#### Passo 4: Criação do reservatório inicial

```hlsl
float pHat = TargetPHat(x1, N, x2, Lo);
float pSrc = max(cosT, 1e-4) / PI;         // PDF da amostragem cosseno-hemisférica
float wInit = (pSrc > 0) ? (pHat / pSrc) : 0; // peso de resampling
ResUpdate(r, x2, n2, Lo, wInit, rng);
```

O peso `pHat / pSrc` é o peso de resampling clássico: quanto melhor a amostra é para este pixel relativo à distribuição com que foi gerada, maior o peso.

#### Passo 5: Reuso temporal

```hlsl
float2 vel    = Velocity.Load(int3(px, 0)).rg; // motion vector
float2 prevUv = uv - vel;
if (all(prevUv > 0) && all(prevUv < 1)) {
    int2 ppx = int2(prevUv * screenSize);
    // Lê o reservatório do pixel correspondente no frame anterior
    Reservoir prev = { PrevResA[ppx].xyz, PrevResB[ppx].xyz, PrevResD[ppx].xyz,
                       PrevResC[ppx].rgb, min(PrevResA[ppx].w, MCap), PrevResB[ppx].w };

    float camDist   = length(cameraPos - x1);
    float posReject = posRejectScale * max(camDist, 1.0); // tolerância proporcional à distância
    if (length(prev.x1 - x1) < posReject)  // aceita se a posição reprojetada está próxima
        ResMerge(r, prev, TargetPHat(x1, N, prev.x2, prev.Lo), J=1.0, rng);
}
```

`MCap = 20` limita quantas amostras históricas podem ser acumuladas. Sem esse cap, o histórico cresceria indefinidamente e o sistema ficaria incapaz de se adaptar a mudanças na cena (ghosting).

---

### 17.3 `ReSTIRGISpatial.cs.hlsl` — Pass B: Reuso Espacial

**Dispatch:** `⌈W/8⌉ × ⌈H/8⌉ × 1`, grupo `8×8×1`

O Pass B lê os reservatórios do Pass A e funde k vizinhos dentro de um disco:

#### Seleção e rejeição de vizinhos

```hlsl
float2 E   = GGX_Rand2(px, frame * 7 + i * 131);  // semente diferente do Pass A
int2   qpx = px + int2(round(GGX_ConcentricDisk(E) * radius));

// Rejeição 1 — normal divergente (superfícies diferentes)
float4 qgb = GBuffer.Load(int3(qpx, 0));
float3 qn1 = DDGI_OctDecode(qgb.rg * 2 - 1);
if (dot(qn1, n1) < normalReject) continue;  // padrão: 0.9 = ~26 graus

// Rejeição 2 — posição muito diferente (depth discontinuity)
if (length(qa.xyz - x1) > posReject) continue;
```

As rejeições são essenciais para não misturar GI de superfícies diferentes (ex: vizinho na parede vs pixel no chão).

#### Fusão com Jacobiano

```hlsl
float J = ReconnectionJacobian(x1, nb.x1, nb.x2, nb.n2);
J = clamp(J, 0.1, 10.0);  // estabilidade numérica
float pHat = TargetPHat(x1, n1, nb.x2, nb.Lo);
ResMerge(rs, nb, pHat, J, rng);
```

O `J` amplifica ou atenua o peso do vizinho baseado em quão bem a amostra dele se transfere para este pixel.

#### Visibility ray (opcional, `UseVisibility = false` por padrão)

```hlsl
if (UseVisibility && rs.W > 0) {
    float3 toS = rs.x2 - x1;
    float  len = length(toS);
    if (len > 0.15) {  // garante TMax > TMin (DXR: TMax > TMin ou UB)
        RayDesc vray = { x1 + n1*normalBias, toS/len, 0.02, len - 0.05 };
        // RAY_FLAG_CULL_BACK_FACING: mesmas flags do trace inicial, para evitar
        // que backfaces sejam oclusores fantasmas que a amostra original nunca viu
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> vq;
        vq.TraceRayInline(...);
        if (hit) rs.W = 0.0;  // ocluído → zera contribuição
    }
}
```

Com `UseVisibility = true`, o GI adquire "sombras de contato suaves": quando `x2` está ocluído, o pixel recebe `gi = 0`. O NRD borra esse 0/1 estocástico em um gradiente suave — o mesmo efeito do AO de bounce real, mas derivado da visibilidade de GI. O custo é um raio extra por pixel.

---

### 17.4 `ReSTIRNrdPack.cs.hlsl` — Fase C: Integração NRD

O NVIDIA NRD RELAX é um denoiser temporal especializado para GI difusa. Ele precisa de inputs específicos com encodings exatos:

```hlsl
// IN_VIEWZ: profundidade linear positiva (câmera → ponto)
// Céu → 1e8 (> denoisingRange → ignorado pelo NRD)
float4 viewPos = mul(float4(worldPos, 1.0), View);
OutViewZ[px]   = abs(viewPos.z);

// IN_NORMAL_ROUGHNESS: normal R10G10B10A2 + roughness (encoding NRD exato)
// O encode deve usar NRD_FrontEnd_PackNormalAndRoughness do NRD.hlsli
// para que o decode interno do NRD seja compatível bit a bit
OutNormalRough[px] = NRD_FrontEnd_PackNormalAndRoughness(N, roughness, 0.0);

// IN_MV: motion vector (curUV - prevUV)
// NRD usa motionVectorScale=(-1,-1,0) para inverter o sinal
OutMv[px] = Velocity.Load(int3(px, 0)).rg;

// IN_DIFF_RADIANCE_HITDIST: radiância + distância do hit (para filtragem hitDist-aware)
// hitDist vem do Pass A (preservado no .a do GIOut ao longo do Pass B)
OutDiffRadHit[px] = RELAX_FrontEnd_PackRadianceAndHitDist(gi.rgb, gi.a, true);
```

O `hitDist` é fundamental para o RELAX: ele usa a distância do hit para ajustar o raio de denoising — pixels com hits próximos (GI de curto alcance) recebem filtro mais estreito do que pixels com hits distantes. Sem isso o denoiser borraria detalhes de GI próximos indevidamente.

---

### 17.5 Integração no `DeferredLighting.ps.hlsl`

O ReSTIR GI substitui o termo difuso do DDGI quando `UseReSTIR = true` (`ReflectionParams.w > 0.5`):

```hlsl
bool UseGI     = DDGIGridCount.w > 0.5;   // DDGI grid ativo
bool UseReSTIR = ReflectionParams.w > 0.5; // ReSTIR GI substitui o difuso do DDGI

if (UseGI || UseReSTIR) {
    float3 gi;
    if (UseReSTIR) {
        gi = ReSTIRGITex.Load(int3(px, 0)).rgb; // lê direto por pixel
    } else {
        gi = SampleDDGIIrradianceCheb(...);      // interpola a grade de probes
    }
    // Aplica sobre a superfície: Kd * albedo * gi * intensity * AO
    Ambient += (1.0 - Metallic) * DiffuseColor * gi * giIntensity * AOAll;

    // Quando não há IBL, adiciona também o especular derivado do GI
    if (IBLParams.w <= 0.5) {
        float3 Fa = F_SchlickRoughness(SpecularColor, NoV, Roughness);
        Ambient += Fa * gi * giIntensity * AOAll * SpecAmbientScale;
    }
}
```

**Hierarquia de iluminação indireta difusa:**

| Modo | Difuso | Especular |
|---|---|---|
| Só IBL | `IrradianceMap` (cubemap) | `PrefilteredMap` + BRDF LUT |
| DDGI sem ReSTIR | `SampleDDGIIrradianceCheb` (grade de probes) | IBL (se disponível) |
| ReSTIR GI (ativo) | `ReSTIRGITex` (per-pixel full-res) | IBL (se disponível) |
| Atmosfera + ReSTIR | Hemisférico Hillaire para difuso | ReSTIR GI para especular aproximado |

Quando o DDGI está ativo mas o ReSTIR não, o DDGI continua sendo o único fornecedor de GI difusa. O ReSTIR usa o DDGI como cache de bounces no hit shading, mas expõe sua própria textura de saída para o deferred.

---

### 17.6 Estado Ping-Pong e Barreiras D3D12

O `RecordTrace` em `ReSTIRGI.cpp` gerencia as transições de estado explicitamente:

```
Frame p (FrameParity = p):

  Pass A:
    Res*[1-p] → NON_PIXEL_SHADER_RESOURCE   (leitura: reservatórios do frame anterior)
    Res*[p]   → UNORDERED_ACCESS             (escrita: reservatórios do frame atual)
    GITexture → UNORDERED_ACCESS             (escrita: resolve inicial)

  Pass B (se Spatial=true):
    Res*[p]   → NON_PIXEL_SHADER_RESOURCE   (leitura: reservatórios do Pass A)
    UAV barrier na GITexture               (Pass A escreveu .rgb; Pass B lê .a e sobrescreve .rgb)
    GITexture → UNORDERED_ACCESS             (escrita: resolve final)

  Após passes:
    GITexture → PIXEL_SHADER_RESOURCE       (sem NRD: deferred PS lê em t16)
             → NON_PIXEL_SHADER_RESOURCE    (com NRD: NrdPack compute lê)

  FrameParity ^= 1  (alterna para o próximo frame)
```

A UAV barrier entre Pass A e Pass B é necessária porque ambos usam `GITexture` como UAV. Pass A escreve `{rgb=gi_fallback, a=hitDist}`; Pass B lê `.a` (hitDist, preservado) e sobrescreve `.rgb` com o gi espacial.

---

## 18. ReSTIR GI — Recursos GPU, API e Configuração

### Recursos GPU

| Recurso | Formato | Tamanho | Propósito |
|---|---|---|---|
| `GITexture` | R16G16B16A16_FLOAT | W×H | rgb=irradiância final, a=hitDist para NRD |
| `ResA[2]` | R32G32B32A32_FLOAT | W×H | x1.xyz + M (precisão total: posição para rejeição) |
| `ResB[2]` | R32G32B32A32_FLOAT | W×H | x2.xyz + W (precisão total: posição para Jacobiano) |
| `ResC[2]` | R16G16B16A16_FLOAT | W×H | Lo.rgb (half: suficiente para radiância) |
| `ResD[2]` | R16G16B16A16_FLOAT | W×H | n2.xyz (half: normal para Jacobiano) |

`ResA` e `ResB` usam `R32` (float completo) porque `x1`, `x2` são posições world-space que precisam de precisão suficiente para rejeição por posição e cálculo do Jacobiano. `ResC` e `ResD` usam `R16` pois radiância e normais têm tolerância a pequenos erros de quantização.

### `ReSTIRGIConstants`

```cpp
struct alignas(256) ReSTIRGIConstants {
    Mat44 InvViewProj;      // reconstrução worldPos do depth
    Vec4  CameraPos;        // xyz = câmera
    Vec4  ScreenParams;     // W, H, 1/W, 1/H
    Vec4  GridMinSpacing;   // DDGI: xyz=origem, w=espaçamento
    Vec4  GridCount;        // DDGI: xyz=countXYZ
    Vec4  AtlasParams;      // DDGI: x=tile(6), y=atlasW, z=atlasH
    Vec4  SunDirIntensity;  // xyz=direção ao sol, w=intensidade
    Vec4  SunColor;         // rgb=cor do sol
    Vec4  TraceParams;      // x=frameIndex, y=maxRayDist, z=skyIntensity, w=normalBias
    Vec4  ShadeParams;      // x=realHitShading(0/1), y=albedoLOD, z=fireflyMaxLuma
    Vec4  ReuseParams;      // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    Vec4  SpatialParams;    // x=radius(px), y=count, z=spatial(0/1), w=normalReject
    Mat44 View;             // worldPos → view.z (NRD pack)
};
```

### API (`Engine/Include/Smile/Graphics/ReSTIRGI.h`)

```cpp
// Sincroniza parâmetros do DDGI. Chamar após FDDGI::SetupForScene.
void SetGIParams(GridMin, Spacing, GridCount, AtlasTile, AtlasW, AtlasH, MaxRayDist);

// (Re)cria texturas de reservatório. Chamar em cada resize de tela.
void SetupForResize(Device, SRVHeap, Width, Height,
                    TlasSlot, SkyViewSlot, InstanceSlot, IrradSlot,
                    VertexSlot, IndexSlot, DepthSlot, GBufferSlot, VelocitySlot);

// Grava Pass A + Pass B no command list.
void RecordTrace(CommandList, SRVHeap);

// Configura o NRD pack (chamar após NrdDenoiser::SetupForResize).
void SetupNrdPack(Device, SRVHeap, NrdInViewZ, NrdInNormalRough, NrdInMv, NrdInDiffRadHit, NrdOut);
void RecordNrdPack(CommandList, SRVHeap);  // grava Fase C

// Retorna o slot SRV para o deferred: NrdOut quando UseNrd, GITexture caso contrário.
u32  GITexSRVSlot() const;

// Toggles principais
void SetRealHitShading(bool);  // shading completo nos hits vs. albedo flat (padrão: true)
void SetTemporal(bool);        // reuso temporal via motion vector (padrão: true)
void SetSpatial(bool);         // reuso espacial k-vizinhos (padrão: true)
void SetVisibility(bool);      // shadow ray no resolve espacial (padrão: false — caro)
void SetUseNrd(bool);          // denoising NRD RELAX (padrão: false)
```

### Parâmetros tuníveis

| Parâmetro | Padrão | Efeito |
|---|---|---|
| `MCap` | 20 | Histórico máximo de amostras. Maior → mais estável mas mais ghosting em câmera/cena em movimento |
| `FireflyMax` | 8.0 | Teto de luminância de `Lo` antes de criar o reservatório. 0 = desativado |
| `AlbedoLOD` | 2.0 | Nível de MIP da textura de albedo nos hits do trace. Economiza largura de banda |
| `SpatialRadius` | 16 px | Raio do disco de busca de vizinhos no Pass B |
| `SpatialCount` | 4 | Número de vizinhos fundidos por pixel. Mais → menos ruído, mais custo |
| `NormalReject` | 0.9 | `cos` mínimo entre normais para aceitar vizinho (~26°) |
| `PosRejectScale` | 0.01 | Tolerância de posição = `scale * dist(cam, pixel)`. Proporcional à distância evita rejeição excessiva em longas distâncias |

---

## 19. Mapa de Arquivos

### Branch `main`

```
SmileEngine/
├── Engine/
│   ├── Include/Smile/Graphics/
│   │   ├── HDREnvironment.h      — classe principal IBL; constantes; API
│   │   ├── CubeTexture.h         — wrapper GPU cubemap; UAVs por mip
│   │   ├── Renderer.h            — FrameConstants; API pública IBL/atmosfera
│   │   └── Skybox.h              — renderização do fundo EnvCube
│   └── Source/Graphics/
│       ├── HDREnvironment.cpp    — pipeline de geração IBL; upload RGBE
│       └── Renderer.cpp          — CreateIBLDescriptorTable; RenderFrame
│
└── Shaders/
    ├── IBL/
    │   ├── Common.hlsli                — Hammersley, GGX, D_GGX, G_Smith
    │   ├── EquirectToCube.cs.hlsl      — equiretangular → cubemap face 0
    │   ├── MipGen.cs.hlsl              — geração de mip chain (box 2×2)
    │   ├── IrradianceConvolution.cs.hlsl — integral hemisférica difusa
    │   ├── SpecularPrefilter.cs.hlsl   — GGX importance sampling por mip
    │   └── BRDFIntegration.cs.hlsl     — LUT Fresnel split-sum
    ├── Triangle.ps.hlsl            — PBR pixel shader; aplicação IBL (L437-456)
    ├── Skybox.ps.hlsl              — amostragem EnvCube como fundo
    └── Skybox.vs.hlsl              — triângulo fullscreen para skybox
```

### Branch `feature/FFT-Oceanv2`

```
SmileEngine/
├── Engine/
│   ├── Include/Smile/Graphics/
│   │   ├── DDGI.h          — grade de probes; DDGIConstants; API; constantes kRaysPerProbe etc.
│   │   ├── DDGIDebug.h     — visualização de probes, raios, volume e stats
│   │   ├── ReSTIRGI.h      — reservatórios ping-pong; ReSTIRGIConstants; API toggles
│   │   ├── Reflections.h   — reflexos ray traced (especular); integra DDGI como cache
│   │   └── TemporalAA.h    — TAA com jitter Halton; usado junto com ReSTIR GI
│   └── Source/Graphics/
│       ├── DDGI.cpp        — SetupForScene; merging de geometria; RecordUpdate
│       ├── DDGIDebug.cpp   — renders de debug por modo
│       ├── ReSTIRGI.cpp    — alocação de reservatórios; tabelas de descritores; RecordTrace
│       └── NrdDenoiser.cpp — integração com NVIDIA NRD (setup + execute)
│
└── Shaders/
    └── GI/
        ├── DDGICommon.hlsli          — SphericalFibonacci, rotação aleatória, oct encode/decode,
        │                               SampleDDGIIrradiance, SampleDDGIIrradianceCheb
        ├── HitShading.hlsli          — ShadeSurfaceHit, ShadeSky, HitGeomNormal
        ├── DDGITrace.cs.hlsl         — trace de raios por probe (64 threads/probe)
        ├── DDGIUpdate.cs.hlsl        — acumulação de irradiância com hysteresis (6×6/probe)
        ├── DDGIUpdateDist.cs.hlsl    — acumulação de distância mean+var (14×14/probe)
        ├── DDGIRelocate.cs.hlsl      — relocalização + deactivação + ray count adaptativo
        ├── DDGIDebugProbes.ps.hlsl   — esferas de probe coloridas por irradiância
        ├── DDGIDebugProbes.vs.hlsl
        ├── DDGIDebugRays.vs.hlsl     — segmentos de raios do último frame
        ├── DDGIDebugVolume.vs.hlsl   — wireframe da grade 3D
        ├── DDGIDebugStats.cs.hlsl    — contadores de probes ativos/inativos
        ├── ReSTIRReservoir.hlsli     — WRS, ResMerge, ReconnectionJacobian, ResResolve
        ├── ReSTIRGITrace.cs.hlsl     — Pass A: trace + temporal (8×8/tile)
        ├── ReSTIRGISpatial.cs.hlsl   — Pass B: reuso espacial k-vizinhos + visibility (8×8/tile)
        └── ReSTIRNrdPack.cs.hlsl     — Fase C: empacotamento inputs NRD RELAX (8×8/tile)
```
