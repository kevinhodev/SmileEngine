// Teste de oclusao contra o HZB — 1 thread por renderable (port da matematica do
// HZBTestPS + NaniteHZBCull da UE). Projeta os 8 cantos do AABB com a ViewProjection
// SEM jitter do MESMO frame que gerou o depth do HZB (teste 100% consistente; o
// resultado e consumido pela CPU kFramesInFlight frames depois via readback).
//
// Regras conservadoras (nunca cullar o que nao da pra afirmar ocludido):
//  - cruza/atras do plano near  -> visivel
//  - fora do frustum lateral/far -> visivel (o frustum cull da CPU decide por frame)
//  - retangulo cabe em <=4x4 texels do mip escolhido; farthest = MIN da regiao
//    (reverse-Z: far = 0); visivel sse nearest da caixa >= farthest do HZB.

struct FBounds {
    float4 Center; // .xyz centro world-space
    float4 Extent; // .xyz meia-extensao; tudo 0 = entrada nula (sempre visivel)
};

cbuffer TestCB : register(b0) {
    row_major float4x4 ViewProj; // world -> clip, sem jitter (convencao row-vector)
    float2   RenderSize; // resolucao de render (area valida do depth)
    uint     ObjectCount;
    uint     NumMips;
    float    DepthBias;  // folga em NDC reverse-Z (0 = teste exato)
    float3   _Pad;
};

StructuredBuffer<FBounds> Bounds     : register(t0);
Texture2D<float>          HZB        : register(t1);
RWStructuredBuffer<uint>  VisibleOut : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const uint i = DTid.x;
    if (i >= ObjectCount) return;

    const float3 C = Bounds[i].Center.xyz;
    const float3 E = Bounds[i].Extent.xyz;
    if (all(E <= 0.0)) { VisibleOut[i] = 1u; return; }

    float3 NdcMin = 1e9;
    float3 NdcMax = -1e9;
    [unroll]
    for (uint c = 0; c < 8; ++c) {
        const float3 Corner = C + E * (float3(c & 1, (c >> 1) & 1, (c >> 2) & 1) * 2.0 - 1.0);
        const float4 Clip   = mul(float4(Corner, 1.0), ViewProj);
        if (Clip.w < 1e-4) { VisibleOut[i] = 1u; return; } // cruza o near
        const float3 Ndc = Clip.xyz / Clip.w;
        NdcMin = min(NdcMin, Ndc);
        NdcMax = max(NdcMax, Ndc);
    }

    if (NdcMax.x < -1.0 || NdcMin.x > 1.0 ||
        NdcMax.y < -1.0 || NdcMin.y > 1.0 || NdcMax.z <= 0.0) {
        VisibleOut[i] = 1u; // fora do frustum: oclusao nao opina
        return;
    }

    // NDC -> retangulo de pixels full-res por centro de pixel (UE GetScreenRect):
    // so o pixel cujo CENTRO esta coberto rasteriza algo do retangulo.
    const float4 RectUV = saturate(float4(NdcMin.xy, NdcMax.xy)
                                   * float2(0.5, -0.5).xyxy + 0.5).xwzy;
    int4 Pixels = int4(RectUV * RenderSize.xyxy + float4(0.5, 0.5, -0.5, -0.5));
    Pixels.xy = max(Pixels.xy, int2(0, 0));
    Pixels.zw = min(Pixels.zw, int2(RenderSize) - 1);

    int4 Texels = int4(Pixels.xy, max(Pixels.xy, Pixels.zw));
    Texels >>= 1; // mip 0 do HZB = meia-res do render

    // Menor mip onde o retangulo cabe em <=4x4 texels (UE MipLevelForRect, footprint 4).
    const int2 MipXY = firstbithigh(Texels.zw - Texels.xy);
    int Mip = clamp(max(max(MipXY.x, MipXY.y) - 1, 0), 0, int(NumMips) - 1);
    if (any(((Texels.zw >> Mip) - (Texels.xy >> Mip)) > 3))
        Mip = min(Mip + 1, int(NumMips) - 1);
    Texels >>= Mip;

    float Farthest = 1.0;
    for (int y = Texels.y; y <= Texels.w; ++y)
        for (int x = Texels.x; x <= Texels.z; ++x)
            Farthest = min(Farthest, HZB.Load(int3(x, y, Mip)));

    // Reverse-Z: visivel se o ponto mais proximo da caixa nao fica atras do
    // occluder mais distante da regiao.
    VisibleOut[i] = (NdcMax.z + DepthBias >= Farthest) ? 1u : 0u;
}
