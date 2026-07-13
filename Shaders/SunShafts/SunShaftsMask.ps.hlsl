#include "SunShaftsCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

// Mascara dos shafts (meia-res): so brilha o que e ceu/geometria distante (mascara de
// distancia estilo UE BloomDistanceMask — evita "raio" saindo de parede proxima iluminada)
// e acima do limiar de luminancia (só o disco do sol/ceu forte alimenta o blur).

Texture2D<float4> SceneColor : register(t0);
Texture2D<float>  SceneDepth : register(t1);

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    float2 uv  = svpos.xy * ScreenParams.zw;
    float3 col = SceneColor.SampleLevel(LinearClamp, uv, 0.0f).rgb;
    float  d   = SceneDepth.SampleLevel(PointClamp, uv, 0.0f);

    // distancia linear em mundo (reverse-Z: 0 = ceu)
    float dist = 1e9f;
    if (!SmileIsSky(d)) {
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 wH  = mul(float4(ndc, d, 1.0f), InvViewProj);
        dist = length(wH.xyz / wH.w - CameraWorldPos.xyz);
    }

    // so a metade distante do range contribui (UE): parede/chao perto nao bloomam
    float invRange  = ShaftTint.w;
    float distMask  = saturate((dist - 0.5f / invRange) * invRange);

    // luminancia acima do limiar, com teto anti-firefly (UE BloomMaxBrightness)
    float lum = max(dot(col, float3(0.30f, 0.59f, 0.11f)), 6.10352e-5f);
    float adj = clamp(lum - ShaftParams.x, 0.0f, 50.0f);
    float3 bloom = col / lum * adj * 2.0f;

    // mascara radial em torno do sol (aspect-corrigida): o blur so puxa energia
    // de perto da origem — com o sol fora da tela o efeito morre sozinho, sem pop
    float2 av       = (SunPosParams.xy - uv) * float2(1.0f, SunPosParams.w);
    float  distSun  = 1.0f - saturate(length(av) * 2.0f);

    // mascara de borda (UE): 1 na borda da tela, 0 no centro — mata streak de borda
    float em = 1.0f - uv.x * (1.0f - uv.x) * uv.y * (1.0f - uv.y) * 8.0f;
    em = em * em;
    em = em * em;

    float3 result = bloom * distMask * distSun * distSun * (1.0f - em) * SunPosParams.z;
    return float4(result, 1.0f);
}
