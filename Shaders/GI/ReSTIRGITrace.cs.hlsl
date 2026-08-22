// ReSTIR GI Pass A: candidato novo, reuso temporal e resolve preliminar.
// O Pass B substitui o resolve quando o reuso espacial está ativo.
// Referência: Ouyang et al. 2021.

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
    float4 TraceParams;             // x=frameIndex, y=maxRayDist, z=skyIntensity, w=shadowRayBias
    float4 ShadeParams;             // x=livre, y=albedoLOD, z=fireflyMaxLuma, w=validateInterval
    float4 ReuseParams;             // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    float4 SpatialParams;           // x=spatialRadius, y=spatialCount, z=spatial(0/1), w=normalReject
    float4 JitterParams;            // xy=jitter delta, z=nº luzes, w=maxAge (0 desliga)
    row_major float4x4 View;        // nao usado aqui; declarado p/ os offsets do CB baterem
    float4 RayEpsA;                 // x=originFloorMin, y=originFloorPerMeter, z=angularMax,
                                    // w=shadowRayBiasMin  (perfil de epsilons — knobs do editor)
    float4 RayEpsB;                 // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin,
                                    // w=angularMinRatio
    float4 PolicyParams;            // x=backface, y=boiling, z=bias temporal, w=Jacobiano
    float4 GIDistParams;            // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;            // x=escala do bias, y=teto em metros, z=fade de sondas,
                                    // w=piso de roughness do hit (cache nao-direcional)
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;               // x = view height (km), y = raio do planeta (km) — ShadeSky
    float4 DebugParams;             // x = slot bindless do alvo de timer (< 0 = captura off)
    float4 HistoryParams;           // x=Surface anterior, y=histórico full-res válido
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 TraceScreenParams;        // xy=dominio trace/reservoir, zw=inverso
    float4 ResolutionParams;         // x=escala (1/2), yz=fase 2x2, w=half-res
};

#include "../RayOffset.hlsli"
#include "../Debug/ShaderTimer.hlsli"

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t5);
Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7);
Texture2D<float2>               Velocity   : register(t8);
// t11/t12 são fillers para preservar as luzes em t13.
Texture2D<float4>               PrevRes0   : register(t9);  // x2.xyz, W
Texture2D<uint4>                PrevRes1   : register(t10); // n2 | Lo | M+idade | n1

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t13); // F5: luzes puntuais nos hits

RWTexture2D<float4>             GIOut    : register(u0); // rgb=gi, a=hitDist (NRD)
RWTexture2D<float4>             CurrRes0 : register(u1); // x2.xyz, W
RWTexture2D<uint4>              CurrRes1 : register(u2); // n2 | Lo | M+idade | n1

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "HitShading.hlsli"
#include "ReSTIRReservoir.hlsli"

// Fonte do candidato novo. DebugParams.y < 0 desliga.
void WriteSourceViz(uint2 px, uint kind) {
    if (DebugParams.y < 0.0f) return;
    RWTexture2D<float4> dst =
        ResourceDescriptorHeap[NonUniformResourceIndex((uint)DebugParams.y)];
    dst[px] = float4(RC_SourceColor(kind), 1.0f);
}

uint2 FullPixelFromTrace(uint2 tracePx, uint2 phase) {
    const uint scale = max((uint)ResolutionParams.x, 1u);
    return min(tracePx * scale + phase, uint2(ScreenParams.xy) - 1u);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)TraceScreenParams.x || px.y >= (uint)TraceScreenParams.y)
        return;

    const uint2 phase = uint2(ResolutionParams.yz);
    const uint2 surfacePx = FullPixelFromTrace(px, phase);

    float deviceZ = Depth.Load(int3(surfacePx, 0)).r;
    if (deviceZ <= 0.0f) {
        GIOut[px]    = float4(0.0f, 0.0f, 0.0f, 0.0f);
        CurrRes0[px] = 0.0f; CurrRes1[px] = uint4(0u, 0u, 0u, 0u);
        WriteSourceViz(px, RC_SRC_NOSURFACE);
        return;
    }

    SMILE_TIMER_BEGIN(timerStart)

    float4 gb = GBuffer.Load(int3(surfacePx, 0));
    float3 N  = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float2 uv  = (surfacePx + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 x1  = wH.xyz / wH.w;
    float  camDist = length(CameraPos.xyz - x1);

    uint rng = RngSeed(surfacePx, (uint)TraceParams.x, SMILE_RNG_GI_WRS);

    // --- (1) Sample inicial: raio cosseno-hemisferico (Malley) -------------------------------
    float2 E    = GGX_Rand2E(surfacePx, (uint)TraceParams.x, SMILE_RNG_GI_INITIAL);
    float  rr   = sqrt(E.x);
    float  phi  = 2.0f * SMILE_PI * E.y;
    float  cosT = sqrt(saturate(1.0f - E.x));
    float3 d    = float3(rr * cos(phi), rr * sin(phi), cosT);
    float3x3 basis = GGX_TangentBasis(N);
    float3 dir  = normalize(mul(d, basis));

    float3 sunDir = normalize(SunDirIntensity.xyz);

    // Offset numérico anti self-hit; x2 continua no hit real.
    RayDesc ray;
    ray.Origin    = OffsetRayGBuffer(x1, N, dir, camDist);
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;
    // Sem culling: a política abaixo precisa classificar o verso.
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
    P.NumLights      = (int)JitterParams.z; // F5
    P.ShadowRayMask  = (uint)SunColor.w;
    P.ReGIRGridMin       = ReGIRGridMinSlots.xyz;
    P.ReGIRSlotsPerCell  = (uint)ReGIRGridMinSlots.w;
    P.ReGIRInvCellSize   = ReGIRInvCellEnabled.xyz;
    P.ReGIREnabled       = ReGIRInvCellEnabled.w > 0.5f;
    P.ReGIRGridCount     = (int3)ReGIRGridCountSamples.xyz;
    P.ReGIRSampleCount   = (int)ReGIRGridCountSamples.w;
    P.ReGIRSlotsSRV      = (uint)ReGIRResources.x;
    P.ReGIRAverageSRV    = (uint)ReGIRResources.y;
    P.FrameIndex         = (uint)TraceParams.x;
    P.ReGIRPad           = 0u;
    P.SkyViewHeightKm    = SkyParams.x;
    P.SkyBottomRKm       = SkyParams.y;
    // O cache é não direcional.
    P.RoughnessMin       = GIBiasParams.w;
    P.CacheRayRoughness  = -1.0f;
    RC_UNPACK_PARAMS(P);

    // Retrace descarta auto-interseção próxima; verso one-sided restante encerra o caminho.
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
            backface = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) &&
                       HitIsBackface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                     q.CommittedWorldToObject3x4(), ray.Direction, twoSided);
        }

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
        // Caminho morto contribui zero, mas continua sendo um hit para o NRD.
        uint srcKind = RC_SRC_KILLED; // sobrescrito pelo Ex quando o caminho nao morre
        Lo = killPath ? float3(0.0f, 0.0f, 0.0f)
                      : ShadeSurfaceHitEx(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                          q.CommittedTriangleBarycentrics(),
                                          q.CommittedWorldToObject3x4(),
                                          ray.Origin, ray.Direction, hitDist, P, sd, srcKind);
        WriteSourceViz(px, srcKind);
    } else {
        hitDist = TraceParams.y;
        Lo = ShadeSky(dir, sunDir, P.SkyIntensity, P);
        x2 = ray.Origin + ray.Direction * hitDist; // ponto distante na direcao do ceu
        n2 = -dir;                                  // normal "virada" p/ o ponto visivel
        WriteSourceViz(px, RC_SRC_SKY);
    }

    // Limita outliers antes do WRS.
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

    // Domínio reprojetado usado pela correção de viés.
    bool   tempMerged = false; // o reservoir anterior entrou no WRS
    bool   tempPicked = false; // ... e a amostra dele venceu a selecao
    float3 tempX1     = 0.0f;
    float3 tempN1     = 0.0f;
    float  tempM      = 0.0f;  // M ja limitado pelo MCap (e o mesmo que pesou o merge)

    // --- (2) Reuso temporal ------------------------------------------------------------------
    if (ReuseParams.w > 0.5f) {
        float2 vel    = Velocity.Load(int3(surfacePx, 0)).rg;
        float2 prevUv = uv - vel;
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            int2 ppx = int2(prevUv * TraceScreenParams.xy);

            // Permutação bijetiva reduz correlação do reuso temporal (RTXDI).
            bool permOk;
            {
                uint  rnd    = GGX_PCG((uint)TraceParams.x);
                int2  offset = int2(rnd & 3u, (rnd >> 2u) & 3u);
                int2  perm   = ppx + offset;
                perm.x ^= 3; perm.y ^= 3;
                perm -= offset;
                // Fallback para ppx quebraria a bijeção.
                permOk = all(perm >= int2(0, 0)) && all(perm < int2(TraceScreenParams.xy));
                if (permOk) ppx = perm;
            }

            uint4 p1 = PrevRes1.Load(int3(ppx, 0));
            float prevM, prevAge;
            ResUnpackMAge(p1.z, prevM, prevAge);

            // x1 vem do histórico de superfície e só é lido para reservoir válido.
            float3 prevX1 = 0.0f;
            float3 prevN1 = 0.0f;
            bool accept = permOk && prevM > 0.0f;
            if (accept) {
                Texture2D<float4> PrevSurface = ResourceDescriptorHeap[(uint)HistoryParams.x];
                const uint prevFrame = (uint)TraceParams.x - 1u;
                const uint2 prevPhase = (ResolutionParams.w > 0.5f)
                    ? uint2(prevFrame & 1u, (prevFrame >> 1u) & 1u) : uint2(0u, 0u);
                const uint2 prevSurfacePx = FullPixelFromTrace(uint2(ppx), prevPhase);
                prevX1 = PrevSurface.Load(int3(prevSurfacePx, 0)).xyz;
                prevN1 = ResUnpackNormal(p1.w);
                float  posReject = ReuseParams.y * max(camDist, 1.0f);
                float  planeDist = abs(dot(N, prevX1 - x1));
                accept = dot(prevN1, N) >= SpatialParams.w &&
                         length(prevX1 - x1) < posReject && planeDist < 0.2f * posReject;
            }

            if (accept) {
                float4 p0 = PrevRes0.Load(int3(ppx, 0));
                Reservoir prev;
                prev.x1 = prevX1;                 prev.x2 = p0.xyz;
                prev.n2 = ResUnpackNormal(p1.x);  prev.Lo = ResUnpackRadiance(p1.y);
                prev.M  = min(prevM, ReuseParams.x); // MCap
                prev.W  = p0.w; prev.wSum = 0.0f;

                // Expiração escalonada evita ondas sincronizadas.
                bool prevValid = true;
                float maxAge = JitterParams.w;
                if (maxAge > 0.0f) {
                    float stagger = 0.75f + 0.5f * float(GGX_PCG(px.x * 7919u + px.y) & 0xFFu) / 255.0f;
                    if (prevAge >= maxAge * stagger) prevValid = false;
                }

                // Re-shade escalonado acompanha iluminação e geometria dinâmicas.
                uint validN = (uint)ShadeParams.w;
                if (prevValid && validN > 0u &&
                    ((uint)TraceParams.x + GGX_PCG(px.x + GGX_PCG(px.y))) % validN == 0u) {
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
                        // Revalidação precisa reencontrar a mesma superfície.
                        RayQuery<RAY_FLAG_NONE> vq;
                        vq.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, vray);
                        SMILE_RT_PROCEED(vq)

                        // Aplica a mesma política de backface do gather.
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
                                // Atualiza a geometria aceita pela tolerância.
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
                            prev.Lo = ShadeSky(vray.Direction, sunDir, P.SkyIntensity, P); // era ceu
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
                    // Rejeita Jacobiano temporal extremo em vez de clampá-lo.
                    float J = ReconnectionJacobian(x1, prev.x1, prev.x2, prev.n2,
                                                   PolicyParams.w > 0.5f);
                    if (J >= 0.1f && J <= 10.0f) {
                        float pHatPrev = TargetPHat(x1, N, prev.x2, prev.Lo);
                        tempMerged = true;
                        tempX1 = prev.x1; tempN1 = prevN1; tempM = prev.M;
                        if (ResMerge(r, prev, pHatPrev, J, rng)) {
                            age = prevAge + 1.0f;
                            tempPicked = true;
                        }
                    }
                }
            }
        }
    }

    // --- (3) Correção de viés temporal e resolve preliminar -------------------------------
    // PolicyParams.z alterna MIS básico e a normalização histórica 1/M.
    {
        float pHatSel = TargetPHat(x1, N, r.x2, r.Lo);
        float pi, piSum;
        if (PolicyParams.z > 0.5f) {
            float temporalP = tempMerged ? TargetPHat(tempX1, tempN1, r.x2, r.Lo) : 0.0f;
            pi    = tempPicked ? temporalP : pHatSel;
            piSum = pHatSel + temporalP * tempM;   // M do sample inicial e 1
        } else {
            // Caso 1/M expresso pelo mesmo helper.
            pi    = pHatSel;
            piSum = pHatSel * (1.0f + tempM);
        }
        ResFinalizeMIS(r, pHatSel, pi, piSum);
    }
    // Boiling filter relativo à wave; esvazia o reservoir para remover o outlier persistente.
    if (PolicyParams.y > 0.0f) {
        const float bw    = ReSTIR_Luminance(r.Lo) * r.W;
        const float bSum  = WaveActiveSum(bw);
        const uint  bCnt  = WaveActiveCountBits(bw > 0.0f);
        const float bAvg  = (bCnt > 0u) ? (bSum / (float)bCnt) : 0.0f;
        // Multiplicador do RTXDI: 10/strength - 9.
        const float bMult = 10.0f / clamp(PolicyParams.y, 1e-6f, 1.0f) - 9.0f;
        if (bAvg > 0.0f && bw > bAvg * bMult) {
            ResInit(r);
            age = 0.0f;
        }
    }

    float3 gi = ResResolve(r, x1, N, ShadeParams.z);

    // O NRD recebe a distância da amostra vencedora.
    float selDist = (r.wSum > 0.0f) ? length(r.x2 - x1) : hitDist;
    GIOut[px]    = float4(gi, selDist);
    CurrRes0[px] = float4(r.x2, r.W);
    CurrRes1[px] = ResPack1(r, age, N); // n1 vai junto: o temporal do proximo frame rejeita por ela

    SMILE_TIMER_END(timerStart, px, DebugParams.x)
}
