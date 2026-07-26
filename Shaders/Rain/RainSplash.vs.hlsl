#include "RainCommon.hlsli"

// Chuva F5b — splash 3D no ponto de impacto, SEM estado: a queda da gota e periodica e a
// fracao "subterranea" do ciclo (depois que ela cruza o heightmap e some) e exatamente a
// janela do splash. Profundidade abaixo do teto local = relogio: tSinceHit = below/speed.
// Cada gota tem 1 splash em voo por ciclo, na posicao real do impacto (chao, telhado,
// capo de carro — qualquer topo do occlusion map). Billboard vertical de frente pra
// camera com coroa expandindo no PS.

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : TEXCOORD1;
    float  alpha : TEXCOORD2;
    float  life  : TEXCOORD3; // 0 = acabou de bater, 1 = fim do splash
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;

    const float boxR = RainParticleParams.x;
    float  speed;
    float2 h1;
    float3 p = RainParticlePos(iid, speed, h1); // MESMA conta da gota

    // onde/quando bateu: precisa do occlusion map (sem ele nao ha colisao nem splash)
    float  life    = -1.0f;
    float3 hitPos  = p;
    if (RainOccParams.x > 0.5f) {
        float3 uvz = mul(float4(p, 1.0f), RainOccMatrix).xyz;
        if (all(uvz.xy == saturate(uvz.xy))) {
            float mapZ   = RainOccMap.Load(int3(int2(uvz.xy * RainOccParams.w), 0));
            float belowM = (uvz.z - mapZ) / RainParams1.y; // metros abaixo do teto local
            const float dur = 0.22f * speed;               // janela = 0.22 s de queda
            if (belowM > 0.0f && belowM < dur) {
                life   = belowM / dur;
                hitPos = float3(p.x, p.y + belowM + 0.015f, p.z); // de volta a superficie
            }
        }
    }

    // billboard vertical de frente pra camera, ancorado no impacto; coroa cresce c/ a vida
    float3 toCam = CameraWorldPos.xyz - hitPos;
    float  dist  = max(length(toCam), 1e-3f);
    float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), toCam / dist) + 1e-5f);
    const float w = 0.025f + life * 0.075f;  // 2.5 -> 10 cm
    const float h = 0.030f + life * 0.025f;

    const uint  idx[6] = { 0u, 1u, 2u, 2u, 1u, 3u };
    const uint  k  = idx[vid];
    const float cu = (k & 1u) ? 1.0f : -1.0f;
    const float cv = (k & 2u) ? 1.0f : -1.0f;
    float3 wp = hitPos + right * (cu * w) + float3(0.0f, 1.0f, 0.0f) * ((cv * 0.5f + 0.5f) * h);

    // fades: vida valida, borda do box, perto demais; splash some mais cedo que a gota (30 m)
    float edge = saturate((boxR - length(p.xz - CameraWorldPos.xz)) / (boxR * 0.15f));
    float distF = saturate(1.0f - dist / 30.0f);
    float a = (life >= 0.0f ? 1.0f : 0.0f) * 0.55f * edge * distF *
              saturate((dist - 0.30f) * 2.0f);
    if (a <= 0.001f) { wp = hitPos; life = 0.0f; } // quad degenerado

    o.pos = mul(float4(wp, 1.0f), RainViewProj);
    o.uv  = float2(cu * 0.5f + 0.5f, cv * 0.5f + 0.5f);

    // splash pega mais luz que a gota (agua espumada espalha em todas as direcoes)
    float  keyLum = dot(KeyLightColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float3 keyCol = lerp(keyLum.xxx, KeyLightColor.rgb, 0.35f);
    o.color = keyCol * 0.10f + SkyAmbientRain.rgb * 0.16f;
    o.alpha = a;
    o.life  = saturate(life);
    return o;
}
