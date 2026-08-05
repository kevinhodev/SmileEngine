#include "TerrainCommon.hlsli"
#include "../GBuffer.hlsli"

// Geometry pass do terreno — F2: 4 camadas texturizadas (grama, terra, rocha, alta)
// com pesos PROCEDURAIS (declive p/ rocha, ruido p/ terra, altitude p/ camada alta —
// nao ha splatmap pintada ate o editor ganhar sculpt/paint), contraste de blend p/
// bordas definidas, triplanar no ALBEDO da rocha (mata o stretching em penhasco; a
// normal da rocha fica planar top-down — compromisso documentado, F2.5 se incomodar)
// e normal de detalhe por camada blendada em tangent space. Sem camadas (TParams2.w=0)
// cai no material cinza da F1. Debug: cores por LOD (TParams2.x = 1).

Texture2D LayerAlbedo0 : register(t1); // grama
Texture2D LayerAlbedo1 : register(t2); // terra
Texture2D LayerAlbedo2 : register(t3); // rocha
Texture2D LayerAlbedo3 : register(t4); // alta (cascalho/scree)
Texture2D LayerNormal0 : register(t5);
Texture2D LayerNormal1 : register(t6);
Texture2D LayerNormal2 : register(t7);
Texture2D LayerNormal3 : register(t8);

SamplerState TerrainAnisoWrap : register(s1);

struct PSInput {
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

static const float3 kLodDebugColors[8] = {
    float3(1.0f, 1.0f, 1.0f), // LOD0 branco
    float3(0.2f, 0.8f, 0.2f), // 1 verde
    float3(0.2f, 0.4f, 1.0f), // 2 azul
    float3(1.0f, 1.0f, 0.2f), // 3 amarelo
    float3(1.0f, 0.5f, 0.1f), // 4 laranja
    float3(1.0f, 0.2f, 0.2f), // 5 vermelho
    float3(0.8f, 0.2f, 1.0f), // 6 roxo
    float3(0.2f, 1.0f, 1.0f), // 7 ciano
};

// Value noise 2D barato p/ os patches de terra e a macro variation — sem textura de
// ruido. 3 oitavas ROTACIONADAS entre si: mata os losangos axis-aligned da
// interpolacao bilinear (F2 usava 2 oitavas alinhadas — patches poligonais duros).
//
// Hash INTEIRO (bit-mix de 32 bits), e nao o classico frac(sin(x)*43758): aquele
// amplifica em ~4e4 qualquer diferenca entre o sin do hardware e o de outra
// implementacao, entao era impossivel reproduzi-lo fora do shader. O bake do albedo do
// proxy de RT (FTerrain::BakeProxyAlbedo) precisa gerar EXATAMENTE este campo, senao as
// manchas de terra e a macro variation ficam num lugar na tela e noutro no GI. Operacao
// inteira de 32 bits tem semantica identica em HLSL e C++, incluindo o wraparound.
//
// A entrada e sempre inteira (vem do floor() do ValueNoise), entao o cast p/ int e exato.
// A saida usa so 24 bits: uint de 24 bits cabe exato na mantissa do float e a escala e
// potencia de 2, entao a conversao nao arredonda — nao sobra nem 1 ULP de divergencia.
uint HashUint2(int2 v) {
    uint h = (uint)v.x * 0x9E3779B1u ^ (uint)v.y * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
float Hash2(float2 p) {
    return (float)(HashUint2((int2)p) >> 8) * (1.0f / 16777216.0f);
}
float ValueNoise(float2 p) {
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float a = Hash2(i);
    const float b = Hash2(i + float2(1, 0));
    const float c = Hash2(i + float2(0, 1));
    const float d = Hash2(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float Fbm3(float2 p) {
    const float2x2 R = float2x2(0.8f, -0.6f, 0.6f, 0.8f); // rotacao ~37 graus por oitava
    float v = ValueNoise(p) * 0.5f;
    p = mul(R, p) * 2.03f;
    v += ValueNoise(p) * 0.3f;
    p = mul(R, p) * 1.97f;
    v += ValueNoise(p) * 0.2f;
    return v;
}

// Pesos das 4 camadas: x = grama, y = terra, z = rocha, w = alta.
float4 LayerWeights(float3 worldPos, float3 geoN) {
    const float slope = 1.0f - saturate(geoN.y);

    float wRock = smoothstep(TParams4.x, TParams4.y, slope);
    const float dirtNoise = Fbm3(worldPos.xz * TParams3.z);
    float wDirt = TParams3.w * smoothstep(0.42f, 0.66f, dirtNoise) * (1.0f - wRock);
    float wHigh = smoothstep(TParams4.z, TParams4.w, worldPos.y) * (1.0f - wRock * 0.5f);
    float wGrass = saturate(1.0f - wRock - wDirt - wHigh);

    float4 w = float4(wGrass, wDirt, wRock, wHigh);
    w = pow(max(w, 1e-4f), TParams3.y); // contraste: transicoes curtas, sem "sopa" de blend
    return w / dot(w, 1.0f);
}

GBufferOutput main(PSInput input) {
    const float3 GeoN = TerrainNormal(input.worldPos.xz);

    float3 baseColor;
    float3 N = GeoN;
    float  roughness = TParams2.z;

    if (TParams2.w > 0.5f) {
        const float4 w = LayerWeights(input.worldPos, GeoN);
        const float mipBias = TParams3.x;

        // Anti-tiling por distancia (F2.5): perto usa o tile normal; longe blenda com a
        // MESMA textura em escala ~5.7x maior (quebra o periodo) e o detalhe da normal
        // faz fade (a media da normal mippada vira flat mesmo — só formaliza).
        const float dist      = distance(CamPosMacro.xyz, input.worldPos);
        const float farBlend  = smoothstep(30.0f, 130.0f, dist);
        const float detailFade = 1.0f - smoothstep(60.0f, 250.0f, dist);
        const float kFarScale = 0.173f;

        const float2 uvG = input.worldPos.xz * LayerTiling.x;
        const float2 uvD = input.worldPos.xz * LayerTiling.y;
        const float2 uvH = input.worldPos.xz * LayerTiling.w;

        float3 albG = lerp(LayerAlbedo0.SampleBias(TerrainAnisoWrap, uvG, mipBias).rgb,
                           LayerAlbedo0.SampleBias(TerrainAnisoWrap, uvG * kFarScale, mipBias).rgb,
                           farBlend);
        float3 albD = lerp(LayerAlbedo1.SampleBias(TerrainAnisoWrap, uvD, mipBias).rgb,
                           LayerAlbedo1.SampleBias(TerrainAnisoWrap, uvD * kFarScale, mipBias).rgb,
                           farBlend);
        float3 albH = lerp(LayerAlbedo3.SampleBias(TerrainAnisoWrap, uvH, mipBias).rgb,
                           LayerAlbedo3.SampleBias(TerrainAnisoWrap, uvH * kFarScale, mipBias).rgb,
                           farBlend);

        // Rocha: albedo triplanar (3 projecoes pesadas por |N|) — penhasco sem stretching.
        // Fica em escala unica: a repeticao some no relevo irregular da montanha.
        float3 tw = pow(abs(GeoN), 4.0f);
        tw /= dot(tw, 1.0f);
        const float  tileR = LayerTiling.z;
        const float3 rockA =
            tw.x * LayerAlbedo2.SampleBias(TerrainAnisoWrap, input.worldPos.zy * tileR, mipBias).rgb +
            tw.y * LayerAlbedo2.SampleBias(TerrainAnisoWrap, input.worldPos.xz * tileR, mipBias).rgb +
            tw.z * LayerAlbedo2.SampleBias(TerrainAnisoWrap, input.worldPos.xy * tileR, mipBias).rgb;

        baseColor = w.x * albG + w.y * albD + w.z * rockA + w.w * albH;

        // Macro variation (F2.5/F2.6): quebra o padrao do tiling em escala grande.
        // (a) BRILHO em 2 escalas (~137/31 m via oitavas do Fbm3) — media/longa distancia.
        // (b) MATIZ em escala media (~23 m) p/ o PRIMEIRO PLANO, onde o anti-tiling por
        //     distancia ainda nao agiu e a grama lia como "carpete" de tom unico:
        //     manchas secas (palha, dessatura o verde) x vicosas (verde mais fundo/frio),
        //     pesado pela camada de grama (rocha/terra ficam de fora) e pelo mesmo slider
        //     da macro, com boost fixo p/ ficar perceptivel de perto.
        if (CamPosMacro.w > 0.0f) {
            const float macro = Fbm3(input.worldPos.xz * (1.0f / 137.0f));
            baseColor *= 1.0f + (macro - 0.5f) * 2.0f * CamPosMacro.w;

            const float  tintN     = Fbm3(input.worldPos.xz * (1.0f / 23.0f));
            const float3 dryTint   = float3(1.12f, 1.04f, 0.74f); // seco: puxa palha
            const float3 lushTint  = float3(0.88f, 1.00f, 0.90f); // vicoso: verde frio
            const float3 grassTint = lerp(lushTint, dryTint, smoothstep(0.35f, 0.72f, tintN));
            const float  hueStr    = saturate(CamPosMacro.w * 2.0f);
            baseColor *= lerp(float3(1.0f, 1.0f, 1.0f), grassTint, w.x * hueStr);
        }

        // Normal de detalhe: acumula em tangent space (planar XZ; rocha idem — ver nota
        // no topo), faz fade por distancia e transforma UMA vez pela TBN do heightfield.
        float3 nts =
            w.x * (LayerNormal0.SampleBias(TerrainAnisoWrap, uvG, mipBias).rgb * 2.0f - 1.0f) +
            w.y * (LayerNormal1.SampleBias(TerrainAnisoWrap, uvD, mipBias).rgb * 2.0f - 1.0f) +
            w.z * (LayerNormal2.SampleBias(TerrainAnisoWrap, input.worldPos.xz * tileR, mipBias).rgb * 2.0f - 1.0f) +
            w.w * (LayerNormal3.SampleBias(TerrainAnisoWrap, uvH, mipBias).rgb * 2.0f - 1.0f);
        nts = normalize(lerp(float3(0.0f, 0.0f, 1.0f), normalize(nts), detailFade));

        // TBN do heightfield: T no eixo X projetado, B = cross(N, T) — convencao NormalGL.
        const float3 T = normalize(float3(1.0f, 0.0f, 0.0f) - GeoN * GeoN.x);
        const float3 B = cross(GeoN, T);
        N = normalize(nts.x * T + nts.y * B + nts.z * GeoN);

        roughness = dot(w, LayerRough);
    } else {
        baseColor = TParams2.y.xxx;
    }

    if (TParams2.x > 0.5f)
        baseColor = kLodDebugColors[min(ChunkLod, 7u)];

    GBufferOutput o = EncodeGBuffer(baseColor, 1.0f, N, roughness, 0.0f,
                                    float3(0.0f, 0.0f, 0.0f), SMILE_SHADINGMODEL_DEFAULTLIT,
                                    float3(0.0f, 0.0f, 0.0f));

    const float2 curNDC  = input.curClip.xy  / input.curClip.w;
    const float2 prevNDC = input.prevClip.xy / input.prevClip.w;
    const float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    const float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    o.Velocity = curUV - prevUV;
    return o;
}
