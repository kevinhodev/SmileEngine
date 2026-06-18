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

## 15. ReSTIR GI — Visão Geral

ReSTIR GI é um estimador de GI difusa por pixel baseado em **Weighted Reservoir Sampling (WRS)** com reuso temporal e espacial. Cada pixel mantém um reservatório `{x1, x2, n2, Lo, M, W}` que representa a melhor amostra de iluminação secundária encontrada até agora.

A técnica reduz a variância em relação a path tracing simples ao reutilizar amostras de pixels vizinhos e do frame anterior, amplificando efetivamente 1 raio/pixel em dezenas de amostras equivalentes.

### Os três passes

```
Pass A — ReSTIRGITrace.cs.hlsl
  Por pixel:
  1. Traça 1 raio cosseno-hemisférico → (x2, n2, Lo)
  2. ShadeSurfaceHit: sol + DDGI bounce no hit
  3. Funde com o reservatório do frame anterior (motion vector)
  4. Resolve irradiância (fallback quando spatial está OFF)

Pass B — ReSTIRGISpatial.cs.hlsl
  Por pixel:
  1. Lê reservatório do Pass A (pós-temporal)
  2. Funde k vizinhos (padrão: 4) no raio de 16 px
     com Jacobiano de reconexão (Ouyang 2021)
  3. Visibility ray opcional para x2 selecionado
  4. Resolve irradiância final → GITexture

Fase C — ReSTIRNrdPack.cs.hlsl + NRD RELAX
  Empacota GITexture + GBuffer + depth + velocity
  para os inputs do NVIDIA NRD RELAX_DIFFUSE.
  NRD RELAX denoise em separado; saída via NrdOutSRV.
```

### Estrutura do reservatório

```
Reservoir {
    x1   : ponto visível (posição do pixel no mundo)
    x2   : ponto da amostra (hit do raio secundário)
    n2   : normal geométrica em x2 (para o Jacobiano de reconexão)
    Lo   : radiância em x2 na direção de x1
    M    : contagem de amostras (aumenta com reuso; clampado em MCap=20)
    W    : peso de contribuição não-enviesado = wSum / (M * pHat(selecionado))
    wSum : soma interna dos pesos WRS (não persiste entre passes)
}
```

---

## 16. ReSTIR GI — Pipeline e Shaders

### 16.1 `ReSTIRReservoir.hlsli`

Implementa as operações matemáticas do WRS.

**`TargetPHat(x1, n1, x2, Lo)`**
PDF alvo para o pixel `(x1, n1)` dada a amostra `(x2, Lo)`:
```
pHat = luminance(Lo) * saturate(dot(n1, normalize(x2 - x1)))
```
Proporcional à irradiância esperada. O albedo cancela (tratado na resolução).

**`ResUpdate`** — adiciona 1 candidato ao reservatório (WRS step):
```
wSum += w;  M += 1;
if (rand() * wSum <= w) → seleciona candidato
```

**`ResMerge`** — funde outro reservatório no atual. O peso escalado pelo Jacobiano:
```
w = pHat(x2_other, no_pixel_atual) * other.W * other.M * J
```

**`ReconnectionJacobian(x1Dst, x1Src, x2, n2)`**
Jacobiano de reconexão (Ouyang 2021). Corrige o change-of-variable ao reutilizar a amostra `x2` gerada a partir de `x1Src` em um pixel diferente `x1Dst`:
```
J = (cos(φ_dst) / cos(φ_src)) * (|x2 - x1Src|² / |x2 - x1Dst|²)
```
onde `φ` é o ângulo entre `n2` e a direção `x2 → x1`. Clampado em `[0.1, 10]` para evitar divergência.

**`ResResolve(r, x1, n1, maxLuma)`**
Extrai a irradiância do reservatório:
```
gi = Lo * cos(θ₁) * W / π    (= (1/π) * E)
```
Aplica firefly clamp na luminância de saída se `maxLuma > 0`.

---

### 16.2 `ReSTIRGITrace.cs.hlsl` — Pass A

**Dispatch:** `⌈W/8⌉ × ⌈H/8⌉ × 1`, grupo `8×8×1`

**Passo 1 — Sample inicial:**
```hlsl
// Amostragem cosseno-hemisférica (método de Malley)
float2 E   = GGX_Rand2(px, frame);          // 2D pseudo-aleatório por pixel
float  rr  = sqrt(E.x);                     // raio no disco de Malley
float  phi = 2π * E.y;
float  cosT = sqrt(1 - E.x);               // cosTheta
float3 dir = TangentBasis(N) * float3(rr*cos(φ), rr*sin(φ), cosT);

// Trace inline + ShadeSurfaceHit (sol + DDGI bounce)
// Firefly clamp: Lo = Lo * min(1, maxLuma / luminance(Lo))
```

**Passo 2 — Reuso temporal:**
```hlsl
float2 vel = Velocity[px];                  // motion vector (curUV - prevUV)
float2 prevUv = uv - vel;
if (dentro_da_tela) {
    prev = lerReservoir(PrevResA/B/C/D[ppx]);
    prev.M = min(prev.M, MCap);             // cap histórico = 20
    posReject = posRejectScale * dist(camera, x1);
    if (length(prev.x1 - x1) < posReject)  // aceita se posição próxima
        ResMerge(r, prev, pHat, J=1.0);    // J=1: mesma superfície reprojetada
}
```

**Passo 3 — Escrita:**
Grava `{x1, x2, n2, Lo, M, W}` em 4 texturas ping-pong (`CurrResA/B/C/D`), além de `gi = ResResolve(...)` em `GIOut` (sobrescrito pelo Pass B quando spatial está ativo).

---

### 16.3 `ReSTIRGISpatial.cs.hlsl` — Pass B

**Dispatch:** `⌈W/8⌉ × ⌈H/8⌉ × 1`, grupo `8×8×1`

Lê os reservatórios do Pass A e funde `K` vizinhos (padrão 4) dentro de raio de 16 px:

```hlsl
// Vizinhos em disco concentrico (distribuição uniforme).
for (int i = 0; i < K; ++i) {
    float2 E   = GGX_Rand2(px, frame * 7 + i * 131);
    int2   qpx = px + round(GGX_ConcentricDisk(E) * radius);

    // Rejeições:
    if (dot(n_vizinho, n_pixel) < normalReject)  continue; // normal (padrão: 0.9)
    if (length(x1_vizinho - x1_pixel) > posReject) continue; // posição

    float J    = ReconnectionJacobian(x1, nb.x1, nb.x2, nb.n2);
    J = clamp(J, 0.1, 10.0);
    ResMerge(rs, nb, pHat(x2_vizinho), J, rng);
}
```

**Visibility ray opcional (`UseVisibility = true`):**
```hlsl
// Testa oclusão entre x1 e a amostra x2 SELECIONADA.
// Se ocluído → W = 0 (gi = 0 para esse pixel).
// NRD borra o 0/1 estocástico → sombra de contato suave.
// Descartado por padrão (caro: 1 shadow ray/pixel extra).
if (len(x2 - x1) > 0.15) {
    vray.TMax = len - 0.05;     // > TMin (0.02) garantido
    if (hit) rs.W = 0.0;
}
```

---

### 16.4 `ReSTIRNrdPack.cs.hlsl` — Fase C

Empacota os dados para o denoiser NVIDIA NRD RELAX_DIFFUSE:

| Output NRD | Fonte | Encoding |
|---|---|---|
| `IN_VIEWZ` | depth → worldPos → view.z linear | float R32 |
| `IN_NORMAL_ROUGHNESS` | GBuffer oct-normal + roughness | `NRD_FrontEnd_PackNormalAndRoughness` → R10G10B10A2 |
| `IN_MV` | velocity (curUV - prevUV) | RG32F |
| `IN_DIFF_RADIANCE_HITDIST` | GITexture (rgb=gi, a=hitDist) | `RELAX_FrontEnd_PackRadianceAndHitDist` |

O NRD usa `motionVectorScale = (-1, -1, 0)` para inverter o sinal do MV. O céu recebe `viewZ = 1e8` para ser ignorado pelo denoiser (fora do `denoisingRange`).

---

## 17. ReSTIR GI — Estruturas e Recursos GPU

### `ReSTIRGIConstants` (`Engine/Include/Smile/Graphics/ReSTIRGI.h`)

```cpp
struct alignas(256) ReSTIRGIConstants {
    Mat44 InvViewProj;      // para reconstruir worldPos do depth
    Vec4  CameraPos;
    Vec4  ScreenParams;     // W, H, 1/W, 1/H
    Vec4  GridMinSpacing;   // DDGI: origem + espaçamento
    Vec4  GridCount;        // DDGI: countXYZ
    Vec4  AtlasParams;      // DDGI: tile, W, H
    Vec4  SunDirIntensity;
    Vec4  SunColor;
    Vec4  TraceParams;      // frameIndex, maxRayDist, skyIntensity, normalBias
    Vec4  ShadeParams;      // realHitShading, albedoLOD, fireflyMaxLuma
    Vec4  ReuseParams;      // MCap, posRejectScale, visibility(0/1), temporal(0/1)
    Vec4  SpatialParams;    // radius(px), count, spatial(0/1), normalReject
    Mat44 View;             // worldPos → view.z para o NRD pack
};
```

### Recursos GPU

| Recurso | Formato | Tamanho | Propósito |
|---|---|---|---|
| `GITexture` | R16G16B16A16_FLOAT | W×H | Saída de GI (rgb=irradiância, a=hitDist para NRD) |
| `ResA[2]` | R32G32B32A32_FLOAT | W×H | x1.xyz, M (ping-pong por paridade de frame) |
| `ResB[2]` | R32G32B32A32_FLOAT | W×H | x2.xyz, W |
| `ResC[2]` | R16G16B16A16_FLOAT | W×H | Lo.rgb |
| `ResD[2]` | R16G16B16A16_FLOAT | W×H | n2.xyz |

O ping-pong usa `FrameParity = FrameIndex & 1`. Pass A lê `Res*[1-p]` (frame anterior) e escreve `Res*[p]` (frame atual).

---

## 18. ReSTIR GI — API e Configuração

### API (`Engine/Include/Smile/Graphics/ReSTIRGI.h`)

```cpp
// Passa os parâmetros do DDGI para o ReSTIR (grade, atlas). Chamar após SetupForScene do DDGI.
void SetGIParams(GridMin, Spacing, GridCount, AtlasTile, AtlasW, AtlasH, MaxRayDist);

// (Re)cria as texturas de reservatório quando a resolução muda.
void SetupForResize(Device, SRVHeap, Width, Height,
                    TlasSlot, SkyViewSlot, InstanceSlot, IrradSlot,
                    VertexSlot, IndexSlot, DepthSlot, GBufferSlot, VelocitySlot);

// Grava Pass A + Pass B (+ Fase C se UseNrd).
void RecordTrace(CommandList, SRVHeap);

// Configura o NRD pack (chamar após Nrd.SetupForResize).
void SetupNrdPack(Device, SRVHeap,
                  NrdInViewZ, NrdInNormalRough, NrdInMv, NrdInDiffRadHit, NrdOut);
void RecordNrdPack(CommandList, SRVHeap);

// Toggles
void SetRealHitShading(bool);  // shading completo nos hits do trace (padrão: true)
void SetTemporal(bool);        // reuso temporal via MV (padrão: true)
void SetSpatial(bool);         // reuso espacial k-vizinhos (padrão: true)
void SetVisibility(bool);      // shadow ray de visibilidade no resolve (padrão: false)
void SetUseNrd(bool);          // denoising via NRD RELAX (padrão: false)
```

### Parâmetros tuníveis (privados com defaults)

| Parâmetro | Padrão | Impacto |
|---|---|---|
| `MCap` | 20 | Máximo de amostras acumuladas; maior = mais estável, mais ghosting |
| `FireflyMax` | 8.0 | Teto de luminância de saída (0 = desativado) |
| `AlbedoLOD` | 2.0 | MIP de albedo nos hits (economiza largura de banda) |
| `SpatialRadius` | 16 px | Raio de busca dos vizinhos espaciais |
| `SpatialCount` | 4 | Número de vizinhos fundidos por pixel |
| `NormalReject` | 0.9 | `dot(n_q, n_r)` mínimo para aceitar vizinho |
| `PosRejectScale` | 0.01 | Fração da distância câmera-pixel para rejeição por posição |

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
