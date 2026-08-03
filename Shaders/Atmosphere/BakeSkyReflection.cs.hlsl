#include "AtmosphereCommon.hlsli"

// Baka a atmosfera Hillaire (SkyView LUT) num cubemap pequeno por frame — fonte do
// reflexo da agua (prefiltrado GGX em seguida por MipGen+SpecularPrefilter, mesmos
// shaders do IBL). Sem disco do sol/lua/estrelas: o spec do astro na agua e analitico.

Texture2D<float4>        SkyViewLUT : register(t0);
RWTexture2DArray<float4> OutCube    : register(u0);

static const uint kSkyReflCubeSize = 64;

// MESMA convencao de face do IBL (Common.hlsli/CubeFaceToDirection) — a agua amostra
// com o R em mundo, entao raw/prefiltrado/HDRI precisam concordar.
float3 SkyReflFaceDir(uint face, float2 uv) {
    float2 t = uv * 2.0f - 1.0f;
    t.y = -t.y;
    float3 dir;
    switch (face) {
        case 0:  dir = float3( 1.0f, t.y, -t.x); break;
        case 1:  dir = float3(-1.0f, t.y,  t.x); break;
        case 2:  dir = float3( t.x, 1.0f, -t.y); break;
        case 3:  dir = float3( t.x,-1.0f,  t.y); break;
        case 4:  dir = float3( t.x, t.y,  1.0f); break;
        default: dir = float3(-t.x, t.y, -1.0f); break;
    }
    return normalize(dir);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kSkyReflCubeSize || id.y >= kSkyReflCubeSize) return;

    float2 uv  = (float2(id.xy) + 0.5f) / float(kSkyReflCubeSize);
    float3 dir = SkyReflFaceDir(id.z, uv);

    // Mesmo mapeamento do SkyAtmosphere.ps (sem disco solar/glare).
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 sunDir = normalize(SunDir.xyz);

    float  viewZenithCos = dot(dir, up);
    float3 viewHoriz = dir    - up * viewZenithCos;
    float3 sunHoriz  = sunDir - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));

    float2 lutUV = SkyViewParamsToUv(viewZenithCos, lightViewCos, kViewHeight);
    float3 L = SkyViewLUT.SampleLevel(LinearClampSampler, lutUV, 0.0f).rgb;

    // Este cubemap e exclusivo da reflexao da agua. Radiancia abaixo do plano macroscopico
    // nao pode iluminar a superficie por reflexao; deixa-la no cubemap fazia o prefilter GGX
    // integrar o limbo atmosferico do planeta (ligeiramente abaixo do horizonte plano). Em
    // incidencia rasante esse pico virava uma faixa branca continua. A UE aplica a mesma
    // politica no capture do skylight: lower hemisphere solid/black antes da convolucao.
    if (dir.y < 0.0f) {
        L = 0.0f;
    }

    OutCube[uint3(id.xy, id.z)] = float4(L, 1.0f);
}
