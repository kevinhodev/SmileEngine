#ifndef SMILE_PT_RESERVOIR_HLSLI
#define SMILE_PT_RESERVOIR_HLSLI

// ReSTIR PT — reservoir de caminho empacotado (64B) em StructuredBuffer ping-pong + WRS.
//
// F2b: a amostra e um caminho na parametrizacao HIBRIDA — kb bounces em PSS (random replay com o
// seed armazenado) ate x_{k-1}, reconexao em x_k (area/solid angle), e a radiancia Lo que sai de
// x_k (sufixo completo, nunca replayado). kb=0 degenera na reconexao pura da F1/F2a (x_k = x2).
// O reservoir persistido esta SEMPRE re-ancorado no pixel que o escreveu (x1 e x_{k-1} do
// destino) — o Jacobiano do shift entra uma unica vez, no peso do merge.
//
// Requer: DDGICommon.hlsli (DDGI_OctEncode/Decode), Reflections/GGXSample.hlsli (GGX_PCG).

#include "../GI/DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"

struct PTReservoir {
    float3 x1;   // ponto primario (validacao temporal/espacial)
    float3 xkm1; // base da reconexao x_{k-1} (= x1 quando kb == 0)
    float3 xk;   // vertice de reconexao
    float3 nk;   // normal em xk (Jacobiano de reconexao)
    float3 Lo;   // radiancia que sai de xk rumo a x_{k-1} (sufixo completo)
    float  hitK; // |x2 - x1| do caminho (hitDist do GI p/ o NRD, F4)
    float  M;    // contagem de amostras
    float  W;    // peso de contribuicao nao-enviesado
    float  pHat; // F3: target (luminancia de C) da amostra selecionada NO dominio do proprio
                 // pixel — cache p/ o pairwise MIS espacial (evita re-shiftar o vizinho p/ ele mesmo)
    float  wSum; // soma dos pesos de resampling
    uint   seed; // seed do caminho de ORIGEM (replay do prefixo, dims globais por bounce)
    uint   kb;   // nº de bounces replayados antes da reconexao (0 = reconecta em x2)
};

// 64B: q0 = x1|(M,pHat f16x2), q1 = xk|W, q2 = Lo(3xf16)+hitK(f16) | nk oct(2xf16) | seed,
// q3 = xkm1|kb. M e inteiro pequeno (<= MCap+1) e pHat e clampado no finalize — f16 basta.
struct PTReservoirPacked {
    float4 q0; // x1.xyz (f32), w = asfloat(f16(M) | f16(pHat) << 16)
    float4 q1; // xk.xyz (f32), W
    uint4  q2; // x=f16(Lo.r,Lo.g), y=f16(Lo.b,hitK), z=f16(nkOct.x,nkOct.y), w=seed
    uint4  q3; // xyz = asuint(xkm1) (f32 — posicao, NUNCA half), w = kb
};

float PT_Lum(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// RNG com estado p/ a selecao estocastica do WRS (stream separado do RNG de caminho).
uint  PTResRngSeed(uint2 px, uint frame, uint salt) {
    return GGX_PCG(px.x + GGX_PCG(px.y + GGX_PCG(frame + salt * 0x1000193u)));
}
float PTResRngNext(inout uint s) { s = GGX_PCG(s); return (s & 0x00FFFFFFu) / 16777216.0f; }

void PTResInit(out PTReservoir r) {
    r.x1 = 0; r.xkm1 = 0; r.xk = 0; r.nk = 0; r.Lo = 0; r.hitK = 0;
    r.M = 0; r.W = 0; r.pHat = 0; r.wSum = 0; r.seed = 0; r.kb = 0;
}

PTReservoirPacked PTResPack(PTReservoir r) {
    PTReservoirPacked p;
    p.q0 = float4(r.x1, asfloat(f32tof16(r.M) | (f32tof16(r.pHat) << 16)));
    p.q1 = float4(r.xk, r.W);
    float2 nkOct = DDGI_OctEncode(r.nk);
    p.q2.x = f32tof16(r.Lo.r) | (f32tof16(r.Lo.g) << 16);
    p.q2.y = f32tof16(r.Lo.b) | (f32tof16(r.hitK) << 16);
    p.q2.z = f32tof16(nkOct.x) | (f32tof16(nkOct.y) << 16);
    p.q2.w = r.seed;
    p.q3 = uint4(asuint(r.xkm1.x), asuint(r.xkm1.y), asuint(r.xkm1.z), r.kb);
    return p;
}

PTReservoir PTResUnpack(PTReservoirPacked p) {
    PTReservoir r;
    r.x1   = p.q0.xyz;
    uint mp = asuint(p.q0.w);
    r.M    = f16tof32(mp & 0xFFFFu);
    r.pHat = f16tof32(mp >> 16);
    r.xk = p.q1.xyz; r.W = p.q1.w;
    r.Lo = float3(f16tof32(p.q2.x & 0xFFFFu), f16tof32(p.q2.x >> 16), f16tof32(p.q2.y & 0xFFFFu));
    r.hitK = f16tof32(p.q2.y >> 16);
    float2 nkOct = float2(f16tof32(p.q2.z & 0xFFFFu), f16tof32(p.q2.z >> 16));
    r.nk = DDGI_OctDecode(nkOct);
    r.seed = p.q2.w;
    r.xkm1 = float3(asfloat(p.q3.x), asfloat(p.q3.y), asfloat(p.q3.z));
    r.kb   = p.q3.w;
    r.wSum = 0.0f; // nao persistido (recomputado por frame)
    return r;
}

// Adiciona um candidato (M=1) com peso de resampling w; retorna true se ele foi SELECIONADO
// (o caller rastreia a contribuicao C do vencedor p/ o finalize/resolve). NOTA: o reservoir
// persistido e SEMPRE re-ancorado no pixel que o escreve (x1 = s1.Pos, xkm1 = base replayada no
// destino) — W ja embute o Jacobiano do shift aplicado no merge, entao o proximo frame calcula
// J contra a base de ONTEM, nunca contra a base ancestral (isso comporia o Jacobiano frame a
// frame = fireflies).
bool PTResUpdate(inout PTReservoir r, float3 x1C, float3 xkm1C, float3 xkC, float3 nkC,
                 float3 LoC, float hitC, uint kbC, uint seedC, float w, inout uint rng) {
    r.wSum += w;
    r.M    += 1.0f;
    if (w > 0.0f && PTResRngNext(rng) * r.wSum <= w) {
        r.x1 = x1C; r.xkm1 = xkm1C; r.xk = xkC; r.nk = nkC; r.Lo = LoC;
        r.hitK = hitC; r.kb = kbC; r.seed = seedC;
        return true;
    }
    return false;
}

// Funde um reservoir inteiro (other) no atual; retorna true se a amostra de other foi
// selecionada. pHatOther = pHat (no pixel atual, via shift) p/ a amostra de other; J = Jacobiano
// do shift other.xkm1 -> base atual (rejeitado pelo caller se fora da faixa).
bool PTResMerge(inout PTReservoir r, PTReservoir other, float pHatOther, float J, inout uint rng) {
    float w = pHatOther * other.W * other.M * J;
    r.wSum += w;
    r.M    += other.M;
    if (w > 0.0f && PTResRngNext(rng) * r.wSum <= w) {
        r.x1 = other.x1; r.xkm1 = other.xkm1; r.xk = other.xk; r.nk = other.nk;
        r.Lo = other.Lo; r.hitK = other.hitK; r.kb = other.kb; r.seed = other.seed;
        return true;
    }
    return false;
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
// Cacheia pHatSel no reservoir (clampado p/ caber em f16): e o p̂_j(y_j) que o pairwise MIS
// espacial (F3) precisa do vizinho sem re-shiftar a amostra dele p/ ele mesmo.
void PTResFinalizeW(inout PTReservoir r, float pHatSel) {
    r.W    = (pHatSel > 0.0f && r.M > 0.0f) ? (r.wSum / (r.M * pHatSel)) : 0.0f;
    r.pHat = min(pHatSel, 1.0e4f);
}

#endif
