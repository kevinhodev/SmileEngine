// ReSTIR GI — Pass A: trace + reservoir TEMPORAL (+ resolve quando o spatial esta OFF).
//
// Por pixel: (1) gera 1 sample inicial (raio cosseno-hemisferico -> x2/n2/Lo), (2) reusa o reservoir
// do frame anterior reprojetado por motion vector (WRS merge, J=1 = mesma superficie), com re-shade
// periodico da amostra (1/N px/frame) p/ iluminacao dinamica nao envelhecer no reservoir, (3) grava
// o reservoir {x1,x2,n2,Lo,M,W} em 4 tex ping-pong p/ o proximo frame E p/ o reuso espacial (Pass B).
// Resolve a irradiancia aqui tambem (gi = Lo*cosTheta1*W/pi); o Pass B sobrescreve quando ativo.
// Referencia: Ouyang et al. 2021 (ReSTIR GI). Convencao: gi = (1/pi)*E (com M=1 reduz a gi=L_i).

#include "DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"

cbuffer ReSTIRCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;            // x=W, y=H, z=1/W, w=1/H
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;             // x=frameIndex, y=maxRayDist, z=skyIntensity, w=normalBias
    float4 ShadeParams;             // x=realHitShading(0/1), y=albedoLOD, z=fireflyMaxLuma, w=validateInterval
    float4 ReuseParams;             // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    float4 SpatialParams;           // x=spatialRadius, y=spatialCount, z=spatial(0/1), w=normalReject
    float4 JitterParams;            // xy = prevJitterUv - currJitterUv (reprojecao no espaco
                                    // jittered), z = nº de luzes puntuais no SceneLights (F5)
};

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
StructuredBuffer<DDGIVertex>    Vertices   : register(t4);
Buffer<uint>                    Indices    : register(t5);
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7);
Texture2D<float2>               Velocity   : register(t8);
Texture2D<float4>               PrevResA   : register(t9);  // x1.xyz, M
Texture2D<float4>               PrevResB   : register(t10); // x2.xyz, W
Texture2D<float4>               PrevResC   : register(t11); // Lo.rgb
Texture2D<float4>               PrevResD   : register(t12); // n2.xyz

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t13); // F5: luzes puntuais nos hits

RWTexture2D<float4>             GIOut    : register(u0); // rgb=gi, a=hitDist (NRD)
RWTexture2D<float4>            CurrResA : register(u1);
RWTexture2D<float4>            CurrResB : register(u2);
RWTexture2D<float4>            CurrResC : register(u3);
RWTexture2D<float4>            CurrResD : register(u4);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "HitShading.hlsli"
#include "ReSTIRReservoir.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) {
        GIOut[px]    = float4(0.0f, 0.0f, 0.0f, 0.0f);
        CurrResA[px] = 0.0f; CurrResB[px] = 0.0f; CurrResC[px] = 0.0f; CurrResD[px] = 0.0f;
        return;
    }

    float4 gb = GBuffer.Load(int3(px, 0));
    float3 N  = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 x1  = wH.xyz / wH.w;

    // Salt no seed: sem ele, RngSeed(px, frame) == s0 do GGX_Rand2(px, frame) usado na direcao do
    // raio (mesma cadeia PCG) — a selecao do WRS ficaria correlacionada com o sample inicial.
    uint rng = RngSeed(px, (uint)TraceParams.x ^ 0xA511E9B3u);

    // --- (1) Sample inicial: raio cosseno-hemisferico (Malley) -------------------------------
    float2 E    = GGX_Rand2(px, (uint)TraceParams.x);
    float  rr   = sqrt(E.x);
    float  phi  = 2.0f * SMILE_PI * E.y;
    float  cosT = sqrt(saturate(1.0f - E.x));
    float3 d    = float3(rr * cos(phi), rr * sin(phi), cosT);
    float3x3 basis = GGX_TangentBasis(N);
    float3 dir  = normalize(mul(d, basis));

    float3 sunDir = normalize(SunDirIntensity.xyz);

    RayDesc ray;
    ray.Origin    = x1 + N * max(TraceParams.w, 1e-3f);
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
    SMILE_RT_PROCEED(q)

    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
    P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColor.rgb;        P.NormalBias   = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
    P.AlbedoLOD      = ShadeParams.y;
    P.RealHitShading = ShadeParams.x > 0.5f;
    P.NumLights      = (int)JitterParams.z; // F5

    float3 Lo, x2, n2;
    float  hitDist;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float sd;
        hitDist = q.CommittedRayT();
        Lo = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                             q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                             ray.Origin, ray.Direction, hitDist, P, sd);
        n2 = HitGeomNormal(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                           q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(), ray.Direction);
        x2 = ray.Origin + ray.Direction * hitDist;
    } else {
        hitDist = TraceParams.y;
        Lo = ShadeSky(dir, sunDir, P.SkyIntensity);
        x2 = ray.Origin + ray.Direction * hitDist; // ponto distante na direcao do ceu
        n2 = -dir;                                  // normal "virada" p/ o ponto visivel
    }

    // Firefly clamp: limita outliers de luminancia (mata os pontinhos brilhantes que o WRS
    // travaria e que o denoiser nao remove bem). Nao afeta a faixa normal de radiancia.
    {
        float lum = ReSTIR_Luminance(Lo);
        float maxLum = ShadeParams.z;
        if (maxLum > 0.0f && lum > maxLum) Lo *= maxLum / lum;
    }

    Reservoir r; ResInit(r);
    r.x1 = x1;
    {
        float pHat = TargetPHat(x1, N, x2, Lo);
        float pSrc = max(cosT, 1e-4f) / SMILE_PI;
        float wInit = (pSrc > 0.0f) ? (pHat / pSrc) : 0.0f;
        ResUpdate(r, x2, n2, Lo, wInit, rng);
    }

    // --- (2) Reuso temporal ------------------------------------------------------------------
    if (ReuseParams.w > 0.5f) {
        float2 vel    = Velocity.Load(int3(px, 0)).rg;
        // Velocity e unjittered (contrato do FSR2), mas o raster e jittered: sem compensar
        // prevJitter - currJitter, a busca no historico desloca ate 1 texel por frame.
        float2 prevUv = uv - vel + JitterParams.xy;
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            int2 ppx = int2(prevUv * ScreenParams.xy);
            float4 pa = PrevResA.Load(int3(ppx, 0));
            float4 pb = PrevResB.Load(int3(ppx, 0));
            float4 pc = PrevResC.Load(int3(ppx, 0));
            float4 pd = PrevResD.Load(int3(ppx, 0));
            float camDist   = length(CameraPos.xyz - x1);
            float posReject = ReuseParams.y * max(camDist, 1.0f);
            // Rejeicao por plano alem da radial: a reprojecao pode cair numa superficie quase
            // paralela (quina/glancing) dentro do raio radial mas fora do plano da atual.
            float planeDist = abs(dot(N, pa.xyz - x1));
            if (pa.w > 0.0f && length(pa.xyz - x1) < posReject && planeDist < 0.2f * posReject) {
                Reservoir prev;
                prev.x1 = pa.xyz; prev.x2 = pb.xyz; prev.n2 = pd.xyz; prev.Lo = pc.rgb;
                prev.M  = min(pa.w, ReuseParams.x); // MCap
                prev.W  = pb.w; prev.wSum = 0.0f;

                // Validacao periodica da amostra temporal (estilo RTXDI): Lo foi shaded no frame
                // em que a amostra nasceu e a iluminacao muda continuamente (TimeOfDay anima o
                // sol) — sem re-shade, radiancia velha sobrevive indefinidamente no reservoir
                // (MCap limita contagem, nao idade). Valida 1/N dos pixels por frame, com fase
                // por hash do pixel (senao a tela validaria em onda): re-traca x1 -> x2; mesmo
                // hit = Lo re-shaded; oclusor novo ou geometria movida = descarta a amostra.
                bool prevValid = true;
                uint validN = (uint)ShadeParams.w;
                if (validN > 0u &&
                    ((uint)TraceParams.x + GGX_PCG(px.x + GGX_PCG(px.y))) % validN == 0u) {
                    float3 vorg = x1 + N * max(TraceParams.w, 1e-3f);
                    float3 toS  = prev.x2 - vorg;
                    float  len  = length(toS);
                    if (len > 1e-3f) {
                        RayDesc vray;
                        vray.Origin    = vorg;
                        vray.Direction = toS / len;
                        vray.TMin      = 0.0f;
                        vray.TMax      = TraceParams.y;
                        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> vq;
                        vq.TraceRayInline(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, vray);
                        SMILE_RT_PROCEED(vq)
                        if (vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                            float t = vq.CommittedRayT();
                            if (abs(t - len) <= max(0.02f * len, 0.05f)) {
                                float vsd;
                                prev.Lo = ShadeSurfaceHit(vq.CommittedInstanceID(),
                                                          vq.CommittedPrimitiveIndex(),
                                                          vq.CommittedTriangleBarycentrics(),
                                                          vq.CommittedWorldToObject3x4(),
                                                          vorg, vray.Direction, t, P, vsd);
                            } else {
                                prevValid = false;
                            }
                        } else if (len >= 0.98f * TraceParams.y) {
                            prev.Lo = ShadeSky(vray.Direction, sunDir, P.SkyIntensity); // era ceu
                        } else {
                            prevValid = false; // superficie sumiu (geometria movida)
                        }
                        if (prevValid) { // mesmo firefly clamp do sample inicial
                            float lum = ReSTIR_Luminance(prev.Lo);
                            if (ShadeParams.z > 0.0f && lum > ShadeParams.z)
                                prev.Lo *= ShadeParams.z / lum;
                        }
                    }
                }

                if (prevValid) {
                    float pHatPrev = TargetPHat(x1, N, prev.x2, prev.Lo); // J=1 (mesma superficie)
                    ResMerge(r, prev, pHatPrev, 1.0f, rng);
                }
            }
        }
    }

    // --- (3) Resolve (sobrescrito pelo Pass B quando o spatial esta ON) ----------------------
    ResFinalizeW(r, x1, N);
    float3 gi = ResResolve(r, x1, N, ShadeParams.z);

    // hitDist p/ o NRD = distancia da amostra VENCEDORA do WRS (o temporal pode trocar x2; o hitT
    // do raio inicial guiaria o denoiser com um caminho diferente do da radiancia resolvida).
    // Fallback pro raio inicial quando nada foi selecionado (wSum=0 deixa x2 zerado).
    float selDist = (r.wSum > 0.0f) ? length(r.x2 - x1) : hitDist;
    GIOut[px]    = float4(gi, selDist);
    CurrResA[px] = float4(r.x1, r.M);
    CurrResB[px] = float4(r.x2, r.W);
    CurrResC[px] = float4(r.Lo, 0.0f);
    CurrResD[px] = float4(r.n2, 0.0f);
}
