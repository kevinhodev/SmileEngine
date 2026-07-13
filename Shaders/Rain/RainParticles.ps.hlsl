#include "RainCommon.hlsli"

// Chuva F5a — PS das gotas por particula: soft-depth manual contra o depth da cena
// (reverse-Z: maior = mais perto) e perfil fino com pontas afiladas. Saida
// premultiplicada (mesmo blend da cortina).

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : TEXCOORD1;
    float  alpha : TEXCOORD2;
};

float4 main(VSOut input) : SV_Target {
    float sceneZ = SceneDepth.Load(int3(int2(input.pos.xy), 0));
    if (sceneZ > input.pos.z) discard; // cena na frente da gota

    float px = 1.0f - abs(input.uv.x * 2.0f - 1.0f); // fino no meio
    float py = 1.0f - abs(input.uv.y * 2.0f - 1.0f); // pontas afiladas
    float a  = input.alpha * px * px * saturate(py * 3.0f);
    if (a <= 0.002f) discard;

    return float4(input.color * a, a);
}
