// Specular GI — composite das reflexoes RT no scene color (HDR). Fullscreen, blend ADITIVO
// (One/One): soma a contribuicao especular sobre a cor ja sombreada. Aplica o peso BRDF
// (split-sum: F0*brdf.x + brdf.y) e o blend por roughness com o DDGI (combineAlpha do Lumen).
// O forward ja reduziu o specular ambiente (IBL/stopgap) por (1-combineAlpha) -> sem double-count.
//
// Fase 1: F0 = lerp(0.04, 1, metallic) (metal "branco"; sem o tint de baseColor — exige um canal
// de F0/albedo no G-buffer, adiado p/ F2/F3). Le radiancia INCIDENTE crua do trace.

#include "../GI/DDGICommon.hlsli" // DDGI_OctDecode

cbuffer CompositeCB : register(b0) {
    row_major float4x4 InvViewProj; // FULL inverse view-proj (reconstroi world do depth)
    float4 CameraPos;       // xyz = camera world, w = -
    float4 ScreenParams;    // x = W, y = H, z = 1/W, w = 1/H
    float4 ReflectParams;   // x = maxRoughnessToTrace, y = roughnessFadeLength, zw = -
};

Texture2D<float4> Reflection : register(t0); // rgb = radiancia incidente
Texture2D<float4> GBuffer    : register(t1); // RG = octN*0.5+0.5, B = roughness, A = metallic
Texture2D<float>  Depth      : register(t2); // NDC z (Reverse-Z: 0 = far/ceu)
Texture2D<float4> BRDFLut    : register(t3); // Karis split-sum LUT (RG: F0 scale, F0 bias)

SamplerState LinearClamp : register(s0);

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET {
    int2 px = int2(input.pos.xy);

    float4 gb        = GBuffer.Load(int3(px, 0));
    float  roughness = gb.b;
    float  metallic  = gb.a;
    float  combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f || combineAlpha <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f); // ceu / pixel rugoso: nada a somar

    float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    // Reconstroi world -> V -> NoV (mesma convencao do fog/trace).
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 V = normalize(CameraPos.xyz - worldPos);
    float  NoV = saturate(dot(N, V));

    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), float3(1.0f, 1.0f, 1.0f), metallic);
    float2 brdf = BRDFLut.SampleLevel(LinearClamp, float2(NoV, roughness), 0.0f).rg;

    float3 reflRad = Reflection.Load(int3(px, 0)).rgb;
    float3 spec    = reflRad * (F0 * brdf.x + brdf.y) * combineAlpha;

    return float4(spec, 0.0f); // blend ADD: HDR.rgb += spec
}
