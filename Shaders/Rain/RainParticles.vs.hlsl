#include "RainCommon.hlsli"

// Chuva F5a — gotas por PARTICULA no near-field (opcao de qualidade sobre o MESMO estado
// de clima). Sem buffer persistente e sem compute: a posicao e 100% procedural por
// instancia (hash + tempo), num box de ~24 m preso na camera com wrap world-fixed — a
// gota e fixa no mundo ate o box "dobrar a esquina". A COLISAO vem do occlusion map da
// F2 usado como heightmap top-down: gota abaixo do teto local (telhado/marquise/chao)
// colapsa — nao chove em area coberta e a gota some exatamente onde bateria (F5b vai
// spawnar o splash 3D nesse ponto). Quad esticado pela queda via SV_VertexID, sem VB.

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float3 color : TEXCOORD1; // tint por particula (fwd scatter, ja inclui tudo menos alpha)
    float  alpha : TEXCOORD2;
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;

    const float boxR = RainParticleParams.x;
    float  speed;
    float2 h1;
    float3 p = RainParticlePos(iid, speed, h1); // conta compartilhada com o splash (F5b)

    // colisao/oclusao pelo heightmap: abaixo do teto local = ja bateu (ou area coberta)
    float kill = 0.0f;
    if (RainOccParams.x > 0.5f) {
        float3 uvz = mul(float4(p, 1.0f), RainOccMatrix).xyz;
        if (all(uvz.xy == saturate(uvz.xy))) {
            float mapZ = RainOccMap.Load(int3(int2(uvz.xy * RainOccParams.w), 0));
            if (uvz.z > mapZ - 0.15f * RainParams1.y) kill = 1.0f; // some 0.15 m antes do hit
        }
    }

    // billboard cilindrico: eixo = velocidade (vertical), largura de frente pra camera
    const float3 axis = float3(0.0f, -1.0f, 0.0f);
    const float  len  = 0.14f + speed * 0.016f;              // motion stretch
    const float  wid  = 0.0045f + h1.x * 0.0040f;
    float3 toCam = CameraWorldPos.xyz - p;
    float  dist  = max(length(toCam), 1e-3f);
    float3 right = normalize(cross(axis, toCam / dist) + 1e-5f);

    const uint  idx[6] = { 0u, 1u, 2u, 2u, 1u, 3u };         // 2 tris do quad
    const uint  k  = idx[vid];
    const float cu = (k & 1u) ? 1.0f : -1.0f;
    const float cv = (k & 2u) ? 1.0f : -1.0f;
    float3 wp = p + right * (cu * wid) + axis * (cv * len * 0.5f);

    // fades: borda do box (esconde o pop do wrap) e muito-perto da camera; kill colapsa
    float edge = saturate((boxR - length(p.xz - CameraWorldPos.xz)) / (boxR * 0.15f));
    float nearF = saturate((dist - 0.35f) * 2.0f);
    float a = 0.32f * edge * nearF * (1.0f - kill);
    if (a <= 0.001f) wp = p; // quad degenerado: rasterizador descarta

    o.pos = mul(float4(wp, 1.0f), RainViewProj);
    o.uv  = float2(cu * 0.5f + 0.5f, cv * 0.5f + 0.5f);

    // mesmo tint da cortina (key dessaturada c/ forward scatter + ambient do ceu)
    float3 dir    = -toCam / dist;
    float  fwd    = pow(saturate(dot(dir, KeyLightDir.xyz) * 0.5f + 0.5f), 4.0f);
    float  keyLum = dot(KeyLightColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float3 keyCol = lerp(keyLum.xxx, KeyLightColor.rgb, 0.35f);
    o.color = keyCol * lerp(0.04f, 0.14f, fwd) + SkyAmbientRain.rgb * 0.12f;
    o.alpha = a;
    return o;
}
