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

// Estado do CAMINHO que chegou ao hit — o que se sabe do raio, nao da superficie.
//
// Hoje o render usa Origin/Dir/SegmentLength/RayRoughness; Depth e Mode ja existem porque a Fase 3
// bifurca por eles (o updater tem profundidade propria e politica de terminacao propria).
//
// Throughput, PDF e lobo escolhido NAO estao aqui, apesar de o plano os listar: nenhum consumidor
// os preenche ainda, e campo sem produtor e campo que nasce mentindo. Entram na Fase 3, junto com o
// laco de bounces que os calcula — e ai com valor de verdade.
struct FPathState {
    float3 Origin;        // origem do segmento que chegou a este hit
    float3 Dir;           // direcao dele, normalizada
    float  SegmentLength; // comprimento do segmento — gate de auto-referencia do cache
    // Roughness do lobo que GEROU o raio, nao a do material atingido. Negativa = transporte
    // difuso; positiva entra no gate de cone do cache.
    float  RayRoughness;
    uint   Depth;         // 0 = primeiro hit secundario
    uint   Mode;          // PT_MODE_*
};

#define PT_MODE_RENDER       0u // trace de render: CONSULTA o cache
#define PT_MODE_CACHE_UPDATE 1u // Fase 3: o passe dedicado que o ALIMENTA

FPathState PT_MakePathState(float3 rayOrigin, float3 rayDir, float hitDist, float rayRoughness) {
    FPathState S;
    S.Origin        = rayOrigin;
    S.Dir           = rayDir;
    S.SegmentLength = hitDist;
    S.RayRoughness  = rayRoughness;
    S.Depth         = 0u;
    S.Mode          = PT_MODE_RENDER;
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

    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];
    uint   i0   = Idx[tri * 3 + 0];
    uint   i1   = Idx[tri * 3 + 1];
    uint   i2   = Idx[tri * 3 + 2];
    float3 nObj = Verts[i0].Normal * (1.0f - bary.x - bary.y)
                + Verts[i1].Normal * bary.x
                + Verts[i2].Normal * bary.y;
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
    const bool faceOk        = HitFaceNormal(Verts, i0, i1, i2, worldToObject, faceN);
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

    S.UV = Verts[i0].TexCoord * (1.0f - bary.x - bary.y)
         + Verts[i1].TexCoord * bary.x
         + Verts[i2].TexCoord * bary.y;

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

// O atlas fornece irradiancia difusa, nao uma distribuicao direcional que permita integrar
// GGX de verdade. O fallback split-sum usa Fresnel roughness-aware e reserva (1-F) para o
// difuso: metal devolve energia tingida por F0 sem fingir que o atlas conhece a direcao de
// espelho. O direto continua sendo a BRDF GGX direcional completa.
float3 PT_ComposeIndirect(FHitSurface S, FHitMaterial M, float3 V, float3 indirect) {
    const float NoV = saturate(dot(S.ShadingN, V));
    const float3 ambientF = F_SchlickRoughness(M.SpecularColor, NoV, M.Roughness);
    return ((1.0f - ambientF) * M.DiffuseColor + ambientF) * indirect;
}

#endif
