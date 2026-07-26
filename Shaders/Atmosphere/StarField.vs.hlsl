#include "AtmosphereCommon.hlsli"

// Passe de estrelas por catalogo real (Yale BSC via HYG, cozido em Assets/Sky/stars.sstars).
// Estilo CryEngine (mesh de estrelas com magnitude/cor reais) mas sem vertex buffer: um quad
// por estrela via DrawInstanced(6, N) lendo StructuredBuffer — quads de ~2px com gaussiana no
// PS ficam estaveis sob TAA/FSR, ao contrario do hash procedural de 1px que cintilava.
struct FStar {
    float3 Dir;        // frame do catalogo (polo celeste = +Y)
    float  Brightness; // 10^(-0.4*mag), mag 0 -> 1.0
    uint   Color;      // RGBA8 (cor do indice B-V)
};
StructuredBuffer<FStar> Stars : register(t0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0; // -1..1 no quad
    float3 col : TEXCOORD1; // cor * brilho * twinkle
    float3 dir : TEXCOORD2; // direcao no mundo (extincao/mascaras no PS)
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const float2 kCorners[6] = {
        float2(-1.0f, -1.0f), float2( 1.0f, -1.0f), float2(-1.0f,  1.0f),
        float2( 1.0f, -1.0f), float2( 1.0f,  1.0f), float2(-1.0f,  1.0f),
    };

    FStar  S    = Stars[iid];
    float3 dirW = normalize(mul(S.Dir, (float3x3)StarMatrix));

    VSOut o;
    float4 clip = mul(float4(dirW, 1.0f), ViewProjNoTrans);
    if (clip.w <= 1e-4f) { // atras da camera: manda pra fora do clip
        o.pos = float4(4.0f, 4.0f, 1.0f, 1.0f);
        o.uv = 0.0f; o.col = 0.0f; o.dir = dirW;
        return o;
    }

    // Cintilacao sutil por estrela (frequencia/fase do id; MoonParams.w = tempo real).
    float phase   = (float)iid * 1.7f;
    float freq    = 2.5f + frac((float)iid * 0.6180339f) * 3.5f;
    float twinkle = 0.82f + 0.18f * sin(MoonParams.w * freq + phase);

    float3 rgb = float3((S.Color) & 0xFF, (S.Color >> 8) & 0xFF, (S.Color >> 16) & 0xFF) / 255.0f;
    o.col = rgb * (S.Brightness * twinkle);

    // Quad em pixels: base ~2.2px + leve crescimento p/ estrelas brilhantes (estilo Cry).
    float sizePx = 2.2f + 1.4f * saturate(0.5f * log2(1.0f + S.Brightness));
    float2 cornerNdc = kCorners[vid] * (sizePx * 2.0f) /
                       float2(max(kViewportW, 1.0f), max(kViewportH, 1.0f));

    clip.xy += cornerNdc * clip.w;
    o.pos = clip;
    o.uv  = kCorners[vid];
    o.dir = dirW;
    return o;
}
