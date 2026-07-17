#ifndef SMILE_TERRAIN_COMMON_HLSLI
#define SMILE_TERRAIN_COMMON_HLSLI

// Terreno F1 (best-of Flax/UE): chunks de grid compartilhado por LOD, altura amostrada
// da heightmap (R16_UNORM, mips por DECIMACAO — mip N pega o texel 2N do mip 0, nunca
// media) e costura de LOD por morph geometrico continuo no VS, estilo Flax/CDLOD: cada
// vertice carrega pesos baricentricos por quadrante e morfa em direcao a grade do
// PROXIMO LOD perto da borda cujo vizinho e mais grosso. Sem skirts, sem permutacao de
// indices. Manter em sincronia com FTerrain (Terrain.h/.cpp).

cbuffer TerrainCB : register(b0) {
    row_major float4x4 TerrainViewProj;          // jittered (SV_POSITION)
    row_major float4x4 TerrainViewProjNoJitter;  // motion vector (atual)
    row_major float4x4 TerrainPrevViewProj;      // motion vector (anterior)
    float4 OriginUnits;   // xyz = posicao de mundo do texel (0,0), w = unidades por texel
    float4 TParams;       // x = escala de altura (mundo), y = texels da heightmap (mip 0),
                          // z = quads por chunk no LOD0, w = maxLod
    float4 TParams2;      // x = debug (0=off, 1=cores por LOD), y = cinza base do material,
                          // z = roughness base, w = camadas texturizadas (0/1)
    float4 TParams3;      // x = mip bias (FSR2), y = contraste do blend, z = escala do ruido
                          // de terra (1/m), w = quantidade de terra (0..1)
    float4 TParams4;      // xy = slope start/end da rocha (1 - N.y), zw = altura start/end
                          // da camada alta (mundo)
    float4 LayerTiling;   // 1/metros-por-tile, por camada (grama, terra, rocha, alta)
    float4 LayerRough;    // roughness por camada
    float4 CamPosMacro;   // xyz = posicao da camera (mundo), w = intensidade da macro
                          // variation (0 desliga o anti-tiling de tinte)
};

// Constantes por chunk (root constants — 8 dwords, sem CB por chunk).
cbuffer ChunkCB : register(b1) {
    uint2  ChunkCoord;   // indice do chunk na grade
    uint   ChunkLod;
    uint   _ChunkPad;
    float4 NeighborLod;  // LOD absoluto do vizinho, clampado a [lod, lod+1]:
                         // x = z-1, y = x-1, z = x+1, w = z+1 (mapeia os pesos do morph)
};

Texture2D<float> Heightmap : register(t0);
SamplerState TerrainLinearClamp : register(s0);

// LOD continuo por vertice (Flax): pesos baricentricos escolhem o quadrante e
// interpolam entre o LOD do chunk e o do vizinho daquele lado.
float TerrainCalcLod(float2 uv, float4 morph) {
    float4 lodCalculated = morph * (float)ChunkLod + NeighborLod * (1.0f - morph);
    float lod;
    if ((uv.x + uv.y) > 1.0f) {
        if (uv.x < uv.y) lod = lodCalculated.w; // borda z+1
        else             lod = lodCalculated.z; // borda x+1
    } else {
        if (uv.x < uv.y) lod = lodCalculated.y; // borda x-1
        else             lod = lodCalculated.x; // borda z-1
    }
    return lod;
}

// Posicao LOCAL do vertice (em texels de mip0 no plano XZ, altura normalizada em Y),
// ja com o morph aplicado. uv = coordenada [0,1] dentro do chunk (grade do LOD do chunk).
float3 TerrainVertexLocal(float2 uv, float4 morph) {
    const uint  chunkQuadsLod0 = (uint)TParams.z;
    const uint  quads  = max(chunkQuadsLod0 >> ChunkLod, 1u);
    const uint  size0  = (uint)TParams.y;

    const float lodF       = TerrainCalcLod(uv, morph);
    const float morphAlpha = saturate(lodF - (float)ChunkLod);

    // Altura no LOD do chunk: texel exato do mip ChunkLod (vertice == texel).
    const uint  mipSize  = max(size0 >> ChunkLod, 1u);
    const uint2 texel    = min(ChunkCoord * quads + (uint2)round(uv * quads),
                               uint2(mipSize - 1u, mipSize - 1u));
    const float h0       = Heightmap.Load(int3(texel, ChunkLod));

    // Alvo do morph: a grade do PROXIMO LOD (uv arredondado pro vertice par) amostrada
    // do mip seguinte — decimacao garante altura identica a que o LOD+1 renderiza.
    const uint  quadsNext   = max(quads >> 1, 1u);
    const float2 uvNext     = round(uv * quadsNext) / (float)quadsNext;
    const uint  mipSizeNext = max(size0 >> (ChunkLod + 1u), 1u);
    const uint2 texelNext   = min(ChunkCoord * quadsNext + (uint2)round(uvNext * quadsNext),
                                  uint2(mipSizeNext - 1u, mipSizeNext - 1u));
    const float h1          = Heightmap.Load(int3(texelNext, ChunkLod + 1u));

    const float2 uvMorphed = lerp(uv, uvNext, morphAlpha);
    const float  h         = lerp(h0, h1, morphAlpha);

    const float2 texelXZ = ((float2)ChunkCoord + uvMorphed) * (float)chunkQuadsLod0;
    return float3(texelXZ.x, h, texelXZ.y);
}

// Local (texels/altura normalizada) -> mundo.
float3 TerrainLocalToWorld(float3 local) {
    return float3(OriginUnits.x + local.x * OriginUnits.w,
                  OriginUnits.y + local.y * TParams.x,
                  OriginUnits.z + local.z * OriginUnits.w);
}

// Normal por pixel via diferencas centrais na heightmap (bilinear, mip 0) — terreno
// nitido sem normal map e sem custo de vertice.
float3 TerrainNormal(float2 worldXZ) {
    const float sizeTexels = TParams.y;
    const float unitsPerTexel = OriginUnits.w;
    const float2 uv = (worldXZ - OriginUnits.xz) / (sizeTexels * unitsPerTexel);
    const float texelUV = 1.0f / sizeTexels;

    const float hL = Heightmap.SampleLevel(TerrainLinearClamp, uv - float2(texelUV, 0.0f), 0);
    const float hR = Heightmap.SampleLevel(TerrainLinearClamp, uv + float2(texelUV, 0.0f), 0);
    const float hD = Heightmap.SampleLevel(TerrainLinearClamp, uv - float2(0.0f, texelUV), 0);
    const float hU = Heightmap.SampleLevel(TerrainLinearClamp, uv + float2(0.0f, texelUV), 0);

    const float heightScale = TParams.x;
    return normalize(float3((hL - hR) * heightScale,
                            2.0f * unitsPerTexel,
                            (hD - hU) * heightScale));
}

#endif
