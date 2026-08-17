#ifndef SMILE_GI_PATHTRACINGCOMMON_HLSLI
#define SMILE_GI_PATHTRACINGCOMMON_HLSLI

// ================================================================================================
// OS BLOCOS DO SHADING DE HIT, separados da POLITICA que os compoe.
//
// Tudo aqui saiu de dentro do `ShadeSurfaceHit` (HitShading.hlsli), que era uma funcao unica de
// ~290 linhas fazendo geometria, material, direta, fallback indireto e cache. Ela continua
// existindo, com a mesma assinatura e a mesma sequencia — virou a composicao destes blocos.
//
// POR QUE SEPARAR, ja que o resultado e identico: o path tracer esparso da Fase 3 precisa do MESMO
// material, do MESMO emissivo, do MESMO shadow bias e da MESMA luz local — mas com outra politica
// de terminacao (ele ALIMENTA o cache em vez de consulta-lo, e nao pode usar DDGI como fonte
// normal). Com um bloco unico, a unica saida seria copiar as 290 linhas e deixar as duas copias
// divergirem no primeiro fix de material. O plano e explicito: "Nao duplicar BRDF entre o path
// updater e ReSTIR GI".
//
// CONTRATO DE BINDINGS: identico ao do HitShading.hlsli, que e quem inclui este arquivo depois de
// declarar tudo (Scene, Instances, SceneLights, atlas do DDGI, samplers, e os campos de cbuffer
// RayEpsA/RayEpsB/GIDistParams/GIBiasParams/GICascade*). Este header nao declara recurso nenhum.
//
// REGRA DE EDICAO: a ordem das operacoes aqui e contrato, nao estilo. Ver a nota do
// PT_AddDirectLocal sobre por que ele acumula por `inout` em vez de devolver o proprio termo.
// ================================================================================================

// GGX_TangentBasis / GGX_SampleVNDF / GGX_VndfPdf, para o PT_SampleBSDF do fim do arquivo. Header
// so de funcoes — sem recurso, sem cbuffer —, entao entrar na cadeia dos cinco traces que incluem
// o HitShading nao acopla nada nem custa DXIL: funcao nao chamada e eliminada. Dois deles
// (ReSTIRGITrace, ReflectionTrace) ja o incluiam por conta propria; a guarda de include cuida.
#include "../Reflections/GGXSample.hlsli"

// Estado do CAMINHO que chegou ao hit — o que se sabe do raio, nao da superficie.
//
// Origin/Dir/SegmentLength/RayRoughness sao o que o render usa. Depth, Mode, Throughput, Pdf e
// Lobe existem para o LACO DE BOUNCES do updater da Fase 3, que e quem os produz. Os tres ultimos
// nasceram junto com o PT_SampleBSDF no fim deste arquivo, e nao antes, de proposito: campo sem
// produtor nasce com valor inventado e o primeiro consumidor herda a invencao sem saber.
struct FPathState {
    float3 Origin;        // origem do segmento que chegou a este hit
    float3 Dir;           // direcao dele, normalizada
    float  SegmentLength; // comprimento do segmento — gate de auto-referencia do cache
    // Roughness do lobo que GEROU o raio, nao a do material atingido. Negativa = transporte
    // difuso; positiva entra no gate de cone do cache.
    float  RayRoughness;
    uint   Depth;         // 0 = primeiro hit secundario
    uint   Mode;          // PT_MODE_*
    // Produto dos f*cos/pdf de todos os vertices ANTERIORES a este. Vale 1 no primeiro hit — nao
    // porque "ainda nao houve espalhamento", mas porque o updater usa o vertice do G-buffer so
    // como ORIGEM: o que ele grava no cache e a radiancia de SAIDA de cada vertice, e essa conta
    // comeca no proprio vertice. Ver o cabecalho do RadianceCacheUpdate.cs.hlsl.
    float3 Throughput;
    float  Pdf;           // densidade (angulo solido) da MISTURA que gerou Dir; 0 = nao amostrado
    uint   Lobe;          // PT_LOBE_*: quem gerou Dir
};

#define PT_MODE_RENDER       0u // trace de render: CONSULTA o cache
#define PT_MODE_CACHE_UPDATE 1u // Fase 3: o passe dedicado que o ALIMENTA

#define PT_LOBE_NONE     0u // direcao nao veio de amostragem de BSDF (raio de origem, revalidacao)
#define PT_LOBE_DIFFUSE  1u
#define PT_LOBE_SPECULAR 2u

FPathState PT_MakePathState(float3 rayOrigin, float3 rayDir, float hitDist, float rayRoughness) {
    FPathState S;
    S.Origin        = rayOrigin;
    S.Dir           = rayDir;
    S.SegmentLength = hitDist;
    S.RayRoughness  = rayRoughness;
    S.Depth         = 0u;
    S.Mode          = PT_MODE_RENDER;
    S.Throughput    = float3(1.0f, 1.0f, 1.0f);
    S.Pdf           = 0.0f;
    S.Lobe          = PT_LOBE_NONE;
    return S;
}

// Geometria resolvida no ponto de hit. Quatro normais, e a diferenca entre elas e o assunto do
// bloco de FACING abaixo — juntar duas delas foi um bug real.
//
// A direcao de VISADA nao esta aqui, e a ausencia e deliberada por dois motivos que se somam.
// Conceitual: `V` e propriedade do CAMINHO que chegou (esta em FPathState.Dir), nao da superficie —
// a mesma superficie vista por outro raio tem outro V. Pratico: o `normalize(-rayDir)` que a produz
// so e necessario DEPOIS do miss do cache, e calcula-la aqui a poria antes do retorno antecipado,
// no unico ponto do arquivo que promete "nada caro aconteceu ainda". Funcionaria — o DXC afunda a
// conta sozinho — mas passaria a depender do otimizador para uma garantia que o codigo deve dar.
struct FHitSurface {
    float3 Pos;
    float3 GeomN;    // interpolada dos vertices, SEM facing aplicado
    float3 ShadingN; // a de cima com facing: manda na BRDF, no N.L e no sample do DDGI
    float3 CacheN;   // chave do cache — propriedade da SUPERFICIE, nunca do raio
    float3 OffsetN;  // normal de FACE, so p/ a ORIGEM dos shadow rays
    float2 UV;
    float  SignedDist;    // negativa = raio dentro de geometria solida (ver o bloco de facing)
    bool   HitFromBehind;
};

// Material avaliado no ponto. Emissivo fica FORA de proposito: ele nao participa da BRDF, e o
// updater da Fase 3 o soma num vertice do caminho em que o resto do material nem e avaliado.
struct FHitMaterial {
    float3 DiffuseColor;
    float3 SpecularColor;
    float  Roughness;
    float  Alpha; // roughness^2
    float  A2;    // alpha^2
};

// --------------------------------------------------------------------------------------------
// Geometria e facing
// --------------------------------------------------------------------------------------------
FHitSurface PT_LoadHitSurface(InstanceGeo geo, uint tri, float2 bary, float3x4 worldToObject,
                              float3 rayOrigin, float3 rayDir, float hitDist) {
    FHitSurface S;
    S.Pos = rayOrigin + rayDir * hitDist;

    // UM registro pre-cozido, indexado direto pelo PrimitiveIndex — sem passar pelo IB e sem as
    // tres leituras dependentes no VB. Ele traz a normal de face, as 3 normais de vertice e as 3
    // UVs, que e tudo o que esta funcao lia da geometria.
    //
    // ⚠️ Esta leitura acontece em 100% dos hits, ANTES do retorno antecipado do cache — a chave do
    // hash precisa da normal. Por isso ela e o unico custo de decodificacao que os ~78% de acerto
    // NAO evitam, e por isso o payload comeca por ela.
    const RTTriangle T = RT_LoadTriangle(geo, tri);

    float3 nObj  = RT_InterpolatedNormalObj(T, bary);
    float3 nWrld = mul(nObj, (float3x3)worldToObject);
    float  nLen  = length(nWrld);
    S.GeomN = (nLen > 1e-5f) ? (nWrld / nLen) : normalize(-rayDir);

    // FACING — duas perguntas DIFERENTES que antes compartilhavam a mesma variavel `backface`:
    //
    //  (1) "o raio veio pelo verso desta face?" — pergunta puramente geometrica, responde se a
    //      normal de SHADING precisa ser virada. Vale para QUALQUER material: uma folha ou uma
    //      cortina atingida por tras tem que iluminar pelo lado de tras. O raster ja faz isso sem
    //      gate nenhum (GBuffer.ps.hlsl: `if (!input.frontFace) GeoN = -GeoN;`) e o Lumen tambem
    //      (LumenHardwareRayTracingCommon.ush: vira a normal quando IsTwoSided && !IsFrontFace).
    //      Com o gate `TwoSided == 0` que existia aqui, folhagem/cortina atingida por tras ficava
    //      com a normal apontando p/ LONGE do raio: N.L errado, Lo errado, e o sample do DDGI
    //      lido do lado errado da superficie.
    //
    //  (2) "este hit significa que o raio esta DENTRO de geometria solida?" — pergunta de
    //      topologia, e so ela justifica o gate por TwoSided. Alimenta o SignedDist, que o
    //      DDGITrace usa p/ encurtar a distancia (0.2x) e deixar o Chebyshev escurecer probes
    //      enterradas. Bater no verso de uma FOLHA nao quer dizer estar dentro de nada — por isso
    //      material two-sided continua reportando distancia POSITIVA. O gate fica aqui.
    //
    // O teste sai da normal de FACE (nao da interpolada): em malha suavizada a interpolada erra o
    // sinal perto da silhueta, e era essa impressao que o gate mascarava.
    float3 faceN;
    const bool faceOk        = HitFaceNormal(T, worldToObject, faceN);
    const bool hitFromBehind = faceOk ? (dot(faceN,  rayDir) > 0.0f)
                                      : (dot(S.GeomN, rayDir) > 0.0f);
    S.HitFromBehind = hitFromBehind;
    S.SignedDist    = (geo.TwoSidedRT == 0 && hitFromBehind) ? -hitDist : hitDist;

    // Normal de FACE, so p/ a ORIGEM dos shadow rays. A interpolada continua mandando na BRDF, no
    // N.L e no sample do DDGI — o offset e um problema de escapar do PLANO do triangulo, e quem
    // descreve esse plano e a face; a normal de vertice/normal map descreve a aparencia. Era a
    // interpolada que deslocava a origem, entao em malha suavizada sobrava componente tangencial e
    // o bias precisava ser grande p/ compensar.
    //
    // Orientada CONTRA o raio incidente, igual ao HitGeomNormal (e nao "p/ o lado da saida"):
    // como o shadow ray so e tracado quando N.L > 0, a luz esta do mesmo lado de onde o raio veio.
    // Nos casos raros em que a interpolada e a face discordam (luz abaixo do horizonte geometrico
    // mas acima do de shading), virar a face p/ a luz empurraria a origem ATRAVES da superficie —
    // o remedio ali e bias modulado por angulo, que entra no sweep da rodada 3.
    //
    // Reusa o facing (1) ja calculado — a face e a orientacao sao as mesmas do teste acima.
    S.OffsetN = faceOk ? (hitFromBehind ? -faceN    : faceN)
                       : (hitFromBehind ? -S.GeomN  : S.GeomN); // degenerado: interpolada

    S.UV = RT_InterpolatedUV(T, bary);

    // Facing (1): sem gate, igual ao raster. A do cache e a MESMA — a chave tem de ser uma
    // propriedade da superficie, e com a direcao do raio na chave cada raio cairia numa celula
    // diferente e o cache nunca acertaria. Sao dois campos porque significam coisas diferentes,
    // ainda que hoje coincidam: o dia em que o shading usar normal map, a do cache nao pode segui-lo.
    S.ShadingN = hitFromBehind ? -S.GeomN : S.GeomN;
    S.CacheN   = S.ShadingN;
    return S;
}

// Direcao de visada do hit. Uma funcao de uma linha para o call site poder ficar DEPOIS do miss do
// cache — ver a nota na FHitSurface sobre por que ela nao e campo da superficie.
float3 PT_ViewDir(FPathState Path) { return normalize(-Path.Dir); }

// --------------------------------------------------------------------------------------------
// Material
// --------------------------------------------------------------------------------------------
FHitMaterial PT_LoadHitMaterial(InstanceGeo geo, float2 uv, float albedoLOD, float roughnessMin) {
    float3 albedo = geo.BaseColor.rgb;
    if (geo.HasAlbedo != 0) {
        Texture2D<float4> albedoTex = ResourceDescriptorHeap[geo.AlbedoIndex];
        albedo *= albedoTex.SampleLevel(LinearWrap, uv, albedoLOD).rgb;
    }

    // Metallic/roughness seguem o mesmo workflow do G-buffer. O MR e amostrado uma vez para os
    // dois parametros; mapas separados ocupam os slots +6/+7.
    float metallic  = geo.EmissiveFactor.w; // MetallicFactor cabe no .w do snapshot
    float roughness = geo.RoughnessFactor;
    if ((geo.Flags & INSTGEO_FLAG_MRMAP) != 0u) {
        Texture2D<float4> mrTex = ResourceDescriptorHeap[geo.MrMapIndex];
        const float4 mr = mrTex.SampleLevel(LinearWrap, uv, albedoLOD);
        metallic  *= ((geo.Flags & INSTGEO_FLAG_SPECPACK) != 0u) ? mr.b : mr.r;
        roughness *= mr.g;
    }
    if ((geo.Flags & INSTGEO_FLAG_METALMAP) != 0u) {
        Texture2D<float4> metalTex = ResourceDescriptorHeap[geo.MetalMapIndex];
        metallic *= metalTex.SampleLevel(LinearWrap, uv, albedoLOD).r;
    }
    if ((geo.Flags & INSTGEO_FLAG_ROUGHMAP) != 0u) {
        Texture2D<float4> roughTex = ResourceDescriptorHeap[geo.RoughMapIndex];
        roughness *= roughTex.SampleLevel(LinearWrap, uv, albedoLOD).r;
    }
    metallic = saturate(metallic);
    // Piso de roughness do SECUNDARIO (RTXDI `minSecondaryRoughness`, default 0.5 no sample
    // app; Doc/RestirGI.md: "the BRDF of secondary surfaces should be limited [...] such as by
    // clamping the roughness to higher values").
    //
    // O motivo e de armazenamento, nao de aparencia: DDGI e ReSTIR GI guardam UM float3 de
    // radiancia por hit e o reusam de outras direcoes — a sonda integra o hit num cosseno e o
    // reservoir entrega o mesmo Lo a vizinhos que olham o ponto de outro angulo. Um lobo GGX
    // estreito no hit e, por definicao, radiancia que so existe naquela direcao: sem o piso ele
    // vira firefly no reuso, e ai quem "conserta" e o FireflyMax (4/8), que tira energia.
    // Com o piso o mesmo brilho vira um lobo largo — perde o highlight nitido do 2o bounce
    // (que nenhum dos dois caches sabe representar) e devolve a energia distribuida.
    //
    // As REFLEXOES passam 0 aqui de proposito: la o hit e sombreado p/ uma direcao de visada
    // conhecida e consumido uma vez so, sem reuso — clampar borraria espelho-no-espelho.
    // O 0.04 continua embaixo como piso numerico do GGX, valha o que valer o knob.
    roughness = max(roughness, max(roughnessMin, 0.04f));

    FHitMaterial M;
    M.DiffuseColor  = albedo * (1.0f - metallic);
    M.SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    M.Roughness     = roughness;
    M.Alpha         = roughness * roughness;
    M.A2            = M.Alpha * M.Alpha;
    return M;
}

// Emissivo do hit (mesma formula do GBuffer.ps: factor*strength ja bakeado no InstanceGeo,
// x mapa quando ha) — sem isto, superficies emissivas nao alimentam GI nem aparecem em
// reflexoes/ReSTIR. O mapa e obrigatorio quando existe (factor costuma ser 1 e o mapa e
// quase todo preto — so o factor estouraria a superficie inteira).
float3 PT_LoadHitEmissive(InstanceGeo geo, float2 uv, float albedoLOD) {
    float3 emissive = geo.EmissiveFactor.rgb;
    if ((geo.Flags & INSTGEO_FLAG_EMISSIVE) != 0u) {
        Texture2D<float4> emissiveTex = ResourceDescriptorHeap[geo.EmissiveMapIndex];
        emissive *= emissiveTex.SampleLevel(LinearWrap, uv, albedoLOD).rgb;
    }
    return emissive;
}

// --------------------------------------------------------------------------------------------
// Iluminacao direta
// --------------------------------------------------------------------------------------------
// Origem dos shadow rays: hitPos deslocado pela normal de FACE. Uma funcao so porque os tres
// sitios que a calculavam tinham de concordar — e o do ReGIR ja tinha divergido uma vez.
float3 PT_ShadowRayOrigin(FHitSurface S, FHitShadeParams P) {
    return S.Pos + S.OffsetN * max(P.ShadowRayBias, RayEpsA.w);
}

float3 PT_ShadeDirectSun(FHitSurface S, FHitMaterial M, float3 V, FHitShadeParams P) {
    float ndl = saturate(dot(S.ShadingN, P.SunDir));
    float vis = 1.0f;
    if (ndl > 0.0f) {
        RayDesc sray;
        sray.Origin    = PT_ShadowRayOrigin(S, P);
        sray.Direction = P.SunDir; // direcional: a direcao nao depende da origem deslocada
        sray.TMin      = RayEpsB.x;
        sray.TMax      = P.MaxRayDist;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, P.ShadowRayMask, sray);
        SMILE_RT_PROCEED(sq)
        vis = (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
    }
    return BRDF_Direct(S.ShadingN, V, P.SunDir, P.SunColor * P.SunIntensity * vis,
                       M.DiffuseColor, M.SpecularColor, M.Roughness, M.A2, 0.0f);
}

// ACUMULA em `directLighting` em vez de devolver o proprio termo, e isso e deliberado: a soma
// original era `sol; += luz1; += luz2; ...`, e ponto flutuante nao e associativo. Devolver a soma
// das locais para somar ao sol depois daria um resultado diferente na ultima casa — invisivel
// (muito abaixo do piso de ruido medido), mas suficiente para o A/B deste refactor sair "dentro do
// ruido" em vez de IDENTICO. Bit a bit e uma prova mais barata de ler que um limiar.
void PT_AddDirectLocal(FHitSurface S, FHitMaterial M, float3 V, FHitShadeParams P,
                       inout float3 directLighting) {
    // ReGIR substitui o loop O(N) por 8 propostas do pool da celula + UM shadow ray. O sol fica
    // dedicado acima. Fora da grade (ou com o toggle off), o loop historico permanece como
    // referencia exata e fallback funcional.
    uint regirLight;
    float3 regirEstimate;
    bool regirHandled = false;
    [branch] if (P.ReGIREnabled) {
        regirHandled = ReGIRSelectPunctual(
            S.Pos, S.ShadingN, V, M.DiffuseColor, M.SpecularColor, M.Roughness, M.A2,
            P.ReGIRGridMin, P.ReGIRInvCellSize, P.ReGIRGridCount,
            P.ReGIRSlotsPerCell, (uint)P.ReGIRSampleCount, P.ReGIRSlotsSRV,
            P.ReGIRAverageSRV, P.FrameIndex, (uint)P.NumLights,
            regirLight, regirEstimate);
    }

    if (regirHandled && regirLight != REGIR_INVALID_LIGHT) {
        const float3 lorg = PT_ShadowRayOrigin(S, P);
        const float3 toL = SceneLights[regirLight].PosInvRadius.xyz - lorg;
        const float lenL = max(length(toL), 1e-4f);
        const float lTMax = lenL - kLightRayEndMargin;
        if (lTMax <= RayEpsB.x + kLightRayMinTMax) {
            directLighting += regirEstimate;
        } else {
            RayDesc lray;
            lray.Origin = lorg; lray.Direction = toL / lenL;
            lray.TMin = RayEpsB.x; lray.TMax = lTMax;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
            lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                              P.ShadowRayMask, lray);
            SMILE_RT_PROCEED(lq)
            if (lq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                directLighting += regirEstimate;
        }
    } else if (!regirHandled) {
      [loop] for (int li = 0; li < P.NumLights; ++li) {
        float3 Ll; float distL;
        float3 contrib = HitPunctualBRDF(SceneLights[li], S.Pos, S.ShadingN, V,
                                         M.DiffuseColor, M.SpecularColor, M.Roughness, M.A2,
                                         Ll, distL);
        if (dot(contrib, float3(0.2126f, 0.7152f, 0.0722f)) < 1e-3f) continue;

        // Segmento medido da origem EFETIVA (deslocada pelo ShadowRayBias): com origem em
        // hitPos+N*b mas direcao/TMax calculados de hitPos, origem/direcao/comprimento
        // descreviam segmentos diferentes — com luz proxima (b=0.2!) o erro angular e grande.
        // O shading (contrib) continua medido do hitPos real; so o raio usa o segmento efetivo.
        float3 lorg = PT_ShadowRayOrigin(S, P);
        float3 toL  = (S.Pos + Ll * distL) - lorg;
        float  lenL = max(length(toL), 1e-4f);

        // TMax NUNCA passa da luz — o piso antigo (kLightRayMinTMax) podia empurra-lo p/ ALEM
        // dela e, pior, deixa-lo ABAIXO do TMin: com ShadowRayTMin virando knob (ate 50 mm) e uma
        // luz a poucos centimetros, saia TMin 50 mm > TMax 20 mm = intervalo invalido no DXR.
        // Agora o segmento e o real e, se nao sobrar corpo util entre TMin e TMax, a luz conta
        // como VISIVEL (o trecho nao testado e menor que os proprios epsilons).
        float lTMax = lenL - kLightRayEndMargin;
        if (lTMax <= RayEpsB.x + kLightRayMinTMax) {
            directLighting += contrib;
            continue;
        }

        RayDesc lray;
        lray.Origin    = lorg;
        lray.Direction = toL / lenL;
        lray.TMin      = RayEpsB.x;
        lray.TMax      = lTMax;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> lq;
        lq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, P.ShadowRayMask, lray);
        SMILE_RT_PROCEED(lq)
        if (lq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) directLighting += contrib;
      }
    }
}

// --------------------------------------------------------------------------------------------
// Fallback indireto (DDGI)
// --------------------------------------------------------------------------------------------
// A IRRADIANCIA crua do volume, sem a BRDF ambiente — quem compoe e o PT_ComposeIndirect. Separado
// porque a Fase 5 troca a FONTE (cache no lugar do volume) sem trocar a composicao, e a Fase 3 nao
// pode chamar isto de jeito nenhum: o updater alimentado por DDGI so esconderia um sinal cuja
// origem continuaria sendo DDGI.
float3 PT_SampleIndirectFallback(FHitSurface S, float3 rayDir, FHitShadeParams P) {
    // 2o bounce com o gather COMPLETO — Chebyshev, bias de superficie, offset de relocacao e
    // skip de sonda inativa —, o mesmo que o deferred usa. Antes aqui era a trilinear pura, e
    // isso era o pior lugar possivel para vazar: o resultado do hit volta para o atlas do DDGI,
    // que reamostra com hysteresis 0,99, entao o leak se REALIMENTA frame a frame. O Flax
    // tambem usa o sampler completo no bounce do surface atlas.
    //
    // O papel de "direcao da camera" no bias e do raio: quem observa este ponto e a origem do
    // raio, entao V = -rayDir. Sem isso o termo de view empurraria o ponto para uma direcao sem
    // relacao com a visada e o bias perderia o sentido geometrico.
    // O bias em si e calculado DENTRO do wrapper, uma vez por cascata — ele escala com o
    // espacamento e no blend as duas cascatas querem valores diferentes.
    float2 distInvSize = float2(1.0f / GIDistParams.y, 1.0f / GIDistParams.z);
    // Fora do volume o gather clampa e estende a ultima fileira de sondas ao infinito. Na tela
    // isso e substituido pelo ambiente hemisferico; aqui a radiancia do hit REALIMENTA o atlas,
    // entao extrapolar seria pior — a borda se reinjetaria. Sem o ambiente colorido do deferred
    // (nao existe no CB de um passe de RT), o hit desvanece o indireto para ZERO: escurece o
    // bounce distante em vez de inventar luz. O direto do hit (sol e puntuais) segue inteiro.
    // GIDistParams.w carrega DUAS coisas (ver FGIHitSampling::SkipModePacked): o skipMode de
    // sonda inativa nos dois bits baixos e, no bit 2, o gate de MEDICAO — "corte o DDGI como
    // terminador e me diga quanto muda". Com o gate ligado nao ha gather nenhum: o volW vai a
    // zero e o custo do tap tambem sai, o que mantem a medicao honesta tambem no profiler.
    const uint giHitFlags = (uint)GIDistParams.w;
    const bool termOff    = (giHitFlags & 4u) != 0u;
    float volW = termOff ? 0.0f
                         : DDGI_VolumeWeight(S.Pos, P.GridMin, P.Spacing, P.Count, GIBiasParams.z);
    float3 indirect = float3(0.0f, 0.0f, 0.0f);
    if (volW > 0.0f) {
        // Tiles por linha (empacotamento 2D; ver DDGI_TileOrigin) derivado do par
        // (largura, tile) do atlas de distancia, que ja chega aqui pelo contrato de NOME do
        // GIDistParams — os cinco shaders que incluem este arquivo o declaram, entao nao ha
        // campo novo a plumbar nem estado duplicado a dessincronizar.
        const int tilesPerRow = DDGI_TilesPerRow(GIDistParams.y, (int)GIDistParams.x);
        // Wrapper com selecao e blend de cascata. O bias NAO entra pronto — ver a nota no
        // wrapper: ele escala com o espacamento e cada cascata quer o seu.
        //
        // O fallback deste caller e PRETO, e nao ambiente hemisferico: nao existe cor de ambiente
        // no cbuffer de um passe de RT. Por isso o volW continua aqui fora, e o wrapper devolve
        // so a irradiancia do DDGI.
        indirect = SampleDDGIIrradianceChebCascaded(
            IrradAtlas, GIDistAtlas, LinearClamp, S.Pos, S.ShadingN, -rayDir,
            GICascadeGridMinSpacing, GICascadeScrollOffset, (int)GICascadeParams.x, P.Count,
            P.AtlasTile, P.AtlasInvSize, (int)GIDistParams.x, distInvSize,
            GIProbeData, giHitFlags & 3u, tilesPerRow,
            GIBiasParams.x, GIBiasParams.y);
        if (volW < 1.0f) indirect *= volW;
    }
    return indirect;
}

// Overload que devolve TAMBEM quem respondeu. Existe para a telemetria de fonte da Fase 5 poder
// separar "o volume cobriu este ponto" de "nao havia volume e o indireto foi zero explicito" —
// duas coisas que a radiancia devolvida nao distingue (um DDGI legitimamente escuro tambem sai
// preto). `volW` e o unico lugar onde essa diferenca existe, e ela morria dentro da funcao.
//
// Sem duplicar corpo: quem responde e o mesmo bloco de cima. `termOff` conta como NAO respondido,
// e corretamente — o gate de medicao existe justamente para tirar o DDGI do terminal.
float3 PT_SampleIndirectFallback(FHitSurface S, float3 rayDir, FHitShadeParams P,
                                 out bool outDDGIAnswered) {
    const uint giHitFlags = (uint)GIDistParams.w;
    const bool termOff    = (giHitFlags & 4u) != 0u;
    outDDGIAnswered = !termOff &&
                      DDGI_VolumeWeight(S.Pos, P.GridMin, P.Spacing, P.Count, GIBiasParams.z) > 0.0f;
    return PT_SampleIndirectFallback(S, rayDir, P);
}

// O atlas fornece irradiancia difusa, nao uma distribuicao direcional que permita integrar
// GGX de verdade. O fallback split-sum usa Fresnel roughness-aware e reserva (1-F) para o
// difuso: metal devolve energia tingida por F0 sem fingir que o atlas conhece a direcao de
// espelho. O direto continua sendo a BRDF GGX direcional completa.
float3 PT_ComposeIndirect(FHitSurface S, FHitMaterial M, float3 V, float3 indirect) {
    const float NoV = saturate(dot(S.ShadingN, V));
    const float3 ambientF = F_SchlickRoughness(M.SpecularColor, NoV, M.Roughness);
    return ((1.0f - ambientF) * M.DiffuseColor + ambientF) * indirect;
}

// --------------------------------------------------------------------------------------------
// Politica de auto-interseccao / backface (Lumen AvoidSelfIntersections modo Retrace)
// --------------------------------------------------------------------------------------------
// Os dois passos sao UMA politica so e nao podem ser separados: sozinha, a terminacao preta
// transformaria toda auto-interseccao proxima em MANCHA PRETA, trocando um artefato por outro.
// Primeiro se descarta o hit proximo suspeito; SO o que sobrar vira oclusao.
//
// Devolve `true` quando o caminho deve MORRER: verso de geometria one-sided que sobreviveu ao
// teste de proximidade, ou seja, geometria real vista por dentro. O `q` sai re-tracado quando
// houve retrace, entao o caller le hit/distancia DELE e nao do trace original.
//
// ⚠️ DUAS COPIAS DESTA POLITICA existem hoje no ReSTIRGITrace.cs.hlsl (gather e revalidacao),
// escritas antes desta funcao. Elas devem migrar para ca — mas em commit proprio, com a
// comparacao de DXIL que a serie usa, porque mexer nelas e mexer no caminho de RENDER.
bool PT_ResolveSelfIntersection(inout RayQuery<RAY_FLAG_NONE> q, inout RayDesc ray, uint mask) {
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;

    bool twoSided;
    bool backface = HitIsBackface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                  q.CommittedWorldToObject3x4(), ray.Direction, twoSided);
    const float t = q.CommittedRayT();

    float skipDist = -1.0f;
    if (twoSided && t < kSelfIsectTwoSidedDist)                  skipDist = t + kSelfIsectRetraceBias;
    else if (!twoSided && backface && t < kSelfIsectBackfaceDist) skipDist = kSelfIsectBackfaceDist;

    if (skipDist > 0.0f) {
        const float tmin = ray.TMin;
        ray.TMin = skipDist;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
        SMILE_RT_PROCEED(q)
        ray.TMin = tmin; // a origem nao mudou; so o intervalo daquela consulta
        // Re-classifica SO aqui: no caminho comum a classificacao de cima ja vale, e o
        // HitIsBackface custa UM registro de 32 B (era 3 indices + 3 posicoes, dependentes).
        backface = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) &&
                   HitIsBackface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                 q.CommittedWorldToObject3x4(), ray.Direction, twoSided);
    }

    return backface && !twoSided && q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// --------------------------------------------------------------------------------------------
// Amostragem do BSDF — o bloco que so o updater da Fase 3 usa
// --------------------------------------------------------------------------------------------
// O render AVALIA a BRDF para uma direcao conhecida (a da luz, no BRDF_Direct); quem precisa
// SORTEAR a direcao e o laco de bounces do updater. Por isso isto nasce aqui e nao na Fase 2:
// nao havia produtor.
//
// Nao ha BRDF nova. O peso sai do PROPRIO BRDF_Direct com radiancia unitaria — o mesmo Burley,
// o mesmo GGX e a mesma compensacao de Kulla-Conty que a luz direta usa. Escrever "albedo/pi"
// aqui seria uma segunda BRDF, e ela divergiria no primeiro ajuste do modelo difuso.
struct FBsdfSample {
    float3 Dir;        // direcao amostrada, em MUNDO
    float3 Throughput; // f * cos / pdf — o peso da radiancia que vier de Dir
    float  Pdf;        // densidade da mistura, em angulo solido
    uint   Lobe;       // PT_LOBE_*: qual lobo gerou a direcao
    bool   Valid;
};

// `Edir` sorteia a direcao dentro do lobo; `Elobe` escolhe o lobo. Numeros separados de proposito:
// reaproveitar uma componente de Edir para a escolha correlaciona lobo e direcao, e a correlacao
// aparece como faixa de material na tabela do cache.
FBsdfSample PT_SampleBSDF(FHitSurface S, FHitMaterial M, float3 V, float2 Edir, float Elobe) {
    FBsdfSample B;
    B.Dir        = float3(0.0f, 0.0f, 1.0f);
    B.Throughput = float3(0.0f, 0.0f, 0.0f);
    B.Pdf        = 0.0f;
    B.Lobe       = PT_LOBE_NONE;
    B.Valid      = false;

    const float3x3 basis = GGX_TangentBasis(S.ShadingN);
    const float3   Vt    = mul(basis, V);
    // Visada abaixo do horizonte da normal de SHADING. Acontece de verdade: o facing usa a normal
    // de FACE (ver PT_LoadHitSurface), entao perto da silhueta de uma malha suavizada a
    // interpolada pode ficar do outro lado. Sem caminho valido para amostrar, o caminho morre —
    // o que custa uma amostra, e nao um lobo com pdf negativa.
    if (Vt.z <= 1e-4f) return B;

    // Probabilidade do lobo pela ENERGIA de cada um [Cyberpunk 2077, "escolher o lobo a partir da
    // energia difusa/especular"]. Metal tem DiffuseColor zero e cairia inteiro no especular, que e
    // o certo: com o piso de roughness do secundario (PT_LoadHitMaterial) esse lobo e largo o
    // bastante para o cache nao-direcional representa-lo.
    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
    const float  wd    = dot(M.DiffuseColor,  kLuma);
    const float  ws    = dot(M.SpecularColor, kLuma);
    const float  pDiff = (wd + ws > 1e-6f) ? saturate(wd / (wd + ws)) : 1.0f;

    const float  alpha = max(M.Alpha, 1e-3f);
    const bool   diffuseLobe = (Elobe < pDiff);

    float3 Lt;
    if (diffuseLobe) {
        // Cosseno-hemisferico (Malley), o mesmo mapeamento do sample inicial do ReSTIR GI.
        const float r    = sqrt(Edir.x);
        const float phi  = 2.0f * SMILE_PI * Edir.y;
        Lt = float3(r * cos(phi), r * sin(phi), sqrt(saturate(1.0f - Edir.x)));
    } else {
        const float4 ggx = GGX_SampleVNDF(Edir, alpha, Vt);
        Lt = reflect(-Vt, ggx.xyz);
        // Reflexao abaixo do horizonte: o VNDF permite, a BRDF nao. Descartar (e nao dobrar para
        // cima) mantem o estimador sem vies — a energia perdida e a que o proprio lobo perde.
        if (Lt.z <= 1e-4f) return B;
    }

    // pdf da MISTURA, e nao a do lobo sorteado: as duas tecnicas podem gerar a mesma direcao,
    // entao a densidade real e a soma ponderada. Dividir so pela do lobo escolhido tambem seria
    // nao-enviesado, mas com a variancia que aparece justo onde os lobos se sobrepoem.
    const float pdfDiff = Lt.z * (1.0f / SMILE_PI);
    const float pdfSpec = GGX_VndfPdf(alpha, Vt, Lt);
    const float pdf     = pDiff * pdfDiff + (1.0f - pDiff) * pdfSpec;
    if (!(pdf > 1e-6f)) return B;

    const float3 L = normalize(mul(Lt, basis));
    // Radiancia unitaria => o retorno E o f * cos.
    const float3 fCos = BRDF_Direct(S.ShadingN, V, L, float3(1.0f, 1.0f, 1.0f),
                                    M.DiffuseColor, M.SpecularColor, M.Roughness, M.A2, 0.0f);

    B.Dir        = L;
    B.Throughput = fCos / pdf;
    B.Pdf        = pdf;
    B.Lobe       = diffuseLobe ? PT_LOBE_DIFFUSE : PT_LOBE_SPECULAR;
    B.Valid      = any(B.Throughput > 0.0f);
    return B;
}

#endif
