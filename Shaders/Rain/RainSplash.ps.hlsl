#include "RainCommon.hlsli"

// Chuva F5b — PS do splash: coroa (anel gaussiano) expandindo dentro do quad + fade
// quadratico da vida. Soft-depth manual como as gotas; premultiplicado.

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : TEXCOORD1;
    float  alpha : TEXCOORD2;
    float  life  : TEXCOORD3;
    float4 curClip  : TEXCOORD4;
    float4 prevClip : TEXCOORD5;
};

struct PSOut {
    float4 color    : SV_Target0;
    float4 velocity : SV_Target1;
};

PSOut main(VSOut input) {
    float sceneZ = SceneDepth.Load(int3(int2(input.pos.xy), 0));
    if (sceneZ > input.pos.z) discard;

    // coroa: anel meio-eliptico (base do quad = superficie) que expande com a vida
    float2 c = float2((input.uv.x - 0.5f) * 2.0f, input.uv.y * 1.4f);
    float  d = length(c);
    float  x = (d - (0.15f + input.life * 0.80f)) * 5.5f;
    float  ring = exp(-x * x);

    float a = input.alpha * ring * (1.0f - input.life) * (1.0f - input.life);
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
