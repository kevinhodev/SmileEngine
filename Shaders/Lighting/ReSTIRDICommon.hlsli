#ifndef SMILE_RESTIR_DI_COMMON_HLSLI
#define SMILE_RESTIR_DI_COMMON_HLSLI

// Contrato do LightBuffer do deferred. O ReSTIR DI amostra o conjunto LOCAL inteiro;
// SpotParams.w preserva o pedido CastShadows do artista, tenha a luz recebido slice raster ou nao.
struct FGPULightFull {
    float4 PosInvRadius;
    float4 ColorSourceRadius;
    float4 DirCosOuter;
    float4 SpotParams;
    row_major float4x4 ShadowMatrix;
    float4 PrevPosInvRadius;
};

struct ReSTIRDIReservoir {
    uint   LightIndex; // indice no pool COMBINADO: < analyticCount = analitica, acima = triangulo
    float2 UV;         // parametro da amostra na luz. Reusar exige reamostrar no pixel de destino
                       // com o MESMO uv: para triangulo isso reconstroi o ponto exato; para esfera
                       // o cone e construido em torno da direcao local, entao o ponto e o
                       // equivalente no dominio do destino (mesmo shift map do RTXDI).
    float  M;
    float  W;
    float  WeightSum;
    float3 X1;
    float2 N1Oct;
};

float DI_Luminance(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float DI_RandNext(inout uint state) {
    state = GGX_PCG(state);
    return (state & 0x00FFFFFFu) / 16777216.0f;
}

void DIResInit(out ReSTIRDIReservoir r) {
    r.LightIndex = 0xFFFFFFFFu;
    r.UV = 0.0f;
    r.M = 0.0f;
    r.W = 0.0f;
    r.WeightSum = 0.0f;
    r.X1 = 0.0f;
    r.N1Oct = 0.0f;
}

// Um candidato da proposta inicial. M cresce inclusive para peso zero: M e o numero de amostras
// vistas pelo stream, nao o numero das que contribuiram (cap. 22 e Alg. 3 do paper original).
bool DIResUpdate(inout ReSTIRDIReservoir r, uint lightIndex, float2 uv, float weight,
                 inout uint rng) {
    r.M += 1.0f;
    r.WeightSum += weight;
    if (weight > 0.0f && DI_RandNext(rng) * r.WeightSum < weight) {
        r.LightIndex = lightIndex;
        r.UV = uv;
        return true;
    }
    return false;
}

// Combina o stream inteiro de `other` em O(1). Para amostra discreta de luz nao existe Jacobiano
// de reconexao: o dominio e o indice da luz; so a target PDF e reavaliada no pixel de destino.
bool DIResMerge(inout ReSTIRDIReservoir r, ReSTIRDIReservoir other,
                float targetAtDestination, inout uint rng) {
    const float weight = targetAtDestination * other.W * other.M;
    r.M += other.M;
    r.WeightSum += weight;
    if (weight > 0.0f && DI_RandNext(rng) * r.WeightSum < weight) {
        r.LightIndex = other.LightIndex;
        r.UV = other.UV;
        return true;
    }
    return false;
}

void DIResFinalize(inout ReSTIRDIReservoir r, float selectedTarget) {
    r.W = (r.LightIndex != 0xFFFFFFFFu && r.M > 0.0f && selectedTarget > 0.0f)
        ? r.WeightSum / (r.M * selectedTarget) : 0.0f;
}

// ResB e R32G32B32A32_UINT. Layout:
//   .x = LightIndex, 32 bits (0xFFFFFFFF = sem amostra)
//   .y = UV da amostra na superficie da luz, unorm 16+16. Um triangulo emissivo nao e identificado
//        so pelo indice: o reuso reamostra com este MESMO uv no pixel de destino.
//   .z = M (16 bits) | idade (8 bits) | livre (8)
//   .w = normal do ponto visivel, octaedrica em unorm 16+16
//
// Era RGBA16F, e o formato antigo tinha DOIS tetos que ja doiam: o LightIndex em fp16 e exato so
// ate 2048 (a cena de teste tem 26 mil triangulos emissivos, entao mesh lights quebravam na hora),
// e M + idade dividiam um canal, prendendo M em 63 — o que impedia o MCap relativo do paper
// (20x o M do frame atual = 160 com 8 candidatas). Custa 8 B/pixel a mais por buffer.
uint DI_PackUnorm16x2(float2 v) {
    const uint2 q = (uint2)(saturate(v * 0.5f + 0.5f) * 65535.0f + 0.5f);
    return q.x | (q.y << 16);
}

float2 DI_UnpackUnorm16x2(uint p) {
    return float2(p & 0xFFFFu, p >> 16) * (2.0f / 65535.0f) - 1.0f;
}

uint DI_PackMAge(float M, float age) {
    return min((uint)(M + 0.5f), 65535u) | (min((uint)max(age, 0.0f), 255u) << 16);
}

void DI_UnpackMAge(uint packed, out float M, out float age) {
    M   = (float)(packed & 0xFFFFu);
    age = (float)((packed >> 16) & 0xFFu);
}

ReSTIRDIReservoir DI_LoadReservoir(float4 a, uint4 b) {
    ReSTIRDIReservoir r;
    DIResInit(r);
    r.X1 = a.xyz;
    r.W = a.w;
    r.LightIndex = b.x;
    // UV vive em [0,1], entao usa o unorm direto em vez do mapeamento [-1,1] da normal.
    r.UV = float2(b.y & 0xFFFFu, b.y >> 16) * (1.0f / 65535.0f);
    float age;
    DI_UnpackMAge(b.z, r.M, age);
    r.N1Oct = DI_UnpackUnorm16x2(b.w);
    return r;
}

void DI_StoreReservoir(ReSTIRDIReservoir r, float age,
                       out float4 a, out uint4 b) {
    a = float4(r.X1, r.W);
    const uint2 quv = (uint2)(saturate(r.UV) * 65535.0f + 0.5f);
    b = uint4(r.LightIndex, quv.x | (quv.y << 16), DI_PackMAge(r.M, age),
              DI_PackUnorm16x2(r.N1Oct));
}

bool DI_IsShadowCaster(FGPULightFull light) {
    return light.SpotParams.w > 0.5f;
}

// A amostragem da luz e a avaliacao da target PDF migraram para DILightSampling.hlsli quando o
// reservoir passou a viver em medida de ANGULO SOLIDO. Sairam daqui junto:
//   DI_Evaluate        — avaliava a luz como pontual, com 1/d^2 dentro da atenuacao;
//   DI_SampleLightPoint— sorteava um ponto na esfera para o raio de sombra;
//   DI_LightPointSeed  — fazia os dois passes concordarem no ponto sorteado.
// O ultimo nao e mais necessario: com a UV guardada no reservoir, os dois passes reamostram o
// MESMO ponto por construcao.

#endif
