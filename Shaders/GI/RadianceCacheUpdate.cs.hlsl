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
    float4 ShadeParams;       // x=nº de luzes puntuais, y=albedoLOD, zw=livres
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

    // ============================================================================================
    // IDA: anda o caminho, guardando o que a volta vai consumir
    // ============================================================================================
    FPathVertex verts[RCU_MAX_VERTS];
    uint  count = 0u;                        // vertices sombreados
    float3 Lterm = float3(0.0f, 0.0f, 0.0f); // radiancia que entra pelo fim do caminho

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
            Lterm = float3(0.0f, 0.0f, 0.0f);
            break;
        }

        // Raio escapou: o ceu fecha o caminho, e os vertices ja andados recebem essa energia na
        // volta. NAO e `return` — com um vertice sombreado e o ceu no fim, o caminho e valido e
        // completo; era o `return` do commit anterior que jogava fora justamente o caso mais
        // comum de iluminacao de exterior.
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            Lterm = ShadeSky(ray.Direction, sunDir, P.SkyIntensity, P);
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
                if (RC_QueryHit(Q)) Lterm = Q.Radiance;
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
        if (!B.Valid) break;

        verts[bounce].Throughput = B.Throughput;

        ray.Origin    = PT_ShadowRayOrigin(S, P); // mesma fuga do plano do triangulo
        ray.Direction = B.Dir;
        ray.TMin      = RayEpsB.x;
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
}
