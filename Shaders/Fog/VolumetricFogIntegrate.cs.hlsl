#include "VolumetricFogCommon.hlsli"

// FinalIntegration (UE): acumula scattering/transmitancia da camera ate cada slice.
// Integracao energy-conserving do Frostbite ("Physically Based and Unified Volumetric
// Rendering"): o inscatter do slice e atenuado pela propria extincao dentro do passo.
// Resultado[z] = (luz acumulada ate o fim do slice z, transmitancia acumulada) — o
// fog apply amostra por (uv de tela, slice(dist)) e compoe sobre o analitico.

Texture3D<float4>   LightScatteringTex : register(t0);
RWTexture3D<float4> IntegratedTex      : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)kVolFogW || id.y >= (uint)kVolFogH) return;

    // Passo em DISTANCIA ao longo do raio da coluna = delta de view-depth / cos.
    float2 uv  = (float2(id.xy) + 0.5f) / GridSize.xy;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 pH  = mul(float4(ndc, 0.5f, 1.0f), InvViewProj);
    float3 dir = normalize(pH.xyz / pH.w - CameraWorldPos.xyz);
    float  invCos = 1.0f / max(dot(dir, CamForward.xyz), 0.1f);

    float3 accumLight = float3(0.0f, 0.0f, 0.0f);
    float  accumT     = 1.0f;
    float  prevDepth  = VolFog_DepthFromSlice(0.0f);

    [loop] for (int z = 0; z < kVolFogZ; ++z) {
        float4 scatExt = LightScatteringTex[uint3(id.xy, z)];

        // LightScatteringTex stores the medium at the cell center, but the
        // integrated result represents the END boundary of that cell. Using
        // z+0.5 left the last half-cell uncovered while the analytical fog only
        // resumed at MaxDistance (slice kVolFogZ), producing a horizon seam.
        float sliceDepth = VolFog_DepthFromSlice((float)z + 1.0f);
        float stepLen    = max(sliceDepth - prevDepth, 0.0f) * invCos;
        prevDepth = sliceDepth;

        float T = exp(-scatExt.w * stepLen);
        float3 sInt = (scatExt.rgb - scatExt.rgb * T) / max(scatExt.w, 1e-5f);
        accumLight += sInt * accumT;
        accumT     *= T;

        IntegratedTex[uint3(id.xy, z)] = float4(accumLight, accumT);
    }
}
