#ifndef SMILE_PT_RESERVOIR_HLSLI
#define SMILE_PT_RESERVOIR_HLSLI

// ReSTIR PT — reservoir de caminho empacotado (64B) em StructuredBuffer ping-pong + WRS.
//
// F1 usa o SUBCONJUNTO de reconexao: a amostra e o primeiro vertice indireto x_k (=x2) e a
// radiancia Lo que sai dele em direcao a x1 (o sufixo COMPLETO multi-bounce). A matematica de
// WRS/Jacobiano/pHat e a mesma do ReSTIR GI (Ouyang 2021) — reaproveitada aqui. Os campos q3 e o
// PathSeed ficam reservados p/ o random replay + lobe indices do F2 (sem re-layout depois).
//
// Requer: DDGICommon.hlsli (DDGI_OctEncode/Decode), Reflections/GGXSample.hlsli (GGX_PCG).

#include "../GI/DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"

struct PTReservoir {
    float3 x1;   // ponto primario (validacao temporal/espacial)
    float3 xk;   // vertice de reconexao (F1: primeiro vertice indireto x2)
    float3 nk;   // normal em xk (Jacobiano de reconexao)
    float3 Lo;   // radiancia que sai de xk rumo a x1 (sufixo completo)
    float  hitK; // |xk - x1| (hitDist do GI p/ o NRD, F4)
    float  M;    // contagem de amostras
    float  W;    // peso de contribuicao nao-enviesado
    float  wSum; // soma dos pesos de resampling
    uint   seed; // seed do random replay (reservado F2)
};

// 64B: q0 = x1|M, q1 = xk|W, q2 = Lo(3xf16)+hitK(f16) | nk oct(2xf16) | seed, q3 = reservado F2.
struct PTReservoirPacked {
    float4 q0; // x1.xyz (f32), M
    float4 q1; // xk.xyz (f32), W
    uint4  q2; // x=f16(Lo.r,Lo.g), y=f16(Lo.b,hitK), z=f16(nkOct.x,nkOct.y), w=seed
    uint4  q3; // reservado (F2: pdf_wk, lobe indices, bits de tecnica)
};

float PT_Lum(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// RNG com estado p/ a selecao estocastica do WRS (stream separado do RNG de caminho).
uint  PTResRngSeed(uint2 px, uint frame, uint salt) {
    return GGX_PCG(px.x + GGX_PCG(px.y + GGX_PCG(frame + salt * 0x1000193u)));
}
float PTResRngNext(inout uint s) { s = GGX_PCG(s); return (s & 0x00FFFFFFu) / 16777216.0f; }

void PTResInit(out PTReservoir r) {
    r.x1 = 0; r.xk = 0; r.nk = 0; r.Lo = 0; r.hitK = 0; r.M = 0; r.W = 0; r.wSum = 0; r.seed = 0;
}

PTReservoirPacked PTResPack(PTReservoir r) {
    PTReservoirPacked p;
    p.q0 = float4(r.x1, r.M);
    p.q1 = float4(r.xk, r.W);
    float2 nkOct = DDGI_OctEncode(r.nk);
    p.q2.x = f32tof16(r.Lo.r) | (f32tof16(r.Lo.g) << 16);
    p.q2.y = f32tof16(r.Lo.b) | (f32tof16(r.hitK) << 16);
    p.q2.z = f32tof16(nkOct.x) | (f32tof16(nkOct.y) << 16);
    p.q2.w = r.seed;
    p.q3 = uint4(0, 0, 0, 0);
    return p;
}

PTReservoir PTResUnpack(PTReservoirPacked p) {
    PTReservoir r;
    r.x1 = p.q0.xyz; r.M = p.q0.w;
    r.xk = p.q1.xyz; r.W = p.q1.w;
    r.Lo = float3(f16tof32(p.q2.x & 0xFFFFu), f16tof32(p.q2.x >> 16), f16tof32(p.q2.y & 0xFFFFu));
    r.hitK = f16tof32(p.q2.y >> 16);
    float2 nkOct = float2(f16tof32(p.q2.z & 0xFFFFu), f16tof32(p.q2.z >> 16));
    r.nk = DDGI_OctDecode(nkOct);
    r.seed = p.q2.w;
    r.wSum = 0.0f; // nao persistido (recomputado por frame)
    return r;
}

// pHat alvo p/ (x1,n1) dada a amostra (xk,Lo): luminancia * cosTheta1 (albedo difuso cancela).
float PTTargetPHat(float3 x1, float3 n1, float3 xk, float3 Lo) {
    float3 d = xk - x1;
    float  l = length(d);
    if (l < 1e-4f) return 0.0f;
    float cosT = saturate(dot(n1, d / l));
    return PT_Lum(Lo) * cosT;
}

// Adiciona um candidato (M=1) com peso de resampling w. x1C = o primario do pixel atual. NOTA:
// o reservoir persistido e SEMPRE re-ancorado no x1 do pixel que o escreve (PTInitial faz
// r.x1 = s1.Pos apos o finalize) — W ja embute o Jacobiano do shift aplicado no merge, entao o
// proximo frame deve calcular J contra o x1 de ONTEM, nunca contra o x1 ancestral da amostra
// (isso comporia o Jacobiano frame a frame = fireflies).
void PTResUpdate(inout PTReservoir r, float3 x1C, float3 xkC, float3 nkC, float3 LoC, float hitC,
                 uint seedC, float w, inout uint rng) {
    r.wSum += w;
    r.M    += 1.0f;
    if (w > 0.0f && PTResRngNext(rng) * r.wSum <= w) {
        r.x1 = x1C; r.xk = xkC; r.nk = nkC; r.Lo = LoC; r.hitK = hitC; r.seed = seedC;
    }
}

// Funde um reservoir inteiro (other) no atual. pHatOther = pHat (do pixel atual) p/ a amostra de
// other; J = Jacobiano do shift other.x1 -> x1 atual (rejeitado pelo caller se fora da faixa).
void PTResMerge(inout PTReservoir r, PTReservoir other, float pHatOther, float J, inout uint rng) {
    float w = pHatOther * other.W * other.M * J;
    r.wSum += w;
    r.M    += other.M;
    if (w > 0.0f && PTResRngNext(rng) * r.wSum <= w) {
        r.x1 = other.x1; r.xk = other.xk; r.nk = other.nk;
        r.Lo = other.Lo; r.hitK = other.hitK; r.seed = other.seed;
    }
}

// Jacobiano de reconexao (Ouyang 2021 / GRIS Eq. 52 em solid angle): reusar xk (normal nk) de um
// pixel visivel x1Src no pixel x1Dst. Clampado pelo caller. J=1 no temporal (mesma superficie).
float PTReconnectionJacobian(float3 x1Dst, float3 x1Src, float3 xk, float3 nk) {
    float3 dDst = xk - x1Dst; float lDst2 = dot(dDst, dDst);
    float3 dSrc = xk - x1Src; float lSrc2 = dot(dSrc, dSrc);
    if (lDst2 < 1e-8f || lSrc2 < 1e-8f) return 0.0f;
    float lDst = sqrt(lDst2), lSrc = sqrt(lSrc2);
    float cosDst = abs(dot(nk, -dDst / lDst));
    float cosSrc = abs(dot(nk, -dSrc / lSrc));
    if (cosSrc < 1e-4f) return 0.0f;
    return (cosDst / cosSrc) * (lSrc2 / lDst2);
}

// Finaliza W = wSum / (M * pHatSel). No F2 o caller passa o pHat BRDF-aware (PT_PHatBrdf) do
// pixel atual p/ a amostra selecionada — mantem W consistente com o target function usado no merge.
void PTResFinalizeW(inout PTReservoir r, float pHatSel) {
    r.W = (pHatSel > 0.0f && r.M > 0.0f) ? (r.wSum / (r.M * pHatSel)) : 0.0f;
}

#endif
