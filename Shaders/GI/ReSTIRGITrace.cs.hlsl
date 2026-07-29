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
    row_major float4x4 View;        // nao usado aqui; declarado p/ os offsets do CB baterem
    float4 RayEpsA;                 // x=originFloorMin, y=originFloorPerMeter, z=angularMax,
                                    // w=shadowRayBiasMin  (perfil de epsilons — knobs do editor)
    float4 RayEpsB;                 // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin,
                                    // w=angularMinRatio
    float4 PolicyParams;            // x = politica deste passe (backface/culling)
    // Gather do 2o bounce (contrato do HitShading.hlsli).
    float4 GIDistParams;            // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;            // x=escala do bias, y=teto em metros, zw=-
};

// Depois do cbuffer: os dois headers leem RayEpsA/RayEpsB (ver o contrato no RayOffset.hlsli).
#include "../RayOffset.hlsli"

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
// t4/t5: atlas de distancia e ProbeData do DDGI — o 2o bounce usa o gather COMPLETO
// (Chebyshev + bias + skip), igual ao deferred. Antes eram filler do VB/IB bindless.
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t5);
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
    ray.Origin    = OffsetRayGBuffer(x1, N, dir, camDist);
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;
    // SEM culling, igual ao `CullingMode = 0` do Lumen no passe equivalente
    // (LumenReSTIRGather.usf:315 e :422). O gather PRECISA enxergar o verso: e a politica de
    // backface abaixo que decide entre re-tracar (auto-interseccao) e matar o caminho (geometria
    // real vista por dentro). Deixar o DXR cullar descartaria o hit antes da classificacao e a
    // politica viraria codigo morto justamente com o culling seletivo ligado.
    // As flags do template RayQuery<> e as do TraceRayInline sao COMBINADAS (spec do DXR), entao
    // as duas tem que ficar em NONE.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, ray);
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

    // POLITICA DE BACKFACE (Lumen AvoidSelfIntersections modo Retrace + terminacao preta).
    //
    // Os dois passos sao UMA politica so e nao podem ser separados: sozinha, a terminacao preta
    // transformaria toda auto-interseccao proxima — a mesma familia de erro que o sweep do ray
    // offset atacou — em MANCHA PRETA, trocando um artefato por outro. Primeiro se descarta o hit
    // proximo suspeito, e SO o que sobrar vira oclusao.
    //
    // Terminacao preta em vez de deixar o raio seguir: bater no verso de uma superficie one-sided
    // significa que o raio esta DENTRO de geometria. Deixa-lo continuar ate o ceu e exatamente o
    // vazamento medido no A/B do culling. A UE comenta a mesma linha com "to minimize leaking" e
    // aceita a sobre-oclusao que isso causa (LumenHardwareRayTracingCommon.ush:1018).
    bool killPath = false;
    if (PolicyParams.x > 0.5f && q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        bool  twoSided;
        bool  backface = HitIsBackface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                       q.CommittedWorldToObject3x4(), ray.Direction, twoSided);
        float t        = q.CommittedRayT();

        float skipDist = -1.0f;
        if (twoSided && t < kSelfIsectTwoSidedDist)            skipDist = t + kSelfIsectRetraceBias;
        else if (!twoSided && backface && t < kSelfIsectBackfaceDist)
                                                               skipDist = kSelfIsectBackfaceDist;
        if (skipDist > 0.0f) {
            ray.TMin = skipDist;
            q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, ray);
            SMILE_RT_PROCEED(q)
            ray.TMin = 0.0f; // a origem nao mudou; so o intervalo daquela consulta
            // Re-classifica SO aqui: no caminho comum (sem retrace) a classificacao de cima ja
            // vale, e HitIsBackface custa 3 indices + 3 posicoes.
            backface = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) &&
                       HitIsBackface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                     q.CommittedWorldToObject3x4(), ray.Direction, twoSided);
        }

        // O que sobrou de verso one-sided ja passou do teste de proximidade: e geometria real
        // vista por dentro, nao BLAS desalinhada.
        killPath = backface && !twoSided &&
                   q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    }

    float3 Lo, x2, n2;
    float  hitDist;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float sd;
        hitDist = q.CommittedRayT();
        n2 = HitGeomNormal(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                           q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(), ray.Direction);
        x2 = ray.Origin + ray.Direction * hitDist;
        // Hit preto = amostra de contribuicao ZERO, nao um "sem hit": tratar como miss mandaria o
        // ceu p/ dentro da parede, que e o bug que estamos fechando. Ela conta no M (a media do
        // reservoir sabe que aquela direcao nao rende nada) mas, com wInit = 0, o WRS nao a
        // seleciona — x2/n2 sao calculados p/ o hitDist do NRD e NAO chegam a entrar no
        // reservoir nem no Jacobiano.
        Lo = killPath ? float3(0.0f, 0.0f, 0.0f)
                      : ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                        q.CommittedTriangleBarycentrics(),
                                        q.CommittedWorldToObject3x4(),
                                        ray.Origin, ray.Direction, hitDist, P, sd);
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
                // historico, nao a vida da amostra. Expira com stagger por hash do pixel
                // (0.75x..1.25x) senao a expiracao viria em onda sincrona.
                // ATENCAO ao motivo: isto foi escrito supondo que a expiracao matava a mancha
                // persistente (amostra brilhante travada em bolsao escuro re-validando a si
                // mesma, bisect nas cortinas do Bistro). A medicao de 2026-07-28 desmentiu:
                // padrao fixo na media 4,00% com MaxAge=8 vs 4,03% com 0 — empate. O que a
                // expiracao entrega de verdade e LATENCIA sob luz mudando (~10 frames na saida
                // do NRD). Ver o bloco de medicao no RayEpsilons.h antes de mexer no default.
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
                    // Direcao aproximada (de x1, nao da origem deslocada) so p/ o termo angular
                    // do offset: a direcao exata depende da origem, que depende do offset.
                    float3 vdir = SafeRayDir(prev.x2 - x1, N);
                    float3 vorg = OffsetRayGBuffer(x1, N, vdir, camDist);
                    float3 toS  = prev.x2 - vorg;
                    float  len  = length(toS);
                    if (len > 1e-3f) {
                        RayDesc vray;
                        vray.Origin    = vorg;
                        vray.Direction = toS / len;
                        vray.TMin      = 0.0f;
                        vray.TMax      = TraceParams.y;
                        // NONE pelo mesmo motivo do gather: este re-trace tem que reencontrar a
                        // MESMA superficie da amostra guardada, e cullar mudaria o que ele acha.
                        RayQuery<RAY_FLAG_NONE> vq;
                        vq.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, vray);
                        SMILE_RT_PROCEED(vq)

                        // MESMA politica de backface do gather, sob o mesmo toggle. Aqui o
                        // RETRACE importa mais que no gather: sem ele, uma auto-interseccao
                        // proxima daria t << len, cairia fora da tolerancia de re-hit e a
                        // revalidacao DESCARTARIA uma amostra boa como se um oclusor tivesse
                        // aparecido. O kill fecha o outro lado: se o x2 guardado passou a ficar
                        // atras de geometria one-sided, a amostra vale zero, nao a radiancia
                        // velha.
                        bool vKill = false;
                        if (PolicyParams.x > 0.5f &&
                            vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                            bool  vTwoSided;
                            bool  vBack = HitIsBackface(vq.CommittedInstanceID(),
                                                        vq.CommittedPrimitiveIndex(),
                                                        vq.CommittedWorldToObject3x4(),
                                                        vray.Direction, vTwoSided);
                            float vt    = vq.CommittedRayT();

                            float vSkip = -1.0f;
                            if (vTwoSided && vt < kSelfIsectTwoSidedDist)
                                vSkip = vt + kSelfIsectRetraceBias;
                            else if (!vTwoSided && vBack && vt < kSelfIsectBackfaceDist)
                                vSkip = kSelfIsectBackfaceDist;

                            if (vSkip > 0.0f) {
                                vray.TMin = vSkip;
                                vq.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, vray);
                                SMILE_RT_PROCEED(vq)
                                vray.TMin = 0.0f;
                                vBack = (vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) &&
                                        HitIsBackface(vq.CommittedInstanceID(),
                                                      vq.CommittedPrimitiveIndex(),
                                                      vq.CommittedWorldToObject3x4(),
                                                      vray.Direction, vTwoSided);
                            }
                            vKill = vBack && !vTwoSided &&
                                    vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                        }

                        if (vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                            float t = vq.CommittedRayT();
                            if (abs(t - len) <= max(kRevalidateRelTol * len, kRevalidateAbsTol)) {
                                float vsd;
                                prev.Lo = vKill
                                    ? float3(0.0f, 0.0f, 0.0f)
                                    : ShadeSurfaceHit(vq.CommittedInstanceID(),
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
