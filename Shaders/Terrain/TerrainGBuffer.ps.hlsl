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

// Value noise 2D barato (2 oitavas) p/ os patches de terra — sem textura de ruido.
float Hash2(float2 p) {
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
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
float Fbm2(float2 p) {
    return ValueNoise(p) * 0.667f + ValueNoise(p * 2.03f) * 0.333f;
}

// Pesos das 4 camadas: x = grama, y = terra, z = rocha, w = alta.
float4 LayerWeights(float3 worldPos, float3 geoN) {
    const float slope = 1.0f - saturate(geoN.y);

    float wRock = smoothstep(TParams4.x, TParams4.y, slope);
    const float dirtNoise = Fbm2(worldPos.xz * TParams3.z);
    float wDirt = TParams3.w * smoothstep(0.45f, 0.62f, dirtNoise) * (1.0f - wRock);
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
        const float2 uvG = input.worldPos.xz * LayerTiling.x;
        const float2 uvD = input.worldPos.xz * LayerTiling.y;
        const float2 uvH = input.worldPos.xz * LayerTiling.w;

        // Rocha: albedo triplanar (3 projecoes pesadas por |N|) — penhasco sem stretching.
        float3 tw = pow(abs(GeoN), 4.0f);
        tw /= dot(tw, 1.0f);
        const float  tileR = LayerTiling.z;
        const float3 rockA =
            tw.x * LayerAlbedo2.SampleBias(TerrainAnisoWrap, input.worldPos.zy * tileR, mipBias).rgb +
            tw.y * LayerAlbedo2.SampleBias(TerrainAnisoWrap, input.worldPos.xz * tileR, mipBias).rgb +
            tw.z * LayerAlbedo2.SampleBias(TerrainAnisoWrap, input.worldPos.xy * tileR, mipBias).rgb;

        baseColor = w.x * LayerAlbedo0.SampleBias(TerrainAnisoWrap, uvG, mipBias).rgb +
                    w.y * LayerAlbedo1.SampleBias(TerrainAnisoWrap, uvD, mipBias).rgb +
                    w.z * rockA +
                    w.w * LayerAlbedo3.SampleBias(TerrainAnisoWrap, uvH, mipBias).rgb;

        // Normal de detalhe: acumula em tangent space (planar XZ; rocha idem — ver nota
        // no topo) e transforma UMA vez pela TBN do heightfield.
        float3 nts =
            w.x * (LayerNormal0.SampleBias(TerrainAnisoWrap, uvG, mipBias).rgb * 2.0f - 1.0f) +
            w.y * (LayerNormal1.SampleBias(TerrainAnisoWrap, uvD, mipBias).rgb * 2.0f - 1.0f) +
            w.z * (LayerNormal2.SampleBias(TerrainAnisoWrap, input.worldPos.xz * tileR, mipBias).rgb * 2.0f - 1.0f) +
            w.w * (LayerNormal3.SampleBias(TerrainAnisoWrap, uvH, mipBias).rgb * 2.0f - 1.0f);
        nts = normalize(nts);

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
                                    float3(0.0f, 0.0f, 0.0f), SMILE_SHADINGMODEL_DEFAULTLIT);

    const float2 curNDC  = input.curClip.xy  / input.curClip.w;
    const float2 prevNDC = input.prevClip.xy / input.prevClip.w;
    const float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    const float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    o.Velocity = curUV - prevUV;
    return o;
}
