#ifndef SMILE_MESHLIGHT_COMMON_HLSLI
#define SMILE_MESHLIGHT_COMMON_HLSLI

// Contrato compartilhado com FMeshLights (MeshLights.h). Os dois lados tem static_assert de
// tamanho; mexer num sem o outro corrompe o buffer em silencio.

// Uma malha emissiva a extrair. A transform vem como 3 linhas da Mat44 TRANSPOSTA, igual ao que
// o RaytracingScene monta para o TLAS — mesma convencao, para nao existirem duas no projeto.
struct FMeshLightTask {
    uint   InstanceIndex;  // indice no buffer de InstanceGeo (== indice do renderable)
    uint   TriangleCount;
    uint   LightOffset;    // primeiro slot deste mesh no buffer de luzes
    uint   Pad;
    float4 Row0;
    float4 Row1;
    float4 Row2;
};

// 32 bytes, mesmo orcamento do RTXDI e do Falcor. As arestas vao em fp16 porque sao RELATIVAS a
// base: a magnitude e pequena e o erro relativo de ~2^-11 da milimetros numa aresta de metros.
struct FTriangleLightGPU {
    float3 Base;     // p0 em mundo, fp32 (a posicao absoluta e que precisa de alcance)
    uint   Edges0;   // e0.x | e1.x, fp16 intercalado (empacotamento do Falcor)
    uint   Edges1;   // e0.y | e1.y
    uint   Edges2;   // e0.z | e1.z
    uint   Radiance; // RGB9E5
    float  Flux;     // area * pi * luminancia — potencia, base da alias table da fase 4
};

// RGB9E5: expoente compartilhado, 9 bits de mantissa por canal. Escolhido em vez de R11G11B10
// porque radiancia emissiva e HDR de alcance largo (um letreiro pode ser 1e4 vezes uma lampada),
// e o expoente compartilhado preserva a razao entre canais melhor que mantissas curtas.
uint MeshLight_PackRGB9E5(float3 c) {
    c = clamp(c, 0.0f, 65408.0f);
    const float maxc = max(c.r, max(c.g, c.b));
    int e = (maxc > 1e-8f) ? (int)ceil(log2(maxc)) : -15;
    e = clamp(e, -15, 16);
    float denom = exp2((float)e - 9.0f);
    // O arredondamento pode empurrar o maior canal para 512, que nao cabe em 9 bits.
    if ((int)floor(maxc / denom + 0.5f) >= 512) { denom *= 2.0f; ++e; }
    const uint3 q = (uint3)clamp(floor(c / denom + 0.5f), 0.0f, 511.0f);
    return q.r | (q.g << 9) | (q.b << 18) | ((uint)(e + 15) << 27);
}

float3 MeshLight_UnpackRGB9E5(uint p) {
    const float s = exp2((float)(p >> 27) - 15.0f - 9.0f);
    return float3(p & 511u, (p >> 9) & 511u, (p >> 18) & 511u) * s;
}

float MeshLight_Luminance(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 MeshLight_Edge0(FTriangleLightGPU t) {
    return float3(f16tof32(t.Edges0), f16tof32(t.Edges1), f16tof32(t.Edges2));
}

float3 MeshLight_Edge1(FTriangleLightGPU t) {
    return float3(f16tof32(t.Edges0 >> 16), f16tof32(t.Edges1 >> 16), f16tof32(t.Edges2 >> 16));
}

#endif
