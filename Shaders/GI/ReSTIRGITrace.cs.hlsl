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
    float4 ShadeParams;             // x=livre, y=albedoLOD, z=fireflyMaxLuma, w=validateInterval
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
    float4 PolicyParams;            // x = politica deste passe (backface/culling),
                                    // y = strength do boiling filter (0 = desligado),
                                    // z = correcao de vies do temporal (0 = 1/M historico),
                                    // w = kill de backface no Jacobiano (0 = abs() historico)
    // Gather do 2o bounce (contrato do HitShading.hlsli).
    float4 GIDistParams;            // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;            // x=escala do bias, y=teto em metros, z=fade de sondas,
                                    // w=piso de roughness do hit (cache nao-direcional)
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;               // x = view height (km), y = raio do planeta (km) — ShadeSky
    float4 DebugParams;             // x = slot bindless do alvo de timer (< 0 = captura off)
    float4 HistoryParams;           // x = slot bindless do historico de superficie do frame
                                    //     ANTERIOR (FTemporalMotionVectors::SurfaceSRV). E de la
                                    //     que sai o x1 do reservoir temporal, que deixou de ser
                                    //     gravado. O Renderer zera ReuseParams.w quando o slot nao
                                    //     existe, entao aqui ele e sempre valido quando lido.
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    // Cascatas do DDGI, consumidas pelo gather do 2o bounce no HitShading (contrato por NOME).
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
};

// Depois do cbuffer: os dois headers leem RayEpsA/RayEpsB (ver o contrato no RayOffset.hlsli).
#include "../RayOffset.hlsli"
#include "../Debug/ShaderTimer.hlsli"

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
// Reservoir do frame anterior em DUAS texturas (era quatro): ver o cabecalho de empacotamento em
// ReSTIRReservoir.hlsli. t11/t12 continuam filler porque a tabela tem 14 descritores e
// SetPunctualLightsSRV escreve as luzes no offset 13 na unha.
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

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) {
        GIOut[px]    = float4(0.0f, 0.0f, 0.0f, 0.0f);
        CurrRes0[px] = 0.0f; CurrRes1[px] = uint4(0u, 0u, 0u, 0u);
        return;
    }

    // Comeca DEPOIS do descarte de ceu: pixel sem geometria nao traca nada e entraria como
    // "frio" de qualquer jeito — medir o early-out so somaria ruido ao piso do heatmap.
    SMILE_TIMER_BEGIN(timerStart)

    float4 gb = GBuffer.Load(int3(px, 0));
    float3 N  = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 x1  = wH.xyz / wH.w;
    float  camDist = length(CameraPos.xyz - x1);

    // Streams por efeito (ver GGXSample.hlsli). Antes isto era um XOR magico no frame, que
    // decorrelacionava o WRS da direcao do raio AQUI DENTRO mas deixava o GI sorteando a mesma
    // sequencia das reflexoes — correlacao entre efeitos, que o Ray Reconstruction delata.
    uint rng = RngSeed(px, (uint)TraceParams.x, SMILE_RNG_GI_WRS);

    // --- (1) Sample inicial: raio cosseno-hemisferico (Malley) -------------------------------
    float2 E    = GGX_Rand2E(px, (uint)TraceParams.x, SMILE_RNG_GI_INITIAL);
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
    // Cache NAO-direcional (o reservoir entrega o mesmo Lo a vizinhos com outra visada), entao
    // o piso de roughness vale aqui — ver o bloco no ShadeSurfaceHit.
    P.RoughnessMin       = GIBiasParams.w;
    P.CacheRayRoughness  = -1.0f;
    RC_UNPACK_PARAMS(P);

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
        Lo = ShadeSky(dir, sunDir, P.SkyIntensity, P);
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

    // Dominio do reservoir reprojetado, guardado p/ a correcao de vies do passo (3). Sao os
    // MESMOS dados que o teste de aceitacao ja carrega (ponto visivel, normal e M do frame
    // anterior) — a correcao nao custa leitura nova, so os dois TargetPHat do fim.
    bool   tempMerged = false; // o reservoir anterior entrou no WRS
    bool   tempPicked = false; // ... e a amostra dele venceu a selecao
    float3 tempX1     = 0.0f;
    float3 tempN1     = 0.0f;
    float  tempM      = 0.0f;  // M ja limitado pelo MCap (e o mesmo que pesou o merge)

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
            // manchas/rastejo; re-introduzir so com A/B dedicado. Unpack do M mantido (Res1.z
            // fica no formato M+idade empacotados; expiracao hoje desligada via MaxAge=0).
            int2 ppx = int2(prevUv * ScreenParams.xy);

            // Permutation sampling (RTXDI_ApplyPermutationSampling, RtxdiHelpers.hlsli). SEMPRE
            // ligado: o A/B de 2026-07-29 eliminou o sparkle do DLSS-RR e nao houve caso a favor
            // do caminho antigo, entao o toggle saiu junto.
            //
            // O DLSS-RR Integration Guide §3.5 exige amostras independentes e diz explicitamente:
            // "RR assumes independent samples, which is violated by ReSTIR temporal and spatial
            // reuse. Permutation sampling helps avoid correlation artifacts."
            //
            // POR QUE ISTO NAO E A BUSCA 2x2 QUE REGREDIU NO BISECT: o numero aleatorio e
            // UNIFORME NA TELA (derivado so do frame), entao o mapa e uma BIJECAO — cada texel do
            // frame anterior e lido por EXATAMENTE UM pixel atual. A busca "melhor x1" nao era
            // bijetiva: varios pixels vizinhos podiam convergir no mesmo reservoir e duplicar uma
            // amostra brilhante pela vizinhanca, que e o mecanismo da mancha/random walk. Esta
            // troca embaralha QUEM le QUEM sem alterar a contagem de amostras.
            //
            // PONTO DE ATENCAO (nao portado): o RTXDI DESLIGA a permutacao em superficie fina/alto
            // detalhe (`usePermutationSampling = !IsComplexSurface(...)`, TemporalResampling.hlsl),
            // com o comentario "Permutation sampling makes more noise on thin, high-detail
            // objects". Folhagem e exatamente esse caso e ja e ponto sensivel na engine. Se
            // aparecer cintilacao NOVA em vegetacao, suspeitar daqui ANTES de mexer em alpha-test
            // ou upscaler: o conserto e o gate por complexidade, nao remover a permutacao.
            bool permOk;
            {
                uint  rnd    = GGX_PCG((uint)TraceParams.x);
                int2  offset = int2(rnd & 3u, (rnd >> 2u) & 3u);
                int2  perm   = ppx + offset;
                perm.x ^= 3; perm.y ^= 3;
                perm -= offset;
                // Fora da tela nao ha parceiro: ABANDONA o reuso deste pixel em vez de cair no ppx
                // original. Cair de volta quebraria a bijecao (o texel do ppx ja e lido por outro
                // pixel) e reintroduziria justamente a duplicacao que a permutacao existe p/
                // evitar. Afeta so uma borda de 3 px. Nao pode ser `return`: o reservoir do sample
                // inicial ainda precisa ser gravado no fim do main.
                permOk = all(perm >= int2(0, 0)) && all(perm < int2(ScreenParams.xy));
                if (permOk) ppx = perm;
            }

            uint4 p1 = PrevRes1.Load(int3(ppx, 0));
            float prevM, prevAge;
            ResUnpackMAge(p1.z, prevM, prevAge);

            // O x1 do frame anterior vem do HISTORICO DE SUPERFICIE, nao mais do reservoir: o
            // FTemporalMotionVectors grava `InvViewProj * (ndc, deviceZ)` com a mesma matriz
            // (InvViewProjFull) e o mesmo depth que este passe usa, entao o valor e identico ao
            // que era gravado aqui — e sai 12 B/pixel do reservoir.
            //
            // A leitura fica DENTRO do gate de prevM: reservoir zerado (clear de historico,
            // disoclusao, ceu) nunca toca a textura de superficie, que pode estar velha de um
            // frame em que o passe de motion confiavel nao rodou. Sem esse gate, uma posicao
            // obsoleta poderia passar nos testes geometricos por coincidencia.
            float3 prevX1 = 0.0f;
            // prevN1 subiu de escopo: alem do teste de aceitacao, ela e a normal do DOMINIO
            // anterior na correcao de vies do passo (3).
            float3 prevN1 = 0.0f;
            bool accept = permOk && prevM > 0.0f;
            if (accept) {
                Texture2D<float4> PrevSurface = ResourceDescriptorHeap[(uint)HistoryParams.x];
                prevX1 = PrevSurface.Load(int3(ppx, 0)).xyz;
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
                    // Jacobiano TEMPORAL: a aceitacao tolera prev.x1 divergindo ate ~1% da
                    // camDist — com segmento indireto curto (contatos) isso muda a medida de
                    // verdade (J=1 era aproximacao enviesada). Mesma politica do espacial:
                    // J extremo REJEITA (clampar mantem o sample com peso errado).
                    float J = ReconnectionJacobian(x1, prev.x1, prev.x2, prev.n2,
                                                   PolicyParams.w > 0.5f);
                    if (J >= 0.1f && J <= 10.0f) {
                        float pHatPrev = TargetPHat(x1, N, prev.x2, prev.Lo);
                        // Registra o dominio ANTES do merge: a correcao de vies precisa dele
                        // tenha a amostra vencido ou nao (quem perde ainda vota no denominador).
                        tempMerged = true;
                        tempX1 = prev.x1; tempN1 = prevN1; tempM = prev.M;
                        // Amostra do historico sobreviveu a selecao -> envelhece 1 frame.
                        if (ResMerge(r, prev, pHatPrev, J, rng)) {
                            age = prevAge + 1.0f;
                            tempPicked = true;
                        }
                    }
                }
            }
        }
    }

    // --- (3) Correcao de vies do temporal + resolve (sobrescrito pelo Pass B c/ spatial ON) ---
    //
    // Era 1/M (W = wSum / (M * pHat)). O argumento que sustentava isso era "a reprojecao cai no
    // MESMO ponto visivel, entao os dois dominios sao o mesmo e a correcao vira no-op" — e ele
    // MORREU quando a permutation sampling entrou (ver o bloco no passo 2): a permutacao desloca
    // a leitura em ate 3 texels, o x1 anterior deixa de ser o atual e os dominios passam a ser
    // diferentes de verdade. A RTXDI, que tambem embaralha, corrige por default
    // (RTXDI_GITemporalResampling, biasCorrectionMode = Basic); era a Smile que estava fazendo o
    // oposto nos dois eixos (permutacao sempre ligada + 1/M).
    //
    // Mesma conta do Pass B, agora no helper compartilhado. Dominios: o proprio pixel (M = 1, o
    // sample inicial) e o reservoir reprojetado (M = tempM). O `pi` e o pHat do vencedor NO
    // DOMINIO QUE O GEROU; o `piSum` soma os dois dominios ponderados pelo M de cada um.
    // pHatSel serve de pi quando o vencedor veio do sample inicial porque o dominio dele E o
    // pixel atual — igual ao `selectedTargetPdf` da RTXDI.
    // DEFAULT OFF (PolicyParams.z) apos o A/B de 2026-08-07 — ver o porque no bloco abaixo.
    {
        float pHatSel = TargetPHat(x1, N, r.x2, r.Lo);
        float pi, piSum;
        if (PolicyParams.z > 0.5f) {
            float temporalP = tempMerged ? TargetPHat(tempX1, tempN1, r.x2, r.Lo) : 0.0f;
            pi    = tempPicked ? temporalP : pHatSel;
            piSum = pHatSel + temporalP * tempM;   // M do sample inicial e 1
        } else {
            // 1/M historico, escrito como CASO PARTICULAR do mesmo helper: com pi = pHatSel e
            // piSum = pHatSel*(1 + tempM), a conta vira wSum / (M_total * pHatSel), que e
            // exatamente o ResFinalizeW de antes (r.M == 1 + tempM por construcao). Uma via de
            // codigo so p/ os dois modos — o A/B nao compara caminhos diferentes.
            pi    = pHatSel;
            piSum = pHatSel * (1.0f + tempM);
        }
        ResFinalizeMIS(r, pHatSel, pi, piSum);
    }
    //
    // POR QUE A CORRECAO NASCEU LIGADA E VOLTOU P/ OFF (A/B do usuario, 2026-08-07):
    //
    // Ela e fiel a RTXDI (RTXDI_GITemporalResampling, modo Basic) e o 1/M e de fato enviesado com
    // a permutation sampling ligada — nada disso mudou. O que mudou foi a medida: com ela ligada o
    // caminho do Ray Reconstruction encheu de firefly TRAVADO no mundo, e o boiling filter nao
    // conteve.
    //
    // O mecanismo e um LOOP, nao um pico isolado. `W_novo/W_velho = M_total*pi/piSum` chega a
    // M_total = 1 + MCap = 21 quando o dominio anterior nao consegue gerar a amostra vencedora
    // (temporalP -> 0, tipico de canto concavo com normal-map). Ate aqui e o que a RTXDI aceita.
    // A diferenca e que AQUI o W inflado e GRAVADO no reservoir e volta como `prev.W` no frame
    // seguinte, sem nada que o amorteca: a RTXDI realimenta o temporal com a saida do ESPACIAL
    // (temporalResamplingInputBufferIndex = spatialResamplingOutputBufferIndex no modo
    // TemporalAndSpatial), e o espacial dilui o outlier entre K vizinhos. Nesta engine o espacial
    // NAO realimenta — o temporal e um laco fechado, e o pico se acumula em vez de decair.
    //
    // O Lumen roda sem realimentacao igual a nós, mas com o temporal 1/M e Jacobiano DESLIGADO,
    // isto e, um temporal fortemente amortecido. A combinacao "sem realimentacao espacial +
    // temporal corrigido por MIS + MCap 20" nao existe em nenhuma das duas referencias.
    //
    // P/ religar isto, o pre-requisito NAO e parametro, e estrutura — uma destas:
    //   (a) realimentar o temporal com a saida do Pass B (vira o desenho da RTXDI), ou
    //   (b) MCap 8 (teto da RTXDI) junto de boiling filter validado, aceitando o loop com ganho
    //       menor.
    // Religar so pelo knob, sem (a) ou (b), reproduz o artefato — foi exatamente isso que o A/B
    // mostrou.

    // Boiling filter (RTXDI_GIBoilingFilter + Utils/BoilingFilter.hlsli, ligado por DEFAULT la com
    // strength 0.2). O firefly clamp da engine NAO cobre este caso: ele limita o Lo da amostra e o
    // gi resolvido, mas o outlier nasce do W — um pixel parado no teto do clamp ainda fica ordens
    // de grandeza acima do vizinho num muro escuro, que e o ponto branco fixo que aparece no RR (o
    // NRD borra, o Ray Reconstruction reconstroi e preserva). O teste do boiling e RELATIVO a
    // vizinhanca, e e isso que separa "amostra rara porem legitima" de "reservoir travado".
    //
    // Esvaziar o reservoir, e nao so zerar o gi do frame: com M = 0 o proximo frame nao le mais a
    // amostra presa (o gate `prevM > 0` do reuso temporal falha) e o pixel recomeca do sample
    // inicial. E o que mata o ponto FIXO; zerar so a saida deixaria ele voltar no frame seguinte.
    //
    // Diferenca deliberada p/ a RTXDI: a media e por WAVE, nao pelo grupo 8x8 inteiro. A versao
    // dela usa groupshared + GroupMemoryBarrierWithGroupSync, e este passe tem dois early-out
    // (fora da tela e ceu) ANTES daqui — barreira sob divergencia e comportamento indefinido.
    // WaveActiveSum opera nas lanes ATIVAS, entao a variante de wave e correta sem reestruturar o
    // main; a vizinhanca cai de 8x8 p/ 8x4 (wave de 32) ou continua 8x8 (wave de 64).
    if (PolicyParams.y > 0.0f) {
        const float bw    = ReSTIR_Luminance(r.Lo) * r.W;
        const float bSum  = WaveActiveSum(bw);
        const uint  bCnt  = WaveActiveCountBits(bw > 0.0f);
        const float bAvg  = (bCnt > 0u) ? (bSum / (float)bCnt) : 0.0f;
        // Multiplicador da RTXDI: 10/strength - 9 (0.2 -> corta acima de 41x a media da vizinhanca).
        const float bMult = 10.0f / clamp(PolicyParams.y, 1e-6f, 1.0f) - 9.0f;
        if (bAvg > 0.0f && bw > bAvg * bMult) {
            ResInit(r);
            age = 0.0f;
        }
    }

    float3 gi = ResResolve(r, x1, N, ShadeParams.z);

    // hitDist p/ o NRD = distancia da amostra VENCEDORA do WRS (o temporal pode trocar x2; o hitT
    // do raio inicial guiaria o denoiser com um caminho diferente do da radiancia resolvida).
    float selDist = (r.wSum > 0.0f) ? length(r.x2 - x1) : hitDist;
    GIOut[px]    = float4(gi, selDist);
    CurrRes0[px] = float4(r.x2, r.W);
    CurrRes1[px] = ResPack1(r, age, N); // n1 vai junto: o temporal do proximo frame rejeita por ela

    // Fecha depois das escritas do reservoir: elas fazem parte do custo do passe.
    SMILE_TIMER_END(timerStart, px, DebugParams.x)
}
