// World radiance cache — PASSE DEDICADO DE UPDATE (Fase 3, Docs/SHARC-PRIMARY-GI-PLAN.md).
//
// O QUE MUDA EM RELACAO AO PRODUTOR ANTERIOR
// Ate aqui o cache aprendia de carona: todo hit do render chamava RC_Update com a radiancia que
// aquele hit produziu — e essa radiancia tinha o DDGI como terminador. O cache guardava, portanto,
// um sinal cuja origem continuava sendo o DDGI, e trocar de primario nao trocaria de fonte.
// Este passe e a fonte propria: os traces de render passam a so CONSULTAR (o CPU para de armar o
// bit de update deles) e quem escreve e so este dispatch, que nunca le DDGI.
//
// O QUE ELE GRAVA, E POR QUE O VERTICE DO G-BUFFER NAO ENTRA
// O cache guarda RADIANCIA DE SAIDA de um ponto do mundo. Este passe usa o pixel do G-buffer
// apenas como ORIGEM de um raio; o vertice gravado e o PRIMEIRO HIT desse raio — que e exatamente
// a populacao de pontos que as consultas do render acertam (elas nascem de raios secundarios).
// A superficie primaria fica de fora de proposito: a radiancia dela e produzida pelo deferred, com
// CSM e outra cadeia de luz, e grava-la exigiria um SEGUNDO caminho de material a partir do
// G-buffer — a copia divergente que a Fase 2 existiu para impedir.
//
// UM BOUNCE, MULTI-BOUNCE NO TEMPO
// Este commit traca dois raios: G-buffer -> v0 (o vertice que se grava) e v0 -> terminal. O
// terminal e o cache RESOLVIDO (frames anteriores), o ceu, ou zero — nunca DDGI. Como a escrita
// vai para `Accum` e a leitura vem de `Resolved`, a realimentacao e uma iteracao de ponto fixo
// entre FRAMES: L_novo = direta + f*L_velho. Com f < 1 ela converge para o transporte completo,
// que e o que a SHaRC chama de backpropagation com o cache do frame anterior. O laco de ate 4
// vertices DENTRO do frame (e a backpropagation em ordem reversa) e o commit seguinte.
//
// ORDEM NO FRAME (decidida no plano, nao re-derivar): depois do RecordGBuffer e antes do
// RecordSceneLighting. Nao em PrepareIndirectLighting, que roda antes de o G-buffer existir.

#include "DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"

cbuffer RadianceCacheUpdateCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;      // x=W, y=H, z=1/W, w=1/H
    // Grade GROSSA do DDGI + atlas. Declarados porque o FHitShadeParams os exige; este passe nao
    // amostra sonda nenhuma (ver PT_SampleIndirectFallback, que ele nunca chama).
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;   // xyz = direcao P/ o sol, w = intensidade
    float4 SunColor;          // rgb = cor, w = mask dos shadow rays
    float4 TraceParams;       // x=frameIndex, y=maxRayDist, z=skyIntensity, w=shadowRayBias
    float4 ShadeParams;       // x=nº de luzes puntuais, y=albedoLOD, zw=livres
    // x = politica de auto-interseccao/backface (0/1). O MESMO toggle do ReSTIR GI, empurrado
    // pelo Renderer — ver o bloco no v0 sobre por que ele nao e knob proprio.
    float4 PolicyParams;
    // x = celulas do tile 5x5 sorteadas por frame (1 = 4% dos pixels)
    // y = vertices do caminho; vale 1 e NAO e lido aqui — o laco chega no commit seguinte
    // z = consultar o cache resolvido no terminal (0/1)
    float4 UpdateParams;
    float4 RayEpsA;           // perfil de epsilons (contrato do RayOffset.hlsli)
    float4 RayEpsB;
    float4 GIDistParams;      // contrato do HitShading; so o skipMode do .w e lido (pelo gather
                              // que este passe nao chama) — fica pelo contrato de NOME
    float4 GIBiasParams;      // .w = piso de roughness do secundario, e este SIM e lido
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;         // x = view height (km), y = raio do planeta (km) — ShadeSky
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    // Cascatas do DDGI. Chegam ZERADAS: o unico leitor seria o gather do fallback, e o updater
    // nao pode chama-lo. Existem porque o contrato por NOME do HitShading.hlsli exige que o
    // cbuffer as declare para o arquivo compilar. Ver RadianceCacheUpdateConstants.
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
};

RaytracingAccelerationStructure Scene       : register(t0);
Texture2D<float4>               SkyViewLUT  : register(t1);
StructuredBuffer<InstanceGeo>   Instances   : register(t2);
// t3/t4/t5: atlas e ProbeData do DDGI. Este passe NAO os le — sao os slots NEUTROS quando nao ha
// volume, e continuam validos quando ha. Estao aqui porque o HitShading.hlsli declara o gather de
// fallback, que precisa dos recursos declarados para compilar; a funcao e eliminada por nao ter
// chamador, e e essa ausencia de chamador que garante "o update nunca le DDGI", nao um gate.
Texture2D<float4>               IrradAtlas  : register(t3);
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t5);
Texture2D<float>                Depth       : register(t6);
Texture2D<float4>               GBuffer     : register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

// Depois do cbuffer: os headers leem RayEpsA/RayEpsB (contrato do RayOffset.hlsli).
#include "../RayOffset.hlsli"
#include "HitShading.hlsli"

// SELECAO ESPARSA — ~4% dos pixels por frame, sem buracos.
//
// Permutacao de periodo 25 sobre o tile 5x5, e nao um sorteio independente por pixel: com sorteio,
// a fracao e respeitada NA MEDIA mas um pixel qualquer pode passar dezenas de frames sem nunca ser
// escolhido, e as celulas atras dele envelhecem ate o despejo. Aqui cada posicao do tile dispara
// exatamente uma vez a cada 25 frames.
//
// O `* 13` e a permutacao: 13 e coprimo com 25, entao tile -> frame e uma bijecao, e frames
// consecutivos caem em posicoes 2 apart no tile (13*2 = 26 = 1 mod 25) em vez de varrerem o tile
// em ordem de raster — o padrao do frame nao "escorrega" pela tela.
bool RCU_PixelSelected(uint2 px, uint frame, uint cellsPerFrame) {
    const uint tile = (px.y % 5u) * 5u + (px.x % 5u);
    const uint slot = (tile * 13u) % 25u;
    return ((slot + frame) % 25u) < cellsPerFrame;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    const uint frameIndex = (uint)TraceParams.x;
    if (!RCU_PixelSelected(px, frameIndex, (uint)UpdateParams.x)) return;

    // Ceu: nao ha superficie de onde partir. (O ceu tambem nao e celula de cache — ele e um
    // TERMINADOR, e entra la embaixo pelo ShadeSky.)
    const float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) return;

    const float4 gb = GBuffer.Load(int3(px, 0));
    const float3 N  = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    const float2 uv  = (px + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 x1  = wH.xyz / wH.w;
    const float  camDist = length(CameraPos.xyz - x1);

    // Direcao de ORIGEM: cosseno-hemisferica em torno da normal do G-buffer. Ela nao pesa energia
    // nenhuma — o que se grava e a radiancia de saida do ponto ATINGIDO, nao a irradiancia deste
    // pixel —, entao o material primario nao entra na conta. Ela so decide QUAIS celulas recebem
    // amostra, e o cosseno as concentra onde os raios secundarios do render vao procurar.
    float3 dir;
    {
        const float2 E    = GGX_Rand2E(px, frameIndex, SMILE_RNG_CACHE_ORIGIN);
        const float  r    = sqrt(E.x);
        const float  phi  = 2.0f * SMILE_PI * E.y;
        const float3 d    = float3(r * cos(phi), r * sin(phi), sqrt(saturate(1.0f - E.x)));
        dir = normalize(mul(d, GGX_TangentBasis(N)));
    }

    const float3 sunDir = normalize(SunDirIntensity.xyz);

    FHitShadeParams P;
    P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
    P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
    P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
    P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
    P.SunColor       = SunColor.rgb;        P.ShadowRayBias = TraceParams.w;
    P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
    P.AlbedoLOD      = ShadeParams.y;
    P.NumLights      = (int)ShadeParams.x;
    P.ShadowRayMask  = (uint)SunColor.w;
    P.ReGIRGridMin       = ReGIRGridMinSlots.xyz;
    P.ReGIRSlotsPerCell  = (uint)ReGIRGridMinSlots.w;
    P.ReGIRInvCellSize   = ReGIRInvCellEnabled.xyz;
    P.ReGIREnabled       = ReGIRInvCellEnabled.w > 0.5f;
    P.ReGIRGridCount     = (int3)ReGIRGridCountSamples.xyz;
    P.ReGIRSampleCount   = (int)ReGIRGridCountSamples.w;
    P.ReGIRSlotsSRV      = (uint)ReGIRResources.x;
    P.ReGIRAverageSRV    = (uint)ReGIRResources.y;
    P.FrameIndex         = frameIndex;
    P.ReGIRPad           = 0u;
    P.SkyViewHeightKm    = SkyParams.x;
    P.SkyBottomRKm       = SkyParams.y;
    // Cache NAO-direcional, e este passe e justamente quem o alimenta: o piso de roughness do
    // secundario vale aqui com mais forca que em qualquer consumidor. Ver PT_LoadHitMaterial.
    P.RoughnessMin       = GIBiasParams.w;
    P.CacheRayRoughness  = -1.0f; // o raio de origem e difuso
    RC_UNPACK_PARAMS(P);

    // --- v0: o vertice que vai para o cache -------------------------------------------------
    RayDesc ray;
    ray.Origin    = OffsetRayGBuffer(x1, N, dir, camDist);
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;
    // SEM culling, igual ao gather do ReSTIR GI: o cache tem de aprender tambem o verso de
    // superficie fina (a chave ja separa os dois lados pelo octante da normal). E a politica de
    // backface abaixo que decide entre re-tracar e matar — deixar o DXR cullar descartaria o hit
    // antes da classificacao.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, ray);
    SMILE_RT_PROCEED(q)

    // AUTO-INTERSECCAO / BACKFACE, sob o MESMO toggle do ReSTIR GI (PolicyParams.x) — e nao um
    // knob proprio: o cache ALIMENTA o ReSTIR GI, e duas politicas de geometria diferentes fariam
    // a mesma superficie ter duas radiancias conforme o caminho.
    //
    // ⚠️ E ESSE TOGGLE NASCE DESLIGADO. Na configuracao padrao este bloco nao roda, e a exposicao
    // que ele fecha CONTINUA no pipeline. Isso e uma decisao adiada de proposito, nao um
    // esquecimento: ligar o default muda a imagem do RENDER (o toggle e um so), e o plano proibe
    // fechar isso por "parece melhor" — precisa de A/B. O que este commit entrega e a capacidade
    // de medir: a politica existe aqui, e o manifesto agora carrega `giBackfacePolicy`, entao as
    // duas capturas deixaram de ser indistinguiveis. A escolha do default sai da medida.
    //
    // O argumento que mantem o default OFF no render nao se transporta inteiro para ca. La ele e
    // "tracando sem culling, o backface ja bloqueia o raio; a terminacao preta so troca quase-preto
    // por exatamente-zero, contra um HitIsBackface por raio". Aqui o custo e ~25x menor (4% dos
    // pixels) e a consequencia dura mais: o render consome um hit ruim uma vez e o descarta, o
    // cache o GRAVA numa celula de mundo e o serve pelos proximos frames. E por isso que a medida
    // pode muito bem terminar com defaults DIFERENTES nos dois lados — o que exigiria separar o
    // toggle, e ai a separacao teria motivo medido em vez de gosto.
    //
    // Caminho morto aqui NAO grava zero — nao grava nada. Zero afirmaria "deste ponto nao sai
    // luz" e a media da celula carregaria a afirmacao; nao gravar diz "esta amostra nao descreve
    // superficie nenhuma", que e o que de fato aconteceu.
    if (PolicyParams.x > 0.5f && PT_ResolveSelfIntersection(q, ray, SMILE_RT_MASK_GATHER)) return;

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return; // ceu: nao ha celula a alimentar

    const float  hitDist = q.CommittedRayT();
    const uint   instId  = q.CommittedInstanceID();
    const FHitSurface  S = PT_LoadHitSurface(Instances[instId], q.CommittedPrimitiveIndex(),
                                             q.CommittedTriangleBarycentrics(),
                                             q.CommittedWorldToObject3x4(),
                                             ray.Origin, ray.Direction, hitDist);
    const float3 V = normalize(-ray.Direction);
    const FHitMaterial M = PT_LoadHitMaterial(Instances[instId], S.UV, P.AlbedoLOD, P.RoughnessMin);

    float3 directLighting = PT_ShadeDirectSun(S, M, V, P);
    PT_AddDirectLocal(S, M, V, P, directLighting);
    const float3 emissive = PT_LoadHitEmissive(Instances[instId], S.UV, P.AlbedoLOD);

    // --- terminal: o indireto de v0 ---------------------------------------------------------
    // Aqui esta a diferenca de POLITICA que justificou a Fase 2 inteira. O render, neste ponto,
    // chamaria PT_SampleIndirectFallback e somaria DDGI. Este passe amostra o BSDF, traca mais um
    // raio e le o cache RESOLVIDO (ou o ceu, ou zero).
    //
    // O raio do terminal e o ceu saem SEMPRE; o knob (UpdateParams.z) fecha apenas a consulta ao
    // `Resolved` no hit geometrico. Ele existe para isolar a REALIMENTACAO — "o multi-bounce vem
    // do cache anterior?" — e um knob que tambem apagasse o trace e o ceu responderia outra
    // pergunta, misturando tres mudancas numa medida so.
    float3 indirectLighting = float3(0.0f, 0.0f, 0.0f);
    {
        const float2 Edir  = GGX_Rand2E(px, frameIndex, SMILE_RNG_CACHE_BOUNCE);
        const float  Elobe = GGX_Rand2E(px, frameIndex, SMILE_RNG_CACHE_BOUNCE + 1u).x;
        const FBsdfSample B = PT_SampleBSDF(S, M, V, Edir, Elobe);
        if (B.Valid) {
            RayDesc bray;
            bray.Origin    = PT_ShadowRayOrigin(S, P); // mesma fuga do plano do triangulo
            bray.Direction = B.Dir;
            bray.TMin      = RayEpsB.x;
            bray.TMax      = TraceParams.y;
            RayQuery<RAY_FLAG_NONE> bq;
            bq.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, bray);
            SMILE_RT_PROCEED(bq)

            // Mesma politica no segundo segmento. Aqui o caminho morto vira ZERO (e nao "nao
            // grava"): o vertice que se grava e o v0, e um segundo raio que termina dentro de
            // geometria e OCLUSAO legitima de v0 — a terminacao preta do Lumen, com o retrace
            // antes dela para nao transformar auto-interseccao em mancha.
            const bool bKill = PolicyParams.x > 0.5f &&
                               PT_ResolveSelfIntersection(bq, bray, SMILE_RT_MASK_GATHER);

            float3 Lterm = float3(0.0f, 0.0f, 0.0f);
            if (bKill) {
                Lterm = float3(0.0f, 0.0f, 0.0f);
            } else if (bq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
                Lterm = ShadeSky(B.Dir, sunDir, P.SkyIntensity, P);
            } else if (UpdateParams.z > 0.5f) {
                // Chave do terminal montada com PT_LoadHitSurface, e nao com o HitGeomNormal
                // barato: a normal do cache tem de ser a MESMA que o ShadeSurfaceHit usa para
                // consultar (a interpolada com facing), senao update e query montam chaves
                // diferentes para o mesmo ponto perto de silhueta de malha suavizada.
                const float  bDist = bq.CommittedRayT();
                const FHitSurface T = PT_LoadHitSurface(
                    Instances[bq.CommittedInstanceID()], bq.CommittedPrimitiveIndex(),
                    bq.CommittedTriangleBarycentrics(), bq.CommittedWorldToObject3x4(),
                    bray.Origin, bray.Direction, bDist);
                // Miss de cache => ZERO, e nao DDGI. O caminho fica escuro enquanto a tabela
                // esfria, e e assim que se ve o cache aquecer; somar DDGI aqui devolveria
                // exatamente o sinal que esta serie existe para trocar.
                const FRCQueryResult Q = RC_QueryEx(P.Cache, T.Pos, T.CacheN, bDist, -1.0f);
                if (RC_QueryHit(Q)) Lterm = Q.Radiance;
            }
            indirectLighting = B.Throughput * Lterm;
        }
    }

    // Mesma ordem de soma do ShadeSurfaceHit (direta + indireta + emissivo). As duas politicas
    // produzem grandezas diferentes e nao ha promessa de igualdade bit a bit entre elas, mas ler
    // as duas lado a lado e o que torna a DIFERENCA obvia — e a diferenca e so o termo do meio.
    const float3 Lo = directLighting + indirectLighting + emissive;

    // Uma amostra por pixel selecionado. O clamp de faixa, o descarte de NaN/Inf e a reserva de
    // vaga no acumulador ficam no RC_Update — que e onde eles ja estavam para o produtor antigo.
    RC_Update(P.Cache, S.Pos, S.CacheN, Lo);
}
