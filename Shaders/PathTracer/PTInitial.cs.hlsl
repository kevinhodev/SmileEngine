// ReSTIR PT — Pass A (F2b: random replay + hybrid shift).
//
// Por pixel: (1) x1 do G-buffer; (2) 1 candidato via walk hibrido (PTShift.hlsli): prefixo
// replayavel ate o primeiro vertice conectavel x_{k-1} -> reconexao em x_k -> sufixo Lo; a luz
// dos vertices do prefixo (e de caminhos sem reconexao) soma DIRETO no frame (Cemit, fora do
// reservoir); (3) reuso temporal: hybrid shift da amostra reprojetada (replay do prefixo com o
// seed FONTE + reconexao + Jacobiano); (4) resolve com o C rastreado do vencedor. O DIRETO do
// sol em x1 e o emissivo ficam FORA do reservoir (deterministicos). Saida full-radiance ->
// deferred (ReflectionParams.w==3). Reservoir 64B ping-pong (q3 = x_{k-1} + kb).
//
// INVARIANTE anti-firefly (licao da 1a tentativa do F2): o reservoir persistido esta SEMPRE no
// dominio do pixel que o escreveu — o Jacobiano do shift entra UMA vez, no peso do merge, e
// x1/x_{k-1} sao re-ancorados antes do pack. Persistir a base de origem faria o proximo frame
// recalcular J contra uma base cada vez mais defasada = Jacobiano composto = fireflies.
//
// Debug (DebugParams.x): 1 = media progressiva do estimador de 1 amostra (ground truth);
// 2 = heatmap de kb (verde=reconexao direta, amarelo/laranja=replay, vermelho=sem reconexao);
// 3 = canario de auto-replay (residuo |C_shift - C_cand| do proprio pixel; deve ser preto).

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
#include "PTShift.hlsli"

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

    // --- (1) Candidato: walk hibrido (prefixo replayavel -> reconexao em x_k -> sufixo) ------
    FPTCandidate cand = PT_TraceCandidate(s1, V, seed);

    PTReservoir r; PTResInit(r);
    r.x1 = s1.Pos;
    float3 Csel = float3(0.0f, 0.0f, 0.0f); // contribuicao (neste pixel) do sample vencedor

    if (cand.HasSample) {
        float wInit = PT_Lum(cand.Crecon) / cand.pdfRecon; // pHat/pdf
        if (PTResUpdate(r, s1.Pos, cand.xkm1, cand.xk, cand.nk, cand.Lo, cand.hit1, cand.kb,
                        seed, wInit, rng))
            Csel = cand.Crecon;
    }

    // Estimador de 1 amostra (ground truth p/ o debug — antes do reuso temporal). O prefixo
    // (Cemit) ja e pdf-dividido; so a reconexao divide pelo pdf da direcao.
    float3 Lsingle = direct + cand.Cemit
                   + (cand.HasSample ? cand.Crecon / cand.pdfRecon : float3(0.0f, 0.0f, 0.0f));

    // --- (2) Reuso temporal: hybrid shift da amostra reprojetada ------------------------------
    // Sem gate em r.M: o historico sobrevive a frames de candidato invalido/nao-conectavel.
    if (ReuseParams.z > 0.5f) {
        float2 prevUv = uv - Velocity.Load(int3(px, 0)).rg;
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            int2 ppx = int2(prevUv * ScreenParams.xy);
            uint pidx = (uint)ppx.y * W + (uint)ppx.x;
            PTReservoir prev = PTResUnpack(PrevReservoir[pidx]);
            float camDist   = length(CameraPos.xyz - s1.Pos);
            float posReject = ReuseParams.y * max(camDist, 1.0f);
            if (prev.M > 0.0f && length(prev.x1 - s1.Pos) < posReject) {
                prev.M = min(prev.M, ReuseParams.x); // MCap
                // Shift completo: replay do prefixo (kb bounces, seed FONTE) + reconexao.
                // prev.xkm1 = base re-ancorada ONTEM => J ~ 1 em reproject de mesma superficie.
                // J fora da faixa = geometria incompativel: REJEITA (clampar enviesa p/ cima).
                FPTShift sh = PT_HybridShift(s1, V, prev);
                if (sh.Ok && sh.J > 0.125f && sh.J < 8.0f) {
                    if (PTResMerge(r, prev, PT_Lum(sh.C), sh.J, rng)) {
                        Csel   = sh.C;
                        r.xkm1 = sh.xkm1; // re-ancora a base replayada no destino
                        r.hitK = sh.hit1;
                    }
                }
            }
        }
    }

    // --- (3) Resolve final (vector weights: C RGB do vencedor / pHat = luminancia dele) ------
    PTResFinalizeW(r, PT_Lum(Csel));

    // RE-ANCORA x1 no pixel atual antes de persistir (xkm1/hitK ja foram re-ancorados na
    // selecao): o Jacobiano do shift ja entrou no peso do merge (e portanto em W). Sem isso o
    // proximo frame recalcula J contra uma base ancestral = Jacobiano composto = fireflies.
    r.x1 = s1.Pos;

    float3 gi = Csel * r.W;
    { float mc = max(gi.r, max(gi.g, gi.b)), mx = PathParams.z;
      if (mx > 0.0f && mc > mx) gi *= mx / mc; } // clamp por CANAL
    float3 L = direct + gi + cand.Cemit;         // Cemit ja clampado no walk

    // --- Debug ---------------------------------------------------------------------------------
    uint dbg = (uint)DebugParams.x;
    if (dbg == PT_DEBUG_ACCUM) {
        // Media progressiva do estimador de 1 amostra (ground truth).
        float  n    = DebugParams.y;
        float3 prev = (n > 0.5f) ? Accum[px].rgb : float3(0.0f, 0.0f, 0.0f);
        float3 avg  = prev + (Lsingle - prev) / (n + 1.0f);
        Accum[px] = float4(avg, 1.0f);
        L = avg;
    } else if (dbg == PT_DEBUG_KMAP) {
        // Heatmap do vertice de reconexao do CANDIDATO deste frame.
        L = !cand.HasSample ? float3(1.0f, 0.0f, 0.0f)                  // sem reconexao (Cemit)
          : (cand.kb == 0   ? float3(0.0f, 1.0f, 0.0f)                  // reconecta em x2 (F2a)
          : (cand.kb == 1   ? float3(1.0f, 1.0f, 0.0f)                  // 1 bounce de replay
                            : float3(1.0f, 0.4f, 0.0f)));               // 2+ bounces de replay
    } else if (dbg == PT_DEBUG_CANARY) {
        // Auto-replay: o shift do candidato p/ o PROPRIO pixel deve reproduzir Crecon bit-exato
        // (mesmo seed, mesmas dims, mesmos raios). Residuo relativo em vermelho; falha do shift
        // em azul. Tela preta = replay integro.
        L = float3(0.0f, 0.0f, 0.0f);
        if (cand.HasSample) {
            PTReservoir self; PTResInit(self);
            self.x1 = s1.Pos; self.xkm1 = cand.xkm1; self.xk = cand.xk; self.nk = cand.nk;
            self.Lo = cand.Lo; self.hitK = cand.hit1; self.seed = seed; self.kb = cand.kb;
            self.M = 1.0f; self.W = 1.0f;
            FPTShift sh = PT_HybridShift(s1, V, self);
            if (!sh.Ok) {
                L = float3(0.0f, 0.0f, 1.0f);
            } else {
                float e = PT_Lum(abs(sh.C - cand.Crecon)) / max(PT_Lum(cand.Crecon), 1e-4f);
                L = float3(saturate(e * 50.0f), 0.0f, 0.0f);
            }
        }
    }

    PTOut[px] = float4(L, r.hitK); // r.hitK = |x2-x1| da amostra selecionada (hitDist p/ o F4)
    CurrReservoir[idx] = PTResPack(r);
}
