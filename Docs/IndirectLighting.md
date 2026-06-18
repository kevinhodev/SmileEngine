# Iluminação Indireta — SmileEngine

O sistema de iluminação indireta da SmileEngine é baseado em **Image-Based Lighting (IBL)** com a aproximação *split-sum* de Karis. Todo o pré-processamento roda em compute shaders na GPU, permitindo trocar o ambiente HDR em tempo real pelo editor.

---

## Sumário

1. [Visão Geral](#1-visão-geral)
2. [Pipeline de Pré-processamento](#2-pipeline-de-pré-processamento)
3. [Recursos GPU](#3-recursos-gpu)
4. [Shaders de Pré-processamento](#4-shaders-de-pré-processamento)
5. [Aplicação em Runtime (Pixel Shader)](#5-aplicação-em-runtime-pixel-shader)
6. [Ambient Hemisférico da Atmosfera](#6-ambient-hemisférico-da-atmosfera)
7. [Integração com o Renderer](#7-integração-com-o-renderer)
8. [Estruturas de Dados](#8-estruturas-de-dados)
9. [API Pública](#9-api-pública)
10. [Constantes de Configuração](#10-constantes-de-configuração)
11. [Mapa de Arquivos](#11-mapa-de-arquivos)

---

## 1. Visão Geral

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

## 2. Pipeline de Pré-processamento

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

## 3. Recursos GPU

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

## 4. Shaders de Pré-processamento

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

## 5. Aplicação em Runtime (Pixel Shader)

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

## 7. Integração com o Renderer

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

## 8. Estruturas de Dados

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

## 9. API Pública

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

## 10. Constantes de Configuração

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

## 11. Mapa de Arquivos

```
SmileEngine/
├── Engine/
│   ├── Include/Smile/Graphics/
│   │   ├── HDREnvironment.h      — classe principal; constantes IBL; API
│   │   ├── CubeTexture.h         — wrapper GPU cubemap; UAVs por mip
│   │   ├── Renderer.h            — FrameConstants; API pública IBL
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
