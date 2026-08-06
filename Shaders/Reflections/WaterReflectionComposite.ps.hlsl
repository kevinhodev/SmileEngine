#include "../GI/DDGICommon.hlsli"
#include "../GBuffer.hlsli"

cbuffer CompositeCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 ReflectParams;
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;
    float4 HalfScreenParams;
    row_major float4x4 PrevViewProj;
    float4 PrevCameraPos;
    float4 TemporalParams;
    float4 DebugParams;
    row_major float4x4 View;
    float4 RayEpsA;
    float4 RayEpsB;
    float4 PolicyParams; // y = escala; zw = direcao do vento no plano XZ
};

Texture2D<float4> Reflection : register(t0);
Texture2D<float4> GBuffer    : register(t1);
Texture2D<float>  Depth      : register(t2);
Texture2D<float4> BRDFLut    : register(t3);
Texture2D<float4> GBufferC   : register(t4); // water: r=foamSuppress, g=cobertura, ba=roughness
Texture2D<float4> GBufferA   : register(t5);

SamplerState LinearClamp : register(s0);

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET {
    const int2 px = int2(input.pos.xy);
    const float4 gb = GBuffer.Load(int3(px, 0));
    if (GBuffer_DecodeShadingModel(gb.a) != SMILE_SHADINGMODEL_WATER) return 0.0f;

    const float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) return 0.0f;

    const float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
    const float4 waterData = GBufferC.Load(int3(px, 0));
    const float2 roughnessAxes = max(waterData.ba, 0.04f.xx);
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wH = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 worldPos = wH.xyz / wH.w;
    const float3 V = normalize(CameraPos.xyz - worldPos);
    const float NoV = saturate(dot(N, V));

    float2 wind = PolicyParams.zw;
    wind = (dot(wind, wind) > 1.0e-6f) ? normalize(wind) : float2(1, 0);
    float3 T = float3(wind.x, 0.0f, wind.y);
    T -= N * dot(N, T);
    if (dot(T, T) <= 1.0e-6f) {
        const float3 axis = (abs(N.y) < 0.999f) ? float3(0, 1, 0) : float3(1, 0, 0);
        T = cross(axis, N);
    }
    T = normalize(T);
    const float3 B = normalize(cross(N, T));
    const float2 alphaAxes = roughnessAxes * roughnessAxes;
    const float2 tangentView = float2(dot(V, T), dot(V, B));
    const float tangentLen2 = dot(tangentView, tangentView);
    const float alphaView = (tangentLen2 > 1.0e-5f)
        ? sqrt(dot(alphaAxes * tangentView, alphaAxes * tangentView) / tangentLen2)
        : sqrt(alphaAxes.x * alphaAxes.y);
    const float directionalRoughness = sqrt(saturate(alphaView));
    const float2 brdf = BRDFLut.SampleLevel(
        LinearClamp, float2(NoV, directionalRoughness), 0.0f).rg;
    const float envBrdf = saturate(0.0204f * brdf.x + brdf.y);
    const float foamSuppress = saturate(1.0f - waterData.x);
    const float reflectionCoverage = saturate(waterData.y);
    const float3 radiance = Reflection.Load(int3(px, 0)).rgb;
    const float3 specular = radiance * envBrdf * max(PolicyParams.y, 0.0f) *
                            foamSuppress * reflectionCoverage;
    return float4(specular, 0.0f);
}
