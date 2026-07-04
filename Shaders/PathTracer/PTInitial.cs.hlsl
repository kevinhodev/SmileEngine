// ReSTIR PT — Pass A (F2: candidato com BSDF completo + reconnection shift com Jacobiano).
//
// Por pixel: (1) x1 do G-buffer; (2) 1 candidato indireto amostrando o BSDF COMPLETO de x1
// (difuso OU especular, one-sample MIS), com Lo = radiancia COMPLETA multi-bounce saindo de x2;
// (3) reuso temporal reprojetando o reservoir do frame anterior (WRS, reconnection shift com
// Jacobiano, MCap); (4) resolve BRDF-aware. O DIRETO do sol em x1 e o emissivo ficam FORA do
// reservoir (deterministicos) e sao somados no resolve. Saida full-radiance -> deferred
// (ReflectionParams.w==3). Reservoir {x1,xk,nk,Lo,M,W} empacotado em 64B ping-pong.
//
// INVARIANTE anti-firefly (a licao da 1a tentativa do F2): o reservoir persistido esta SEMPRE no
// dominio do pixel que o escreveu — o Jacobiano do shift entra UMA vez, no peso do merge, e o x1
// e re-ancorado antes do pack. Persistir o x1 de origem faria o proximo frame recalcular J contra
// um x1 cada vez mais defasado = Jacobiano composto/duplicado frame a frame = fireflies.
// Especular liso (roughness < ShiftParams.y) nao reconecta (quase-delta); fica p/ o replay (F2b).
// Debug (DebugParams.x==1): media progressiva do estimador de 1 amostra (ground truth).

#include "../GI/DDGICommon.hlsli"
#include "../GBuffer.hlsli"
#include "PTCommon.hlsli"
#include "PTReservoir.hlsli" // struct PTReservoirPacked (usado nos bindings abaixo)

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
Texture2D<float2>               Velocity   : register(t10);
StructuredBuffer<PTReservoirPacked> PrevReservoir : register(t11);

RWTexture2D<float4>                 PTOut         : register(u0); // rgb = radiancia full, a = |xk-x1|
RWTexture2D<float4>                 Accum         : register(u1); // debug: media progressiva
RWStructuredBuffer<PTReservoirPacked> CurrReservoir : register(u2);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli" // ShadeSky, AlphaTestPass, SMILE_RT_PROCEED, DDGI sampling
#include "PTRng.hlsli"
#include "PTRayUtil.hlsli"
#include "PTMaterial.hlsli"
#include "PTPathTrace.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    uint  W  = (uint)ScreenParams.x;
    if (px.x >= W || px.y >= (uint)ScreenParams.y)
        return;
    uint idx = px.y * W + px.x;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) {
        PTOut[px] = float4(0.0f, 0.0f, 0.0f, 0.0f); // ceu: o deferred descarta antes do passthrough
        PTReservoir empty; PTResInit(empty);
        CurrReservoir[idx] = PTResPack(empty);
        return;
    }

    // Superficie primaria x1 (mesmo decode do deferred).
    GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                  GBufferB.Load(int3(px, 0)),
                                  GBufferC.Load(int3(px, 0)));
    FPTSurface s1;
    s1.Pos       = PT_WorldPosFromDepth(px, deviceZ);
    s1.N         = g.WorldNormal;
    s1.Albedo    = g.BaseColor;
    s1.Emissive  = g.Emissive;
    s1.Roughness = max(g.Roughness, 0.04f);
    s1.Metallic  = g.Metallic;
    s1.Foliage   = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE);

    float3 V     = normalize(CameraPos.xyz - s1.Pos);
    uint   frame = (uint)TraceParams.x;
    uint   seed  = PTPathSeed(px, frame);
    uint   rng   = PTResRngSeed(px, frame, 1u); // stream do WRS (separado do RNG de caminho)
    float2 uv    = (px + 0.5f) * ScreenParams.zw;

    // --- (0) Direto do sol em x1 + emissivo (fora do reservoir, deterministico) --------------
    float3 direct = s1.Emissive + PT_SunNEE(s1, V, seed, 0);

    // --- (1) Candidato indireto: amostra o BSDF COMPLETO de x1 (difuso OU especular) ---------
    // F2: importance sampling do BSDF em x1 -> reconecta em x2 com BSDF completo => o indireto
    // glossy/especular de x1 volta (o F1 so reconectava o lobe difuso).
    FPTBsdfSample bs = PTSampleBsdf(s1, V, seed, 0);
    uint subSeed = GGX_PCG(seed ^ 0xA511E9B3u); // seed do sub-caminho (replay do F2b)

    // Criterios de connectability compartilhados pelo candidato e pelo reuso temporal.
    float camDist  = length(CameraPos.xyz - s1.Pos);
    float roughMin = ShiftParams.y;
    float minDist  = max(ShiftParams.x * camDist, 1e-3f); // footprint κ (Enhanced §4): reconexao
                                                          // mais curta que isso => J instavel

    PTReservoir r; PTResInit(r);
    r.x1 = s1.Pos;
    float3 giSingle = float3(0.0f, 0.0f, 0.0f); // estimador de 1 amostra do indireto (debug)

    if (bs.Valid) {
        RayDesc ray;
        ray.Origin    = s1.Pos + s1.N * max(TraceParams.w, 1e-3f);
        ray.Direction = bs.Dir;
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
        SMILE_RT_PROCEED(q)

        float3 xk, nk, Lo;
        float  hitK;
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            hitK = q.CommittedRayT();
            FPTSurface h = PTFetchHitSurface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                             q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                             ray.Origin, ray.Direction, hitK, PathParams.w);
            xk = h.Pos; nk = h.N;
            float subFirst;
            Lo = h.Emissive + PT_PathRadiance(h, -bs.Dir, subSeed,
                                              max((uint)PathParams.x, 1u) - 1u, (uint)PathParams.y,
                                              /*enableRR=*/true, subFirst);
        } else {
            hitK = TraceParams.y;
            xk = ray.Origin + bs.Dir * hitK; // ponto distante na direcao do ceu
            nk = -bs.Dir;
            Lo = ShadeSky(bs.Dir, normalize(SunDirIntensity.xyz), TraceParams.z);
        }

        // Firefly clamp no Lo por CANAL (clamp por luminancia deixa passar outlier saturado:
        // Lo azul puro com luminancia 25 tem canal B ~350 — as bolinhas coloridas do Bistro).
        { float mc = max(Lo.r, max(Lo.g, Lo.b)), mx = PathParams.z;
          if (mx > 0.0f && mc > mx) Lo *= mx / mc; }

        // Connectability (GRIS §7.5 + Enhanced §4): so reconecta o lobe ESPECULAR se x1 for
        // rugoso o bastante — reconexao em superficie lisa da pHat quase-delta e W estoura.
        // Reconexao mais curta que o footprint tambem e vetada (Jacobiano hipersensivel).
        bool connectable = (bs.Lobe == PT_LOBE_DIFFUSE || s1.Roughness >= roughMin)
                         && (hitK > minDist);

        if (connectable) {
            // Direcao de reconexao CONSISTENTE (mesma no peso e no pHat) — evita o descasamento
            // bs.Dir vs (xk-x1) que amplificava fireflies no especular.
            float3 wr  = xk - s1.Pos;
            float  lwr = length(wr);
            if (lwr > 1e-4f) {
                float3 omega = wr / lwr;
                float  pdf   = PT_BsdfPdf(s1, V, omega);
                if (pdf > 1e-5f) {
                    // Contribuicao de reconexao (roughness clampada >= roughMin, ver PTMaterial):
                    // o MESMO f~ usado no pHat do merge/finalize/resolve — target e contribuicao
                    // nunca divergem. pdf = pdf REAL do sampling (roughness verdadeira).
                    float3 Finit = PT_ReconnectContribution(s1, V, xk, Lo);
                    float  wInit = PT_Lum(Finit) / pdf;            // pHat/pdf
                    PTResUpdate(r, s1.Pos, xk, nk, Lo, hitK, subSeed, wInit, rng);
                    giSingle = Finit / pdf;                        // estimador de 1 amostra
                }
            }
        }
    }

    // Estimador de 1 amostra (ground truth p/ o debug — antes do reuso temporal).
    float3 Lsingle = direct + giSingle;

    // --- (2) Reuso temporal (reprojecao por motion vector + Jacobiano de reconexao) ----------
    // SEM gate em r.M: o historico sobrevive mesmo quando o candidato do frame e invalido/vetado
    // (com gate, todo frame que amostrava o lobe especular numa superficie lisa resetava M=0).
    if (ReuseParams.z > 0.5f) {
        float2 prevUv = uv - Velocity.Load(int3(px, 0)).rg;
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            int2 ppx = int2(prevUv * ScreenParams.xy);
            uint pidx = (uint)ppx.y * W + (uint)ppx.x;
            PTReservoir prev = PTResUnpack(PrevReservoir[pidx]);
            float posReject = ReuseParams.y * max(camDist, 1.0f);
            if (prev.M > 0.0f && length(prev.x1 - s1.Pos) < posReject
                && length(prev.xk - s1.Pos) > minDist) {
                prev.M = min(prev.M, ReuseParams.x); // MCap
                // pHat BRDF-aware do pixel ATUAL p/ a amostra de prev. prev.x1 = superficie do
                // pixel de ONTEM (re-ancorado la) => J ~ 1 em reproject de mesma superficie.
                // J fora da faixa = geometria incompativel: REJEITA o merge (clampar enviesa o
                // peso p/ cima e vira firefly).
                float pHatPrev = PT_PHatBrdf(s1, V, prev.xk, prev.Lo);
                float J = PTReconnectionJacobian(s1.Pos, prev.x1, prev.xk, prev.nk);
                if (J > 0.125f && J < 8.0f)
                    PTResMerge(r, prev, pHatPrev, J, rng);
            }
        }
    }

    // --- (3) Resolve final (vector weights: contribuicao RGB completa / pHat escalar) --------
    float  pHatSel  = PT_PHatBrdf(s1, V, r.xk, r.Lo);
    PTResFinalizeW(r, pHatSel);

    // RE-ANCORA o reservoir no dominio do pixel atual antes de persistir: o Jacobiano do shift ja
    // entrou no peso do merge (e portanto em W). Sem isso o proximo frame recalcula J contra um x1
    // ancestral = Jacobiano composto frame a frame = a fonte primaria dos fireflies do F2 v1.
    r.x1   = s1.Pos;
    r.hitK = (r.M > 0.0f) ? length(r.xk - s1.Pos) : 0.0f;

    float3 indirect = PT_ReconnectContribution(s1, V, r.xk, r.Lo) * r.W;
    { float mc = max(indirect.r, max(indirect.g, indirect.b)), mx = PathParams.z;
      if (mx > 0.0f && mc > mx) indirect *= mx / mc; } // clamp por CANAL (vector weights)
    float3 L = direct + indirect;

    // Debug: media progressiva do estimador de 1 amostra (ground truth).
    if ((uint)DebugParams.x == PT_DEBUG_ACCUM) {
        float  n    = DebugParams.y;
        float3 prev = (n > 0.5f) ? Accum[px].rgb : float3(0.0f, 0.0f, 0.0f);
        float3 avg  = prev + (Lsingle - prev) / (n + 1.0f);
        Accum[px] = float4(avg, 1.0f);
        L = avg;
    }

    PTOut[px] = float4(L, r.hitK); // r.hitK = |xk-x1| da amostra selecionada (hitDist do GI p/ F4)
    CurrReservoir[idx] = PTResPack(r);
}
