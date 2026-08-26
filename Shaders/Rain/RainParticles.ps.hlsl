#include "RainCommon.hlsli"

// Chuva F5a — PS das gotas por particula: soft-depth manual contra o depth da cena
// (reverse-Z: maior = mais perto) e perfil fino com pontas afiladas. Saida
// premultiplicada (mesmo blend da cortina).

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : TEXCOORD1;
    float  alpha : TEXCOORD2;
    float4 curClip  : TEXCOORD3;
    float4 prevClip : TEXCOORD4;
};

struct PSOut {
    float4 color    : SV_Target0;
    float4 velocity : SV_Target1;
};

PSOut main(VSOut input) {
    float sceneZ = SceneDepth.Load(int3(int2(input.pos.xy), 0));
    if (sceneZ > input.pos.z) discard; // cena na frente da gota

    float px = 1.0f - abs(input.uv.x * 2.0f - 1.0f); // fino no meio
    float py = 1.0f - abs(input.uv.y * 2.0f - 1.0f); // pontas afiladas
    float a  = input.alpha * px * px * saturate(py * 3.0f);
    if (a <= 0.002f) discard;

    float2 velocity = 0.0f;
    if (input.curClip.w > 1e-4f && input.prevClip.w > 1e-4f) {
        const float2 curNdc  = input.curClip.xy / input.curClip.w;
        const float2 prevNdc = input.prevClip.xy / input.prevClip.w;
        const float2 curUv   = float2(curNdc.x * 0.5f + 0.5f, 0.5f - curNdc.y * 0.5f);
        const float2 prevUv  = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
        velocity = curUv - prevUv;
    }

    PSOut o;
    o.color = float4(input.color * a, a);
    o.velocity = float4(velocity, 0.0f, a);
    return o;
}
