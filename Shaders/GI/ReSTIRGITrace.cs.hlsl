// ReSTIR GI — Pass A: trace + reservoir TEMPORAL (+ resolve quando o spatial esta OFF).
//
// Por pixel: (1) gera 1 sample inicial (raio cosseno-hemisferico -> x2/n2/Lo), (2) reusa o reservoir
// do frame anterior reprojetado por motion vector (WRS merge com Jacobiano de reconexao + rejeicao
// por posicao/plano/normal), com re-shade periodico da amostra (1/N px/frame) p/ iluminacao dinamica
// nao envelhecer no reservoir, (3) grava o reservoir {x1,x2,n2,Lo,M,W} + n1 oct em 4 tex ping-pong
// p/ o proximo frame E p/ o reuso espacial (Pass B).
// Resolve a irradiancia aqui tambem (gi = Lo*cosTheta1*W/pi); o Pass B sobrescreve quando ativo.
// Referencia: Ouyang et al. 2021 (ReSTIR GI). Convencao: gi = (1/pi)*E (com M=1 reduz a gi=L_i).

#include "DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"
#include "../RayOffset.hlsli"

cbuffer ReSTIRCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;            // x=W, y=H, z=1/W, w=1/H
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;             // x=frameIndex, y=maxRayDist, z=skyIntensity,
                                    // w=shadowRayBias (SO sombras no hit; a origem dos raios
                                    // que saem do G-buffer usa OffsetRayGBuffer)
    float4 ShadeParams;             // x=realHitShading(0/1), y=albedoLOD, z=fireflyMaxLuma, w=validateInterval
    float4 ReuseParams;             // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    float4 SpatialParams;           // x=spatialRadius, y=spatialCount, z=spatial(0/1), w=normalReject
    float4 JitterParams;            // xy = prevJitterUv - currJitterUv (reprojecao temporal),
                                    // z = nº de luzes (F5),
                                    // w = maxAge do reservoir (0 = sem expiracao)
};

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
// t4/t5 aposentados (VB/IB bindless via InstanceGeo); a tabela CPU mantem o layout com filler.
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7);
Texture2D<float2>               Velocity   : register(t8);
Texture2D<float4>               PrevResA   : register(t9);  // x1.xyz, M
Texture2D<float4>               PrevResB   : register(t10); // x2.xyz, W
Texture2D<float4>               PrevResC   : register(t11); // Lo.rgb, a = n1.oct.x
Texture2D<float4>               PrevResD   : register(t12); // n2.xyz, a = n1.oct.y

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
    float  camDist = length(CameraPos.xyz - x1);

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

    // Offset APENAS numerico (anti self-hit): o normal-bias de 0.2 aqui contaminava a medida —
    // x2 ficava deslocado do x1 usado por pHat/resolve (gi = Lo*cos'/cos, ~1.9x em contatos) e
    // o hitT do NRD nunca caia abaixo de ~0.2. Com offset ~mm, x2 e o hit real p/ todo efeito.
    RayDesc ray;
    ray.Origin    = OffsetRayGBuffer(x1, N, camDist);
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
    P.SunColor       = SunColor.rgb;        P.ShadowRayBias = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
    P.AlbedoLOD      = ShadeParams.y;
    P.RealHitShading = ShadeParams.x > 0.5f;
    P.NumLights      = (int)JitterParams.z; // F5
    P.ShadowRayMask  = (uint)SunColor.w;

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
    float age = 0.0f; // idade da amostra selecionada (frames sobrevividos no reservoir)
    {
        float pHat = TargetPHat(x1, N, x2, Lo);
        float pSrc = max(cosT, 1e-4f) / SMILE_PI;
        float wInit = (pSrc > 0.0f) ? (pHat / pSrc) : 0.0f;
        ResUpdate(r, x2, n2, Lo, wInit, rng); // adotada -> age 0 (ja e o default)
    }

    // --- (2) Reuso temporal ------------------------------------------------------------------
    if (ReuseParams.w > 0.5f) {
        float2 vel    = Velocity.Load(int3(px, 0)).rg;
        // SEM compensacao prevJitter-currJitter (A/B em bisect: com ela o fetch do reservoir
        // danca entre texels e o conteudo faz random walk -> manchas + rastejo). A busca 2x2
        // ancorada abaixo cobre o erro sub-texel restante.
        float2 prevUv = uv - vel;
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            // Fetch de UM texel por truncamento — config estavel do bisect 2026-07-12. A busca
            // 2x2 por melhor x1 (fix 6) foi removida: mesmo ancorada em prevPx ela mantinha as
            // manchas/rastejo; re-introduzir so com A/B dedicado. Unpack do M mantido (ResA.w
            // fica no formato M+idade empacotados; expiracao hoje desligada via MaxAge=0).
            int2 ppx = int2(prevUv * ScreenParams.xy);
            float4 pa = PrevResA.Load(int3(ppx, 0));
            float4 pc = PrevResC.Load(int3(ppx, 0));
            float4 pd = PrevResD.Load(int3(ppx, 0));
            float posReject = ReuseParams.y * max(camDist, 1.0f);
            float planeDist = abs(dot(N, pa.xyz - x1));
            float3 prevN1 = DDGI_OctDecode(float2(pc.a, pd.a));
            float prevM, prevAge;
            ResUnpackMAge(pa.w, prevM, prevAge);
            bool accept = prevM > 0.0f && dot(prevN1, N) >= SpatialParams.w &&
                          length(pa.xyz - x1) < posReject && planeDist < 0.2f * posReject;

            if (accept) {
                float4 pb = PrevResB.Load(int3(ppx, 0));
                Reservoir prev;
                prev.x1 = pa.xyz; prev.x2 = pb.xyz; prev.n2 = pd.xyz; prev.Lo = pc.rgb;
                prev.M  = min(prevM, ReuseParams.x); // MCap
                prev.W  = pb.w; prev.wSum = 0.0f;

                // Idade maxima da amostra (RTXDI maxReservoirAge): o MCap limita o PESO do
                // historico, nao a vida da amostra — uma amostra brilhante rara travada num
                // bolsao escuro re-valida a si mesma e vira mancha persistente (bisect nas
                // cortinas do Bistro: Temporal OFF = manchas somem). Expira com stagger por
                // hash do pixel (0.75x..1.25x) senao as manchas expirariam em onda sincrona.
                bool prevValid = true;
                float maxAge = JitterParams.w;
                if (maxAge > 0.0f) {
                    float stagger = 0.75f + 0.5f * float(GGX_PCG(px.x * 7919u + px.y) & 0xFFu) / 255.0f;
                    if (prevAge >= maxAge * stagger) prevValid = false;
                }

                // Validacao periodica da amostra temporal (estilo RTXDI): Lo foi shaded no frame
                // em que a amostra nasceu e a iluminacao muda continuamente (TimeOfDay anima o
                // sol) — sem re-shade, radiancia velha sobrevive indefinidamente no reservoir.
                // Valida 1/N dos pixels por frame, com fase por hash do pixel (senao a tela
                // validaria em onda): re-traca x1 -> x2; mesmo hit = Lo re-shaded; oclusor novo
                // ou geometria movida = descarta a amostra.
                uint validN = (uint)ShadeParams.w;
                if (prevValid && validN > 0u &&
                    ((uint)TraceParams.x + GGX_PCG(px.x + GGX_PCG(px.y))) % validN == 0u) {
                    float3 vorg = OffsetRayGBuffer(x1, N, camDist);
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
                            if (abs(t - len) <= max(kRevalidateRelTol * len, kRevalidateAbsTol)) {
                                float vsd;
                                prev.Lo = ShadeSurfaceHit(vq.CommittedInstanceID(),
                                                          vq.CommittedPrimitiveIndex(),
                                                          vq.CommittedTriangleBarycentrics(),
                                                          vq.CommittedWorldToObject3x4(),
                                                          vorg, vray.Direction, t, P, vsd);
                                // Geometria pode ter se movido DENTRO da tolerancia: refresca
                                // x2/n2 pro hit re-tracado, senao a radiancia nova fica
                                // associada a geometria velha (Jacobiano/reuso espacial
                                // operariam sobre um ponto que nao existe mais).
                                prev.x2 = vorg + vray.Direction * t;
                                prev.n2 = HitGeomNormal(vq.CommittedInstanceID(),
                                                        vq.CommittedPrimitiveIndex(),
                                                        vq.CommittedTriangleBarycentrics(),
                                                        vq.CommittedWorldToObject3x4(),
                                                        vray.Direction);
                            } else {
                                prevValid = false;
                            }
                        } else if (len >= kRevalidateSkyFrac * TraceParams.y) {
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
                    // Jacobiano TEMPORAL: a aceitacao tolera prev.x1 divergindo ate ~1% da
                    // camDist — com segmento indireto curto (contatos) isso muda a medida de
                    // verdade (J=1 era aproximacao enviesada). Mesma politica do espacial:
                    // J extremo REJEITA (clampar mantem o sample com peso errado).
                    float J = ReconnectionJacobian(x1, prev.x1, prev.x2, prev.n2);
                    if (J >= 0.1f && J <= 10.0f) {
                        float pHatPrev = TargetPHat(x1, N, prev.x2, prev.Lo);
                        // Amostra do historico sobreviveu a selecao -> envelhece 1 frame.
                        if (ResMerge(r, prev, pHatPrev, J, rng)) age = prevAge + 1.0f;
                    }
                }
            }
        }
    }

    // --- (3) Resolve (sobrescrito pelo Pass B quando o spatial esta ON) ----------------------
    ResFinalizeW(r, x1, N);
    float3 gi = ResResolve(r, x1, N, ShadeParams.z);

    // hitDist p/ o NRD = distancia da amostra VENCEDORA do WRS (o temporal pode trocar x2; o hitT
    // do raio inicial guiaria o denoiser com um caminho diferente do da radiancia resolvida).
    float selDist = (r.wSum > 0.0f) ? length(r.x2 - x1) : hitDist;
    float2 n1Oct = DDGI_OctEncode(N); // n1 no historico p/ a rejeicao por normal do temporal
    GIOut[px]    = float4(gi, selDist);
    CurrResA[px] = float4(r.x1, ResPackMAge(r.M, age));
    CurrResB[px] = float4(r.x2, r.W);
    CurrResC[px] = float4(r.Lo, n1Oct.x);
    CurrResD[px] = float4(r.n2, n1Oct.y);
}
