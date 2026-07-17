#include "TerrainCommon.hlsli"
#include "../GBuffer.hlsli"

// Geometry pass do terreno: material F1 (cinza uniforme, dieletrico) + normal por pixel
// da heightmap. Debug: cores por LOD (TParams2.x = 1) p/ validar selecao/costura.

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

GBufferOutput main(PSInput input) {
    const float3 N = TerrainNormal(input.worldPos.xz);

    float3 baseColor = TParams2.y.xxx;
    if (TParams2.x > 0.5f)
        baseColor = kLodDebugColors[min(ChunkLod, 7u)];

    GBufferOutput o = EncodeGBuffer(baseColor, 1.0f, N, TParams2.z, 0.0f,
                                    float3(0.0f, 0.0f, 0.0f), SMILE_SHADINGMODEL_DEFAULTLIT);

    const float2 curNDC  = input.curClip.xy  / input.curClip.w;
    const float2 prevNDC = input.prevClip.xy / input.prevClip.w;
    const float2 curUV   = float2(curNDC.x  * 0.5f + 0.5f, 0.5f - curNDC.y  * 0.5f);
    const float2 prevUV  = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    o.Velocity = curUV - prevUV;
    return o;
}
