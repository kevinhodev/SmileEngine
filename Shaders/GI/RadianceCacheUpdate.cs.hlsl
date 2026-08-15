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
// MULTI-BOUNCE: NO FRAME E NO TEMPO
// O caminho anda ate RCU_MAX_VERTS vertices sombreados (knob, default 4) e mais um raio para o
// TERMINAL, que e o ceu, o cache RESOLVIDO (frames anteriores) ou zero — nunca DDGI. Depois a
// radiancia volta em ordem REVERSA, e cada vertice elegivel recebe a sua:
//
//     L_terminal = ceu / cache anterior / zero
//     L_i        = local_i + throughput_i * L_(i+1)      (local = direta + emissivo)
//     RC_Update(pos_i, normal_i, L_i)                    para cada i elegivel
//
// As DUAS realimentacoes coexistem e nao se confundem. A do FRAME e este laco: quatro vertices
// resolvidos de uma vez, latencia zero. A do TEMPO e o terminal lendo `Resolved` e a escrita indo
// para `Accum` — L_novo = direta + f*L_velho, uma iteracao de ponto fixo que ja convergia para o
// transporte completo com um vertice so. O laco nao cria o efeito; ele encurta a latencia e
// reduz o vies do truncamento, que e o que a SHaRC obtem com os quatro bounces do update.
//
// Com UpdateMaxVertices = 1 o passe reproduz exatamente o comportamento do commit anterior — e e
// esse o A/B que separa "o multi-bounce do frame vale o custo" de "o do tempo ja bastava".
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
    // z = grupos X do dispatch compacto. Viaja no CB porque Dispatch pode precisar de Y quando
    // a fracao/resolucao faz o total passar do limite D3D12 de 65.535 grupos por dimensao; o
    // shader lineariza (dtid.x, dtid.y) com o MESMO pitch que o CPU usou.
    float4 ShadeParams;       // x=nº de luzes puntuais, y=albedoLOD, z=dispatchGroupsX, w=livre
    // x = politica de auto-interseccao/backface (0/1). O MESMO toggle do ReSTIR GI, empurrado
    // pelo Renderer — ver o bloco no v0 sobre por que ele nao e knob proprio.
    float4 PolicyParams;
    // x = celulas do tile 5x5 sorteadas por frame (1 = 4% dos pixels)
    // y = vertices SOMBREADOS do caminho (1..RCU_MAX_VERTS); o custo em raios e y + 1
    // z = consultar o cache resolvido no terminal (0/1)
    // w = piso de roughness do lobo QUE CHEGA para o vertice ser gravavel
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

#ifndef RCU_COMPACT_DISPATCH
    // Hot reload/manual DXC continua produzindo o caminho de producao quando o chamador nao
    // declara a permutacao. O CMake compila explicitamente 1 (compacto) e 0 (controle legado).
    #define RCU_COMPACT_DISPATCH 1
#endif

// SELECAO ESPARSA — ~4% dos pixels por frame, sem buracos, ja COMPACTADA em waves cheias.
//
// Permutacao de periodo 25 sobre o tile 5x5, e nao um sorteio independente por pixel: com sorteio,
// a fracao e respeitada NA MEDIA mas um pixel qualquer pode passar dezenas de frames sem nunca ser
// escolhido, e as celulas atras dele envelhecem ate o despejo. Aqui cada posicao do tile dispara
// exatamente uma vez a cada 25 frames.
//
// O `* 13` e a permutacao: 13 e coprimo com 25, entao tile -> frame e uma bijecao, e frames
// consecutivos caem em posicoes 2 apart no tile (13*2 = 26 = 1 mod 25) em vez de varrerem o tile
// em ordem de raster — o padrao do frame nao "escorrega" pela tela.
//
// Ate a Fase 6 o dispatch era de TELA CHEIA e cada thread fazia o teste acima. Com uma celula por
// tile, isso deixava ~4% das lanes vivas no ponto em que comeca o RayQuery — uma wave inteira era
// ocupada para dois ou tres caminhos. A Fase 7 inverte a bijecao e despacha diretamente
// `tiles*cellsPerFrame` work items. Nao ha lista, contador atomico nem passe de compactacao porque
// a lista ja e uma funcao fechada de (workItem, frame): `2` e o inverso de `13` modulo `25`.
//
// PROVA DA EQUIVALENCIA. Para rank em [0, cellsPerFrame), escolhemos
//
//   slot  = (rank - frame) mod 25
//   local = slot * 13^-1 mod 25 = slot * 2 mod 25
//
// Logo `(local*13 + frame) mod 25 == rank`, exatamente o predicado antigo. Cada rank produz um
// local diferente porque as duas multiplicacoes sao bijecoes; portanto nao ha pixel repetido nem
// perdido. Tiles parciais da borda ainda saem da funcao e sao rejeitados pelo bounds check — o
// mesmo conjunto que o dispatch de tela cheia alcancava.
#if RCU_COMPACT_DISPATCH
uint2 RCU_WorkItemPixel(uint workItem, uint2 tileCount, uint frame, uint cellsPerFrame) {
    const uint tileIndex = workItem / cellsPerFrame;
    const uint rank      = workItem - tileIndex * cellsPerFrame;
    const uint2 tile     = uint2(tileIndex % tileCount.x, tileIndex / tileCount.x);

    const uint slot  = (rank + 25u - (frame % 25u)) % 25u;
    const uint local = (slot * 2u) % 25u;
    return tile * 5u + uint2(local % 5u, local / 5u);
}
#else
// CONTROLE DA FASE 7: o predicado exato que existia antes da compactacao. Mantido como uma
// permutacao separada do shader para o A/B nao pagar branch por lane nem comparar dois binarios.
bool RCU_PixelSelected(uint2 px, uint frame, uint cellsPerFrame) {
    const uint local = (px.y % 5u) * 5u + (px.x % 5u);
    const uint slot  = (local * 13u) % 25u;
    return ((slot + frame) % 25u) < cellsPerFrame;
}
#endif

// Teto de vertices do caminho. CONSTANTE DE COMPILACAO, e nao so o teto do knob: os dois lacos
// abaixo sao [unroll] justamente para que `verts[i]` fique com indice literal e o array viva em
// REGISTRADOR. Com indice dinamico o DXC o joga em scratch, e um caminho de 4 vertices lendo e
// escrevendo memoria por iteracao custaria mais que os proprios raios.
#define RCU_MAX_VERTS 4

// O que se guarda de cada vertice ate a volta. Exatamente o que a backpropagation consome — e
// nada alem: `Pos`/`CacheN` para a chave, `Local` (direta + emissivo) e `Throughput` para a
// recorrencia, e `Eligible` porque um vertice alcancado por lobo estreito nao pode ser gravado.
//
// 12 floats + 1 bit por vertice, 4 vertices. A alternativa da SHaRC guarda o INDICE da celula
// (inserido na ida) em vez de posicao e normal, o que economiza 5 registradores por vertice ao
// preco de reservar entrada para caminho que talvez nao seja gravado. Fica anotada: o plano manda
// medir a pressao de registradores antes de otimizar, e nao supor.
struct FPathVertex {
    float3 Pos;
    float3 CacheN;
    float3 Local;
    float3 Throughput;
    bool   Eligible;
};

// Por onde o caminho terminou. A ordem espelha RC_STAT_TERM_* (o endereçamento e
// `RC_STAT_TERM_SKY + kind`), e o valor so e lido pela telemetria — nenhuma decisao de transporte
// depende dele.
//
// Os quatro do fim eram um `ZERO` so, e ele juntava causas que pedem acoes OPOSTAS: MISS e cache
// frio (passa com o tempo), NOQUERY e o operador tendo desligado o terminal, LOBE e material, e
// OTHER e o teto de vertices. Todos entregam a mesma radiancia — zero pela frente — e e por isso
// que um contador unico parecia suficiente; mas "o numero subiu" nao dizia qual dos quatro.
#define RCU_TERM_SKY     0u
#define RCU_TERM_CACHE   1u
#define RCU_TERM_KILLED  2u
#define RCU_TERM_MISS    3u
#define RCU_TERM_NOQUERY 4u
#define RCU_TERM_LOBE    5u
#define RCU_TERM_OTHER   6u
#define RCU_TERM_COUNT   7u

#if RCU_COMPACT_DISPATCH
[numthreads(64, 1, 1)]
#else
[numthreads(8, 8, 1)]
#endif
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint frameIndex = (uint)TraceParams.x;
    const uint cellsPerFrame = clamp((uint)UpdateParams.x, 1u, 25u);
    const uint2 screenSize   = (uint2)ScreenParams.xy;

#if RCU_COMPACT_DISPATCH
    const uint2 tileCount    = (screenSize + 4u) / 5u;
    const uint  workItemCount = tileCount.x * tileCount.y * cellsPerFrame;
    const uint  dispatchGroupsX = max(1u, (uint)ShadeParams.z);
    const uint  workItem = dtid.x + dtid.y * dispatchGroupsX * 64u;
    if (workItem >= workItemCount) return;

    const uint2 px = RCU_WorkItemPixel(workItem, tileCount, frameIndex, cellsPerFrame);
    if (px.x >= screenSize.x || px.y >= screenSize.y) return;
#else
    const uint2 px = dtid.xy;
    if (px.x >= screenSize.x || px.y >= screenSize.y) return;
    if (!RCU_PixelSelected(px, frameIndex, cellsPerFrame)) return;
#endif

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

    // ============================================================================================
    // IDA: anda o caminho, guardando o que a volta vai consumir
    // ============================================================================================
    FPathVertex verts[RCU_MAX_VERTS];
    uint  count = 0u;                        // vertices sombreados
    float3 Lterm = float3(0.0f, 0.0f, 0.0f); // radiancia que entra pelo fim do caminho
    // OTHER e o residual — o caminho que sai do laco pelo teto de vertices sem passar pelo bloco
    // do terminal. Todas as outras saidas se declaram no ponto em que acontecem.
    uint  termKind = RCU_TERM_OTHER;

    RayDesc ray;
    ray.Origin    = OffsetRayGBuffer(x1, N, dir, camDist);
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = TraceParams.y;
    // Roughness do lobo que GEROU o raio corrente. O de origem e cosseno-hemisferico: difuso.
    // E ela — nao a do material atingido — que decide se o proximo vertice e gravavel.
    float rayRoughness = -1.0f;

    const uint maxVerts = min((uint)UpdateParams.y, (uint)RCU_MAX_VERTS);

    // maxVerts vertices + 1 raio de terminal. O [unroll] e o que mantem `verts` em registrador
    // (ver RCU_MAX_VERTS); os `break` continuam valendo depois de desenrolado.
    [unroll] for (uint bounce = 0u; bounce <= (uint)RCU_MAX_VERTS; ++bounce) {
        if (bounce > maxVerts) break; // teto em tempo de execucao, dentro do teto de compilacao

        // SEM culling, igual ao gather do ReSTIR GI: o cache tem de aprender tambem o verso de
        // superficie fina (a chave ja separa os dois lados pelo octante da normal). E a politica
        // de backface abaixo que decide entre re-tracar e matar — deixar o DXR cullar descartaria
        // o hit antes da classificacao.
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, SMILE_RT_MASK_GATHER, ray);
        SMILE_RT_PROCEED(q)

        // AUTO-INTERSECCAO / BACKFACE, sob o MESMO toggle do ReSTIR GI (PolicyParams.x) — e nao
        // um knob proprio: o cache ALIMENTA o ReSTIR GI, e duas politicas de geometria diferentes
        // fariam a mesma superficie ter duas radiancias conforme o caminho.
        //
        // ⚠️ E ESSE TOGGLE NASCE DESLIGADO. Na configuracao padrao este bloco nao roda, e a
        // exposicao que ele fecha CONTINUA no pipeline. Isso e uma decisao adiada de proposito,
        // nao um esquecimento: ligar o default muda a imagem do RENDER (o toggle e um so), e o
        // plano proibe fechar isso por "parece melhor" — precisa de A/B. O que ja esta entregue e
        // a capacidade de medir: a politica existe aqui e o manifesto carrega `giBackfacePolicy`,
        // entao as duas capturas deixaram de ser indistinguiveis. O default sai da medida.
        //
        // O argumento que mantem o default OFF no render nao se transporta inteiro para ca. La ele
        // e "tracando sem culling, o backface ja bloqueia o raio; a terminacao preta so troca
        // quase-preto por exatamente-zero, contra um HitIsBackface por raio". Aqui o custo e ~25x
        // menor (4% dos pixels) e a consequencia dura mais: o render consome um hit ruim uma vez e
        // o descarta, o cache o GRAVA numa celula de mundo e o serve pelos proximos frames. E por
        // isso que a medida pode terminar com defaults DIFERENTES nos dois lados — e ai a
        // separacao do toggle teria motivo medido em vez de gosto.
        //
        // TERMINACAO PRETA, e nao descarte do caminho: o segmento morto significa "dali para a
        // frente nao vem luz", que e uma afirmacao sobre o SEGMENTO. Os vertices ja andados
        // continuam validos e recebem zero pela frente — matar o caminho inteiro jogaria fora a
        // direta e o emissivo que eles ja mediram. Em `bounce == 0` isto e equivalente a desistir,
        // porque `count` fica em zero e a volta nao grava nada.
        if (PolicyParams.x > 0.5f && PT_ResolveSelfIntersection(q, ray, SMILE_RT_MASK_GATHER)) {
            Lterm    = float3(0.0f, 0.0f, 0.0f);
            termKind = RCU_TERM_KILLED;
            break;
        }

        // Raio escapou: o ceu fecha o caminho, e os vertices ja andados recebem essa energia na
        // volta. NAO e `return` — com um vertice sombreado e o ceu no fim, o caminho e valido e
        // completo; era o `return` do commit anterior que jogava fora justamente o caso mais
        // comum de iluminacao de exterior.
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            Lterm    = ShadeSky(ray.Direction, sunDir, P.SkyIntensity, P);
            termKind = RCU_TERM_SKY;
            break;
        }

        const float hitDist = q.CommittedRayT();

        // Ja ha vertices sombreados de sobra: ESTE hit e o TERMINAL, e nao mais um vertice. Ele
        // nao paga material nem luz — so a consulta ao cache RESOLVIDO, que e o que resume todo o
        // transporte dali para a frente.
        //
        // O raio do terminal e o ceu acima saem SEMPRE; o knob (UpdateParams.z) fecha apenas esta
        // consulta. Ele existe para isolar a REALIMENTACAO — "o multi-bounce vem do cache
        // anterior?" — e um knob que tambem apagasse o trace responderia outra pergunta.
        // Este hit e o TERMINAL quando ja ha vertices de sobra — ou quando o indice do array
        // acabou. As duas condicoes dizem a mesma coisa (maxVerts nunca passa de RCU_MAX_VERTS, e
        // `count == bounce` aqui), e a segunda esta escrita porque a PROVA do limite nao pode
        // depender do otimizador: em Debug (-Od) nao ha unroll, o indice de `verts[bounce]` fica
        // dinamico e o DXC recusa o acesso como fora de faixa. Release compilava; Debug nao.
        if (count >= maxVerts || bounce >= (uint)RCU_MAX_VERTS) {
            termKind = RCU_TERM_NOQUERY; // sobrescrito abaixo se a consulta de fato acontecer
            if (UpdateParams.z > 0.5f) {
                // Chave do terminal montada com PT_LoadHitSurface, e nao com o HitGeomNormal
                // barato: a normal do cache tem de ser a MESMA que o ShadeSurfaceHit usa para
                // consultar (a interpolada com facing), senao update e query montam chaves
                // diferentes para o mesmo ponto perto de silhueta de malha suavizada.
                const FHitSurface T = PT_LoadHitSurface(
                    Instances[q.CommittedInstanceID()], q.CommittedPrimitiveIndex(),
                    q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                    ray.Origin, ray.Direction, hitDist);
                // Miss de cache => ZERO, e nao DDGI. O caminho fica escuro enquanto a tabela
                // esfria, e e assim que se ve o cache aquecer; somar DDGI aqui devolveria
                // exatamente o sinal que esta serie existe para trocar.
                const FRCQueryResult Q = RC_QueryEx(P.Cache, T.Pos, T.CacheN, hitDist,
                                                    rayRoughness);
                if (RC_QueryHit(Q)) { Lterm = Q.Radiance; termKind = RCU_TERM_CACHE; }
                else                { termKind = RCU_TERM_MISS; }
            }
            break;
        }

        // --- vertice sombreado ---------------------------------------------------------------
        const uint instId = q.CommittedInstanceID();
        const FHitSurface S = PT_LoadHitSurface(Instances[instId], q.CommittedPrimitiveIndex(),
                                                q.CommittedTriangleBarycentrics(),
                                                q.CommittedWorldToObject3x4(),
                                                ray.Origin, ray.Direction, hitDist);
        const float3 V = normalize(-ray.Direction);
        const FHitMaterial M = PT_LoadHitMaterial(Instances[instId], S.UV, P.AlbedoLOD,
                                                  P.RoughnessMin);

        float3 local = PT_ShadeDirectSun(S, M, V, P);
        PT_AddDirectLocal(S, M, V, P, local);
        local += PT_LoadHitEmissive(Instances[instId], S.UV, P.AlbedoLOD);

        // ELEGIBILIDADE: o vertice so entra na tabela se o lobo que CHEGOU nele for largo o
        // bastante. O cache guarda um RGB por celula e o serve a qualquer direcao; radiancia de
        // saida vista por um lobo estreito so vale para aquela direcao, e grava-la mentiria para
        // todos os que consultarem a celula depois.
        //
        // Duas guardas, e as duas usam a MESMA funcao que a query usa (RC_ConeCoversCell):
        // roughness acima do piso, e cone cobrindo a celula. O gate de SEGMENTO CURTO da query
        // NAO se aplica aqui — la ele evita auto-referencia (origem e hit na mesma celula), e o
        // produtor nao le nada, so escreve o que aquele ponto emite.
        const float cellSize = RC_CellSizeAt(P.Cache, S.Pos);
        const bool  eligible = (rayRoughness < 0.0f) ||
                               (rayRoughness >= UpdateParams.w &&
                                RC_ConeCoversCell(hitDist, rayRoughness, cellSize));

        verts[bounce].Pos        = S.Pos;
        verts[bounce].CacheN     = S.CacheN;
        verts[bounce].Local      = local;
        verts[bounce].Throughput = float3(0.0f, 0.0f, 0.0f); // preenchido pela amostragem abaixo
        verts[bounce].Eligible   = eligible;
        count = bounce + 1u;

        // --- proximo segmento ----------------------------------------------------------------
        // Streams por PROFUNDIDADE: sem isso os quatro vertices sorteariam a mesma direcao no
        // mesmo pixel, e o caminho andaria em ziguezague correlacionado em vez de amostrar.
        const float2 Edir  = GGX_Rand2E(px, frameIndex, SMILE_RNG_CACHE_BOUNCE + bounce * 2u);
        const float  Elobe = GGX_Rand2E(px, frameIndex, SMILE_RNG_CACHE_BOUNCE + bounce * 2u + 1u).x;
        const FBsdfSample B = PT_SampleBSDF(S, M, V, Edir, Elobe);
        // Sem direcao valida o caminho para aqui com terminal ZERO, e os vertices ja andados
        // continuam validos — eles so perdem o que viria da frente.
        if (!B.Valid) { termKind = RCU_TERM_LOBE; break; }

        verts[bounce].Throughput = B.Throughput;

        // ORIGEM DO SEGMENTO: offset ULP (Wachter-Binder), NAO o do shadow ray.
        //
        // Isto era PT_ShadowRayOrigin, e estava errado: aquele aplica o ShadowRayBias, que vale
        // 0,20 m por default. Num raio de TRANSPORTE isso desloca o vertice em 20 cm antes de
        // tracar — atravessa parede fina (e o vazamento vai parar numa celula do cache, servido
        // por dezenas de frames) e move a origem do segmento para longe do ponto cuja radiancia se
        // esta medindo. A propria base ja tinha tomado esta decisao duas vezes, e por escrito: o
        // ReSTIRGITrace ("o normal-bias de 0.2 aqui contaminava a medida") e os dois traces de
        // reflexao ("o bias 0.2 na origem deslocava o reflexo de contato e inflava o hitT") usam
        // offset so-numerico. O shadow ray continua com o bias dele — la o 0,20 e anti-acne
        // calibrado, e nao ha segmento cuja geometria dependa da origem.
        //
        // OffsetRayWB e nao OffsetRayGBuffer: aqui a posicao vem de um hit EXATO, e nao da
        // reconstrucao do G-buffer (depth + normal quantizada), cujo erro maior e o que justifica
        // os termos calibrados daquele.
        //
        // Normal orientada para a direcao de SAIDA: a OffsetN aponta contra o raio que CHEGOU, e
        // no caso comum isso coincide com o hemisferio da amostra. Quando a interpolada e a de
        // face discordam (silhueta de malha suavizada), o lobo pode sair do outro lado — e ai um
        // offset fixo empurraria a origem ATRAVES da superficie, que e o mesmo modo de falha que
        // se esta fechando.
        const float3 offN = (dot(S.OffsetN, B.Dir) >= 0.0f) ? S.OffsetN : -S.OffsetN;
        ray.Origin    = OffsetRayWB(S.Pos, offN);
        ray.Direction = B.Dir;
        // TMin ZERO, como o raio de origem acima e como os tres raios de transporte da engine
        // (ReSTIRGITrace, ReflectionTrace, ReflectionTraceMirror). Era RayEpsB.x — o TMin dos
        // SHADOW rays, 1 cm —, e o proprio RayEpsilons.h avisa contra isto na definicao dele:
        // "soma com o bias de origem: em 1 cm, oclusor a menos de 1 cm do hit e invisivel MESMO
        // DEPOIS DE O BIAS CAIR". Era exatamente o estado que sobrou ao remover os 20 cm: contato
        // dentro de 1 cm deixava de ocluir, e o cache aprenderia "sem oclusor por perto" em canto
        // apertado — do lado errado, porque erro assim CLAREIA.
        //
        // A REGRA, ja que este e o quarto sitio em que ela se aplica: este passe traca DUAS
        // familias de raio a partir do mesmo FHitShadeParams. Shadow ray usa os epsilons de sombra
        // (bias 0,20 + TMin 1 cm, calibrados como anti-acne). Raio de TRANSPORTE nao usa nenhum
        // dos dois — ele confia no offset ULP da origem e vai com TMin 0, porque qualquer corte no
        // inicio do intervalo apaga geometria real. Segmento novo que nasca aqui herda esta linha,
        // nao a de cima.
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;
        // A roughness que viaja e a do MATERIAL quando o lobo foi especular, e difusa quando nao:
        // e ela que o proximo vertice consulta para saber se pode ser gravado.
        rayRoughness  = (B.Lobe == PT_LOBE_SPECULAR) ? M.Roughness : -1.0f;
    }

    // ============================================================================================
    // VOLTA: backpropagation em ordem reversa
    // ============================================================================================
    // L_i = local_i + throughput_i * L_(i+1), com L_(count) = Lterm. Cada vertice elegivel recebe
    // a SUA radiancia de saida — e nao a do caminho inteiro —, que e o que a celula tem de guardar.
    //
    // O [unroll] com guarda por `count` mantem o indice literal (ver RCU_MAX_VERTS). Ele desenrola
    // a partir do teto de COMPILACAO, entao um caminho curto so pula iteracoes.
    float3 L = Lterm;
    [unroll] for (int i = RCU_MAX_VERTS - 1; i >= 0; --i) {
        if ((uint)i >= count) continue;
        L = verts[i].Local + verts[i].Throughput * L;
        // O clamp de faixa, o descarte de NaN/Inf e a reserva de vaga no acumulador ficam no
        // RC_Update — onde ja estavam para o produtor antigo.
        if (verts[i].Eligible) RC_Update(P.Cache, verts[i].Pos, verts[i].CacheN, L);
    }

    // ============================================================================================
    // TELEMETRIA DO PRODUTOR (regime de detalhe)
    // ============================================================================================
    // Chega aqui so quem LANCOU caminho: os pixels nao sorteados e os de ceu ja retornaram la em
    // cima, e e isso que faz `PATHS` medir a fracao efetiva em vez do dispatch.
    //
    // As waves aqui sao esparsas por construcao (4% dos pixels), entao a reducao por wave rende
    // menos que nos traces — mas o dispatch tambem e ~25x menor, e sem estes numeros o gate da
    // Fase 3 que exige "a taxa de paths e a profundidade media batem com os parametros" nao tem
    // como ser verificado.
    [branch] if ((P.Cache.Flags & RC_FLAG_STATS_DETAIL) != 0u) {
        RWStructuredBuffer<uint> stats =
            ResourceDescriptorHeap[NonUniformResourceIndex(P.Cache.StatsUAV)];
        const uint wavePaths = WaveActiveCountBits(true);
        const uint waveVerts = WaveActiveSum(count);
        const uint waveDepth = WaveActiveMax(count);
        uint ignored;
        if (WaveIsFirstLane()) {
            InterlockedAdd(stats[RC_STAT_PATHS],      wavePaths, ignored);
            InterlockedAdd(stats[RC_STAT_PATH_VERTS], waveVerts, ignored);
            InterlockedMax(stats[RC_STAT_PATH_DEPTH], waveDepth, ignored);
        }
        // Um atomico por tipo PRESENTE na wave — e nao sete sempre. O laco desenrola sobre
        // constantes, entao o indice do buffer continua literal.
        [unroll] for (uint t = 0u; t < RCU_TERM_COUNT; ++t) {
            const uint n = WaveActiveCountBits(termKind == t);
            if (n > 0u && WaveIsFirstLane()) {
                InterlockedAdd(stats[RC_STAT_TERM_SKY + t], n, ignored);
            }
        }
    }
}
