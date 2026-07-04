// ReSTIR PT — Pass A (F1: reservoir TEMPORAL + reconnection shift puro).
//
// Por pixel: (1) x1 do G-buffer; (2) 1 candidato indireto DIFUSO (raio cosseno de x1 -> x2), com
// Lo = radiancia COMPLETA multi-bounce saindo de x2 (NEE do sol + bounces + cauda DDGI); (3) reuso
// temporal reprojetando o reservoir do frame anterior (WRS, J=1 = reconnection shift/mesma
// superficie, MCap); (4) resolve. O DIRETO do sol em x1 e o emissivo ficam FORA do reservoir
// (deterministicos, baixa variancia) e sao somados no resolve. Saida full-radiance -> deferred
// (ReflectionParams.w==3). Reservoir {x1,xk,nk,Lo,M,W} empacotado em 64B ping-pong.
//
// LIMITE do F1 (intencional): so o lobe DIFUSO de x1 e reconectado — o indireto glossy/especular
// de x1 volta no F2 (random replay + hybrid shift). Debug (DebugParams.x==1): media progressiva
// do estimador de 1 amostra (ground truth p/ comparar com o reuso).

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

    // --- (1) Candidato indireto DIFUSO: raio cosseno de x1 -> x2 -----------------------------
    // Consome as dims BSDF em bounce 0 (contrato do replay do F2, mesmo que aqui seja so difuso).
    float2 u    = PTRand2(seed, 0, PT_DIM_BSDF_U);
    float  rr   = sqrt(u.x);
    float  phi  = 2.0f * SMILE_PI * u.y;
    float  cosT = sqrt(saturate(1.0f - u.x));
    float3 dir  = normalize(mul(float3(rr * cos(phi), rr * sin(phi), cosT), GGX_TangentBasis(s1.N)));

    RayDesc ray;
    ray.Origin    = s1.Pos + s1.N * max(TraceParams.w, 1e-3f);
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    SMILE_RT_PROCEED(q)

    float3 xk, nk, Lo;
    float  hitK;
    // Seed do sub-caminho a partir de x2 (armazenado p/ o random replay do F2).
    uint   subSeed = GGX_PCG(seed ^ 0xA511E9B3u);
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hitK = q.CommittedRayT();
        FPTSurface h = PTFetchHitSurface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                         q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                         ray.Origin, ray.Direction, hitK, PathParams.w);
        xk = h.Pos; nk = h.N;
        // Lo = radiancia saindo de x2 rumo a x1 = emissivo + sufixo multi-bounce (NEE + DDGI).
        float subFirst;
        Lo = h.Emissive + PT_PathRadiance(h, -dir, subSeed,
                                          max((uint)PathParams.x, 1u) - 1u, (uint)PathParams.y,
                                          /*enableRR=*/true, subFirst);
    } else {
        hitK = TraceParams.y;
        xk = ray.Origin + dir * hitK; // ponto distante na direcao do ceu
        nk = -dir;
        Lo = ShadeSky(dir, normalize(SunDirIntensity.xyz), TraceParams.z);
    }

    // Firefly clamp no Lo (mata outliers antes do reservoir; o brilho vem de W, nao de Lo).
    {
        float lum = PT_Lum(Lo), mx = PathParams.z;
        if (mx > 0.0f && lum > mx) Lo *= mx / lum;
    }

    // Reservoir inicial (M=1). pSrc = cosTheta1/pi (hemisferio cosseno).
    PTReservoir r; PTResInit(r);
    r.x1 = s1.Pos;
    {
        float pHat  = PTTargetPHat(s1.Pos, s1.N, xk, Lo);
        float pSrc  = max(cosT, 1e-4f) / SMILE_PI;
        float wInit = (pSrc > 0.0f) ? (pHat / pSrc) : 0.0f;
        PTResUpdate(r, xk, nk, Lo, hitK, subSeed, wInit, rng);
    }

    // Estimador de 1 amostra (ground truth p/ o debug — antes do reuso temporal).
    float3 diffAlbedo = PT_DiffuseAlbedo(s1);
    float3 Lsingle;
    {
        PTReservoir r0 = r;
        PTResFinalizeW(r0, s1.Pos, s1.N);
        Lsingle = direct + diffAlbedo * PTResResolve(r0, s1.Pos, s1.N, PathParams.z);
    }

    // --- (2) Reuso temporal (reprojecao por motion vector, J=1 = reconnection shift) ---------
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
                float pHatPrev = PTTargetPHat(s1.Pos, s1.N, prev.xk, prev.Lo);
                PTResMerge(r, prev, pHatPrev, 1.0f, rng);
            }
        }
    }

    // --- (3) Resolve final -------------------------------------------------------------------
    PTResFinalizeW(r, s1.Pos, s1.N);
    r.x1 = s1.Pos;
    float3 indirect = diffAlbedo * PTResResolve(r, s1.Pos, s1.N, PathParams.z);
    float3 L = direct + indirect;

    // Debug: media progressiva do estimador de 1 amostra (ground truth).
    if ((uint)DebugParams.x == PT_DEBUG_ACCUM) {
        float  n    = DebugParams.y;
        float3 prev = (n > 0.5f) ? Accum[px].rgb : float3(0.0f, 0.0f, 0.0f);
        float3 avg  = prev + (Lsingle - prev) / (n + 1.0f);
        Accum[px] = float4(avg, 1.0f);
        L = avg;
    }

    PTOut[px] = float4(L, hitK);
    CurrReservoir[idx] = PTResPack(r);
}
