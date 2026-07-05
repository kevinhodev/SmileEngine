// ReSTIR PT — Pass B (F3): reuso ESPACIAL com pairwise MIS defensivo + vector weights.
//
// Por pixel: le o reservoir canonico (saida do Pass A, mesmo frame) + ate SpatialParams.y
// vizinhos gaussianos (sigma = SpatialParams.x). Cada vizinho aceito custa DOIS hybrid shifts:
//   direto  (amostra do vizinho -> pixel atual)  = contribuicao + p̂_c(T(y_j))
//   reverso (amostra canonica -> pixel do vizinho) = p̂_j(T'(y_c)) p/ o MIS do canonico
//
// Pairwise MIS defensivo (GRIS Eq. 38, generalizado c/ confidence M):
//   D_j(y) = M_c*p̂_c(y) + (M_S - M_c)*p̂_{<-j}(y),  M_S = M_c + soma dos M_j aceitos
//   vizinho:  m_j = (M_j/M_S)*(M_S - M_c)*p̂_j(y_j) / (M_c*p̂_c(T(y_j))*J_j + (M_S - M_c)*p̂_j(y_j))
//   canonico: m_c = M_c/M_S + soma_j (M_j/M_S)*M_c*p̂_c(y_c) / D_j(y_c)
//   (soma pontual das m = 1 em todo y; shift falho/J fora da faixa => p̂ = 0, o peso migra p/
//    os outros SEM darkening — a razao de usar pairwise e nao contagem 1/M como no temporal).
//   p̂_j(y_j) vem CACHEADO do reservoir (PTResFinalizeW); J_j entra multiplicado no termo do
//   canonico (p̂ transportado de dominio: p̂_{<-j}(y_j) = p̂_j(y_j)/J_j, tudo escalado por J_j).
//
// Vector weights (Enhanced §6.3): o shading NAO seleciona uma amostra — soma direto
//   L_gi = m_c*C_c*W_c + soma_j m_j*C_j->c*W_j*J_j   (C em RGB; o escalar/luminancia so
// existe dentro das m). Marginaliza a escolha do indice => mata o color noise sem custo extra.
// O spatial NAO realimenta o temporal (anti-bias), entao nada disso e persistido.
//
// Debug (DebugParams.x == 4): r = fracao de vizinhos rejeitados (geo/shift), g = m_c,
// b = fracao da energia vinda dos vizinhos. Demais modos de debug nem despacham este pass (C++).

#include "../GI/DDGICommon.hlsli"
#include "../GBuffer.hlsli"
#include "PTCommon.hlsli"
#include "PTReservoir.hlsli"

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
StructuredBuffer<DDGIVertex>    Vertices   : register(t4);
Buffer<uint>                    Indices    : register(t5);
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBufferA   : register(t7);
Texture2D<float4>               GBufferB   : register(t8);
Texture2D<float4>               GBufferC   : register(t9);
StructuredBuffer<PTReservoirPacked> Reservoirs : register(t10); // saida do Pass A (mesmo frame)
Texture2D<float4>               LitTex     : register(t11);     // direto(x1) + Cemit do Pass A

RWTexture2D<float4> PTOut    : register(u0); // in: C_c*W_c do Pass A; out: final (ou gi, se NRD)
RWTexture2D<float4> PTGiDiff : register(u1); // F4: in: parcela difusa do canonico; out: idem combinado

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli" // ShadeSky, AlphaTestPass, SMILE_RT_PROCEED, DDGI sampling
#include "PTRng.hlsli"
#include "PTRayUtil.hlsli"
#include "PTMaterial.hlsli"
#include "PTPathTrace.hlsli"
#include "PTShift.hlsli"

#define PT_SPATIAL_MAX 4

// Decodifica a superficie primaria de um pixel (mesmo decode do Pass A / deferred).
FPTSurface PT_SurfaceAt(uint2 px, float deviceZ) {
    GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                  GBufferB.Load(int3(px, 0)),
                                  GBufferC.Load(int3(px, 0)));
    FPTSurface s;
    s.Pos       = PT_WorldPosFromDepth(px, deviceZ);
    s.N         = g.WorldNormal;
    s.Albedo    = g.BaseColor;
    s.Emissive  = g.Emissive;
    s.Roughness = max(g.Roughness, 0.04f);
    s.Metallic  = g.Metallic;
    s.Foliage   = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE);
    return s;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    uint  W  = (uint)ScreenParams.x;
    uint  H  = (uint)ScreenParams.y;
    if (px.x >= W || px.y >= H)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f)
        return; // ceu: Pass A ja escreveu 0

    FPTSurface s1 = PT_SurfaceAt(px, deviceZ);
    float3 V      = normalize(CameraPos.xyz - s1.Pos);
    uint   frame  = (uint)TraceParams.x;

    PTReservoir rc  = PTResUnpack(Reservoirs[px.y * W + px.x]);
    float3 giC      = PTOut[px].rgb;    // C_c * W_c (Pass A, clampado)
    float3 giCDiff  = PTGiDiff[px].rgb; // parcela difusa (mesma escala)
    float  hitK     = PTOut[px].a;

    // --- Selecao de vizinhos (gaussiana, rejeicao geometrica ANTES dos shifts) ----------------
    uint  count  = min((uint)SpatialParams.y, PT_SPATIAL_MAX);
    float sigma  = SpatialParams.x;
    uint  rng    = PTResRngSeed(px, frame, 2u); // salt 2: stream proprio do spatial
    float camDist   = length(CameraPos.xyz - s1.Pos);
    float posReject = ReuseParams.y * max(camDist, 1.0f);

    uint2 nPx[PT_SPATIAL_MAX];
    float nM [PT_SPATIAL_MAX];
    uint  nAccept = 0;
    uint  nTried  = 0;

    [loop]
    for (uint c = 0; c < count; ++c) {
        ++nTried;
        // Offset gaussiano via Box-Muller (raio minimo 1px p/ nao cair no proprio pixel).
        float u1 = max(PTResRngNext(rng), 1e-6f);
        float u2 = PTResRngNext(rng);
        float rad = max(sigma * sqrt(-2.0f * log(u1)), 1.5f);
        float phi = 2.0f * SMILE_PI * u2;
        int2 q = int2(px) + int2(round(float2(rad * cos(phi), rad * sin(phi))));
        if (q.x < 0 || q.y < 0 || q.x >= (int)W || q.y >= (int)H || all(q == int2(px)))
            continue;

        float qZ = Depth.Load(int3(q, 0)).r;
        if (qZ <= 0.0f)
            continue;

        // Rejeicao por posicao (mesma escala do temporal) + normal (superficie compativel).
        float3 qPos = PT_WorldPosFromDepth((uint2)q, qZ);
        if (length(qPos - s1.Pos) >= posReject)
            continue;
        GBufferData qg = DecodeGBuffer(GBufferA.Load(int3(q, 0)),
                                       GBufferB.Load(int3(q, 0)),
                                       GBufferC.Load(int3(q, 0)));
        if (dot(qg.WorldNormal, s1.N) < SpatialParams.w)
            continue;

        nPx[nAccept] = (uint2)q;
        // So o M (f16 baixo de q0.w) — o unpack completo fica p/ o loop de shifts.
        nM [nAccept] = f16tof32(asuint(Reservoirs[(uint)q.y * W + (uint)q.x].q0.w) & 0xFFFFu);
        ++nAccept;
    }

    // Confidence total M_S (canonico + vizinhos aceitos). Vizinho com M=0 nao contribui nem
    // pesa; some-o de M_S removendo-o do conjunto e tudo continua consistente.
    float Mc = rc.M;
    float MS = Mc;
    for (uint i = 0; i < nAccept; ++i)
        MS += nM[i];

    float pHatC  = rc.pHat;           // p̂_c(y_c) cacheado (0 = canonico sem amostra)
    bool  hasC   = (Mc > 0.0f) && (pHatC > 0.0f);
    float mcSum  = 0.0f;              // termos do MIS do canonico (um por vizinho)
    float3 LgiN     = float3(0.0f, 0.0f, 0.0f); // soma vetorial dos vizinhos
    float3 LgiNDiff = float3(0.0f, 0.0f, 0.0f); // parcela difusa dela (split NRD, F4)
    uint  nShiftOk = 0;

    [loop]
    for (uint n = 0; n < nAccept; ++n) {
        float Mi = nM[n];
        if (Mi <= 0.0f || MS <= 0.0f)
            continue;
        PTReservoir rn = PTResUnpack(Reservoirs[nPx[n].y * W + nPx[n].x]);

        // --- shift DIRETO: amostra do vizinho avaliada neste pixel -----------------------------
        if (rn.pHat > 0.0f && rn.W > 0.0f) {
            FPTShift fw = PT_HybridShift(s1, V, rn, true);
            if (fw.Ok && fw.J > 0.125f && fw.J < 8.0f) {
                float pFwd  = PT_Lum(fw.C);                 // p̂_c(T(y_j))
                float denom = Mc * pFwd * fw.J + (MS - Mc) * rn.pHat;
                if (denom > 0.0f && pFwd > 0.0f) {
                    float mj = (Mi / MS) * (MS - Mc) * rn.pHat / denom;
                    LgiN     += mj * fw.C     * (rn.W * fw.J); // vector weight: C em RGB
                    LgiNDiff += mj * fw.Cdiff * (rn.W * fw.J);
                    ++nShiftOk;
                }
            }
        }

        // --- shift REVERSO: amostra canonica avaliada no pixel do vizinho ----------------------
        // Compoe o m_c (defensivo): shift reverso falho => p̂_{<-j} = 0 => o termo vira
        // (Mj/MS)*Mc*p̂_c/D = Mj/MS, devolvendo o peso ao canonico (sem darkening).
        if (hasC) {
            float pRev = 0.0f;
            float qZ = Depth.Load(int3(nPx[n], 0)).r;
            FPTSurface sj = PT_SurfaceAt(nPx[n], qZ);
            float3 Vj = normalize(CameraPos.xyz - sj.Pos);
            FPTShift rv = PT_HybridShift(sj, Vj, rc, true);
            if (rv.Ok && rv.J > 0.125f && rv.J < 8.0f)
                pRev = PT_Lum(rv.C) * rv.J;                 // p̂_{<-j}(y_c) ja transportado
            float denom = Mc * pHatC + (MS - Mc) * pRev;
            if (denom > 0.0f)
                mcSum += (Mi / MS) * Mc * pHatC / denom;
        }
    }

    float mc = (MS > 0.0f) ? (Mc / MS + mcSum) : 0.0f; // sem vizinhos: mc = 1 => passthrough
    float3 Lgi     = mc * giC     + LgiN;
    float3 LgiDiff = mc * giCDiff + LgiNDiff;

    // Firefly clamp por CANAL (a mesma escala nos dois p/ manter diff <= combinado).
    { float mcc = max(Lgi.r, max(Lgi.g, Lgi.b)), mx = PathParams.z;
      if (mx > 0.0f && mcc > mx) { Lgi *= mx / mcc; LgiDiff *= mx / mcc; } }

    // F4: com NRD ligado, PTOut/PTGiDiff seguem carregando SO o GI (o pack separa diff/spec e o
    // composite soma o PTLit depois do denoise); sem NRD, PTOut ja e a imagem final.
    bool nrdOn = (ReuseParams.w > 0.5f);
    float3 L = nrdOn ? Lgi : (LitTex.Load(int3(px, 0)).rgb + Lgi);

    if ((uint)DebugParams.x == PT_DEBUG_SPATIAL) {
        float rejected = (nTried > 0) ? 1.0f - (float)nAccept / (float)nTried : 0.0f;
        float fracN    = PT_Lum(LgiN) / max(PT_Lum(Lgi), 1e-4f);
        L = float3(rejected, saturate(mc), saturate(fracN));
    }

    PTGiDiff[px] = float4(LgiDiff, 0.0f);
    PTOut[px]    = float4(L, hitK);
}
