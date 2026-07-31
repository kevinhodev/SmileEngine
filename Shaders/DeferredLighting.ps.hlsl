#include "GBuffer.hlsli"
#include "BRDF.hlsli"
#include "Shadow/CSMCommon.hlsli"   
#include "GI/DDGICommon.hlsli"     

cbuffer FrameCB : register(b0) {
    float4 CameraPosition;
    float4 IBLParams;     
    float4 Time;
    float4 SunDirection;  
    float4 SunColor;
    float4 SkyAmbientColor;    
    float4 GroundAmbientColor; 
    float4 DDGIGridMin;        
    float4 DDGIGridCount;      
    float4 DDGIParams;         
    float4 DDGIDistParams;    
    float4 ReflectionParams;   
    float4 MoonDirection;
    float4 MoonColor;
    row_major float4x4 InvViewProj;
    float4 RenderParams;       // nao usado aqui; mantem os offsets do FrameConstants
    float4 CloudShadowParams;  // xy = centro XZ (km), z = 1/extent, w = forca (0 = off)
    float4 CloudShadowParams2; // x = km/unidade, y = altura da base (km), zw = keyDir.xz/y
    float4 LightParams;        // x = nº de luzes puntuais no buffer t17,
                               // y = 1/res do atlas de sombra local, z = bias (NDC z),
                               // w = direta local: 0 raster, 1 ReSTIR DI
    float4 LightParams2;       // x = 1/res do cube shadow (point), y = near das faces, zw = -
    float4 DDGIBiasParams;     // x = escala do bias (0.2 legado), y = teto em metros (0 = sem
                               // teto), zw = reservados
};

// Luz puntual (point/spot) — espelha o FGPULight do Renderer.h. SpotParams.z e o fade do slot de
// sombra; .w preserva o pedido CastShadows para o ReSTIR DI, independentemente do orcamento.
struct FGPULight {
    float4 PosInvRadius;      // xyz = posicao, w = 1/raio de atenuacao
    float4 ColorSourceRadius; // rgb = cor*intensidade, w = bulb (distancia minima)
    float4 DirCosOuter;       // xyz = eixo do spot, w = cos(outer); -2 = point (sem cone)
    float4 SpotParams;        // x = 1/(cosInner - cosOuter), y = slice de sombra (-1 = sem),
                              // z = fade do slot [0..1], w = CastShadows (0/1)
    row_major float4x4 ShadowMatrix; // world -> UVZ do slice (dividir por w: perspectiva)
    float4 PrevPosInvRadius;          // xyz = posicao anterior (consumida pelo ReSTIR DI)
};

StructuredBuffer<FGPULight> Lights : register(t17);
Texture2DArray   LocalShadowMap    : register(t18); // atlas D32 dos spots sombreados (F3a)
TextureCubeArray LocalCubeShadow   : register(t19); // cube array dos points sombreados (F3b)

// Depth NDC (LH, 0..1) da projecao das sombras locais a partir da distancia LINEAR ao longo
// do eixo da face/cone. O bias e aplicado em ESPACO LINEAR antes desta conversao: bias
// constante em NDC numa projecao perspectiva com near pequeno vale centimetros perto da luz
// e METROS na borda do alcance (peter-panning longe, acne perto).
float LocalNdcDepth(float viewZ, float farP) {
    float nearP = LightParams2.y;
    float fRng  = farP / max(farP - nearP, 1e-4f);
    return fRng - (fRng * nearP) / max(viewZ, nearP);
}

// Bias em metros: constante + termo relativo (a incerteza do depth cresce com a distancia,
// junto com o texel projetado).
float LocalShadowBias(float viewZ) {
    return LightParams.z + viewZ * 0.008f;
}

// PCF 3x3 no slice do atlas (ShadowCmp s2 do CSMCommon). uv ja dividido por w; refZ vem
// pre-calculado do caminho linear (LocalNdcDepth).
float LocalShadowPCF(float2 uv, float refZ, float slice) {
    float t   = LightParams.y; // 1/res
    float vis = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            vis += LocalShadowMap.SampleCmpLevelZero(
                       ShadowCmp, float3(uv + float2(x, y) * t, slice), refZ);
    return vis * (1.0f / 9.0f);
}

// AreaSphereSpecular vive no BRDF.hlsli, junto da BRDF_DirectArea que consome a saida dela. O
// deferred e o ReSTIR DI precisam do mesmo especular de area para preservar a aparencia das luzes.

// Sombra de POINT: o vetor luz->pixel escolhe a face do cubo no hardware; a profundidade de
// referencia usa o EIXO DOMINANTE (viewZ da face que vai responder — mesma projecao de 90
// graus com que as faces foram renderizadas), entao nao precisa de matriz por luz. PCF de
// 4 taps em grade rotacionada na base tangente da direcao.
float LocalCubeShadowPCF(float3 lightToPixel, float cube, float farP) {
    // Pixel na posicao exata da luz: nao existe direcao de lookup (normalize(0) = NaN) e
    // nada pode ocluir a luz dela mesma — devolve iluminado.
    if (dot(lightToPixel, lightToPixel) < 1e-12f) return 1.0f;

    float3 ad    = abs(lightToPixel);
    float  viewZ = max(ad.x, max(ad.y, ad.z));

    // Bias linear (x1.5: 90 graus + 512 = texel mais gordo que o do spot) -> NDC da face.
    float refZ = LocalNdcDepth(viewZ - LocalShadowBias(viewZ) * 1.5f, farP);

    float3 dir = normalize(lightToPixel);
    float3 T   = normalize(cross(dir, abs(dir.y) > 0.9f ? float3(1.0f, 0.0f, 0.0f)
                                                        : float3(0.0f, 1.0f, 0.0f)));
    float3 B   = cross(dir, T);
    float  ang = 2.0f * LightParams2.x; // ~1 texel angular da face

    static const float2 kOfs[4] = {
        float2(-0.5f, -1.5f), float2( 1.5f, -0.5f),
        float2(-1.5f,  0.5f), float2( 0.5f,  1.5f)
    };
    float vis = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        float3 d = dir + (T * kOfs[k].x + B * kOfs[k].y) * ang;
        vis += LocalCubeShadow.SampleCmpLevelZero(ShadowCmp, float4(d, cube), refZ);
    }
    return vis * 0.25f;
}

Texture2D        GBufferA   : register(t0);
Texture2D        GBufferB   : register(t1);
Texture2D        GBufferC   : register(t2);
Texture2D<float> SceneDepth : register(t3);
Texture2D<float> CloudShadowMap : register(t4); // transmitancia das nuvens p/ a key light

TextureCube IrradianceMap  : register(t8);
TextureCube PrefilteredMap : register(t9);
Texture2D   BRDFLut        : register(t10);

Texture2D<float4> DDGIIrradianceAtlas : register(t12);
Texture2D<float4> DDGIDistanceAtlas   : register(t13);
Buffer<float4>    DDGIProbeData        : register(t15);
Texture2D<float>  SceneAO             : register(t14);

// ReSTIR GI: irradiancia difusa por pixel (final-gather sobre o DDGI). Ativa via ReflectionParams.w.
// Quando ligada, substitui o termo difuso do DDGI; o DDGI segue como cache no trace (multi-bounce).
Texture2D<float4> ReSTIRGITex         : register(t16);
// Direta local integral resolvida pelo ReSTIR DI. Ja chega com BRDF e visibilidade aplicadas;
// LightParams.w escolhe entre este alvo e o loop raster abaixo.
Texture2D<float4> DirectLocalTex      : register(t20);

SamplerState IBLSampler : register(s1); 

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// GTAO F3 — toggles p/ A/B (0 = comportamento antigo: AO escalar cheio em tudo)
#define GTAO_MULTIBOUNCE    1
#define GTAO_SPEC_OCCLUSION 1

// Multibounce do Jimenez (GTAO paper / UE AOMultiBounce em BRDF.ush): aproxima a luz que
// rebate DENTRO da area ocluida — superficie clara devolve energia e o canto nao vira carvao.
float3 AOMultiBounce(float3 baseColor, float ao) {
    float3 a =  2.0404f * baseColor - 0.3324f;
    float3 b = -4.7951f * baseColor + 0.6417f;
    float3 c =  2.7552f * baseColor + 0.6903f;
    return max(ao, ((ao * a + b) * ao + c) * ao);
}

// Specular occlusion do Lagarde (Moving Frostbite to PBR, listing 26): deriva a oclusao do
// lobo especular a partir do AO difuso — lobo estreito (roughness baixa) e rasante escapa
// da oclusao; aplicar o AO difuso cheio no especular escurece demais.
float SpecularOcclusionFromAO(float noV, float roughness, float ao) {
    return saturate(pow(abs(noV + ao), exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
}

// Amostra o indireto do DDGI respeitando os flags do frame (Chebyshev/skip de probes inativos).
// Caminho UNICO p/ geometria e folhagem — com os pisos defensivos do Chebyshev (DDGICommon),
// folhagem densa nao crusha mais a preto e nao precisa de sampling separado.
// Ambiente indireto para pontos FORA do volume de sondas: reproduz o que este mesmo shader
// usaria se o GI estivesse desligado, e nessa ordem — ambiente atmosferico hemisferico quando
// ligado (o ramo UseAtmoAmbient), senao o difuso do IBL (o ramo IBLParams.w), senao nada.
// Seguir o gate importa: sem ele, desligar o ambiente atmosferico deixaria o fallback usando as
// cores dele assim mesmo, que nao e o que o resto da cena faz.
//
// O caller multiplica por giIntensity, entao dividimos aqui: o ambiente de fora do volume nao
// deve escalar com o slider de intensidade do GI — a promessa e "o mesmo que existiria sem GI".
float3 DDGI_FallbackAmbient(float3 N) {
    float3 amb = float3(0.0f, 0.0f, 0.0f);
    if (SkyAmbientColor.w > 0.5f) {
        float hemi = saturate(N.y * 0.5f + 0.5f);
        amb = lerp(GroundAmbientColor.rgb, SkyAmbientColor.rgb, hemi) * GroundAmbientColor.w;
    } else if (IBLParams.w > 0.5f) {
        amb = IrradianceMap.SampleLevel(IBLSampler, RotateY(N, IBLParams.y), 0.0f).rgb
            * IBLParams.x;
    }
    float giI = (DDGIParams.x > 0.0f) ? DDGIParams.x : 1.0f; // espelha o giIntensity do caller
    return amb / giI;
}

float3 SampleSceneDDGI(float3 worldPos, float3 N) {
    float2 atlasInvSize = float2(1.0f / DDGIParams.z, 1.0f / DDGIParams.w);
    int  giFlags      = (int)DDGIDistParams.w;
    bool useChebyshev = (giFlags & 1) != 0;
    bool skip         = (giFlags & 2) != 0;
    bool fallback     = (giFlags & 4) != 0;
    uint skipMode     = skip ? (fallback ? 2u : 1u) : 0u;

    // Fora do volume o gather nao falha: ele CLAMPA e estende as probes de borda ao infinito.
    // Quando o peso cai a zero nem vale pagar os 8 taps.
    float volW = DDGI_VolumeWeight(worldPos, DDGIGridMin.xyz, DDGIGridMin.w,
                                   (int3)DDGIGridCount.xyz, DDGIBiasParams.z);
    if (volW <= 0.0f) return DDGI_FallbackAmbient(N);

    float3 gi;
    if (useChebyshev) {
        float2 distInvSize = float2(1.0f / DDGIDistParams.y, 1.0f / DDGIDistParams.z);
        float3 V = normalize(CameraPosition.xyz - worldPos);
        float3 biasVec = DDGI_SurfaceBias(N, V, DDGIGridMin.w,
                                          DDGIBiasParams.x, DDGIBiasParams.y);
        gi = SampleDDGIIrradianceCheb(DDGIIrradianceAtlas, DDGIDistanceAtlas, IBLSampler,
                 worldPos, N, DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                 (int)DDGIParams.y, atlasInvSize, (int)DDGIDistParams.x, distInvSize, biasVec,
                 DDGIProbeData, skipMode);
    } else {
        gi = SampleDDGIIrradiance(DDGIIrradianceAtlas, IBLSampler, worldPos, N,
                 DDGIGridMin.xyz, DDGIGridMin.w, (int3)DDGIGridCount.xyz,
                 (int)DDGIParams.y, atlasInvSize);
    }
    return (volW >= 1.0f) ? gi : lerp(DDGI_FallbackAmbient(N), gi, volW);
}

float4 main(VSOutput input) : SV_Target {
    int2  px       = int2(input.pos.xy);
    float rawDepth = SceneDepth.Load(int3(px, 0));
    if (rawDepth <= 0.0f) discard;

    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, rawDepth, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;

    float4 gA = GBufferA.Load(int3(px, 0));
    float4 gB = GBufferB.Load(int3(px, 0));
    float4 gC = GBufferC.Load(int3(px, 0));
    GBufferData g = DecodeGBuffer(gA, gB, gC);

    float3 N         = g.WorldNormal;
    float3 BaseColor = g.BaseColor;
    float  Metallic  = g.Metallic;
    float  Roughness = max(g.Roughness, 0.04f);
    float  matAO     = g.AO;
    bool   IsFoliage = (g.ShadingModel == SMILE_SHADINGMODEL_FOLIAGE);

    float3 V = normalize(CameraPosition.xyz - worldPos);

    float ssao  = IsFoliage ? 1.0f : SceneAO.Load(int3(px, 0));
    float AOAll = matAO * ssao;

    if (Time.w > 0.5f) return float4(ssao, ssao, ssao, 1.0f); 

    float3 DiffuseColor  = BaseColor * (1.0f - Metallic);
    float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, Metallic);

    // Transmissao data-driven do G-buffer (C.gba = tint x intensidade do material): folhagem
    // vem do loader com 0.6 branco (= o antigo hardcode, paridade bit-a-bit); DefaultLit vem 0.
    // Material com subsurface setado na janela de Materiais transmite sem trocar de ID.
    float3 TransColor = BaseColor * g.Subsurface;

    float a   = Roughness * Roughness;
    float a2  = a * a;
    float NoV = saturate(dot(N, V));

    // AO por lobo: difuso ganha multibounce (tinge com o albedo em vez de escurecer a preto),
    // especular ganha a occlusion derivada do Lagarde em vez do AO difuso cheio.
#if GTAO_MULTIBOUNCE
    float3 AODiffuse = AOMultiBounce(DiffuseColor, AOAll);
#else
    float3 AODiffuse = float3(AOAll, AOAll, AOAll);
#endif
#if GTAO_SPEC_OCCLUSION
    float AOSpec = SpecularOcclusionFromAO(NoV, Roughness, AOAll);
#else
    float AOSpec = AOAll;
#endif

    float ReflectCombine = ReflectionParams.z > 0.5f
        ? saturate((ReflectionParams.x - Roughness) / max(ReflectionParams.y, 1e-4f))
        : 0.0f;
    float SpecAmbientScale = 1.0f - ReflectCombine;

    float3 Lighting = float3(0.0f, 0.0f, 0.0f);
    {
        float3 Lsun        = normalize(SunDirection.xyz);
        float3 SunRadiance = SunColor.rgb * SunDirection.w;
        float3 SunLit      = BRDF_Direct(N, V, Lsun, SunRadiance,
                                         DiffuseColor, SpecularColor, Roughness, a2, TransColor);

        float3 MoonLit = float3(0.0f, 0.0f, 0.0f);
        if (MoonDirection.w > 0.0f) {
            float3 Lmoon        = normalize(MoonDirection.xyz);
            float3 MoonRadiance = MoonColor.rgb * MoonDirection.w;
            MoonLit = BRDF_Direct(N, V, Lmoon, MoonRadiance,
                                  DiffuseColor, SpecularColor, Roughness, a2, TransColor);
        }

        // Sombra das nuvens: projeta o ponto ate a base da camada na direcao da key light
        // e amostra a transmitancia bakeada (CloudShadowMap.cs). Aplicada junto do CSM —
        // nuvem escurece sol E luar (a key light dominante e quem dirige o bake).
        float cloudShadow = 1.0f;
        if (CloudShadowParams.w > 0.0f) {
            float  kKm   = CloudShadowParams2.x;
            float2 hitKm = worldPos.xz * kKm + CloudShadowParams2.zw *
                           max(CloudShadowParams2.y - worldPos.y * kKm, 0.0f);
            float2 suv   = (hitKm - CloudShadowParams.xy) * CloudShadowParams.z + 0.5f;
            if (all(suv > 0.0f) && all(suv < 1.0f)) {
                float T = CloudShadowMap.SampleLevel(IBLSampler, suv, 0.0f).r;
                cloudShadow = lerp(1.0f, T, CloudShadowParams.w);
            }
        }

        Lighting += (SunLit + MoonLit) * SampleCSM(worldPos, N, input.pos.xy) * cloudShadow;
    }

    // Luzes puntuais (point/spot): inverse-square com janela de raio da Unreal
    // ((1-(d/r)^4)^2 — a luz morre exatamente no raio de atenuacao, sem pop de culling)
    // + piso de "bulb" da Cry (distancia minima = SourceRadius: superficie encostada na
    // luz nao estoura a branco) + mascara de cone quadratica no spot (UE/Flax identicas).
    // SEM sombra na F1 (luz vaza parede) — sombras locais chegam na F3.
    {
        // ReSTIR DI resolve o conjunto local inteiro; o loop raster nao participa.
        uint NumLights = (LightParams.w < 0.5f) ? (uint)LightParams.x : 0u;
        [loop]
        for (uint li = 0; li < NumLights; ++li) {
            FGPULight Lp = Lights[li];

            float3 ToLight = Lp.PosInvRadius.xyz - worldPos;
            float  DistSqr = dot(ToLight, ToLight);
            float  InvR    = Lp.PosInvRadius.w;

            float Window = DistSqr * InvR * InvR;          // (d/r)^2
            Window = saturate(1.0f - Window * Window);     // 1-(d/r)^4
            Window *= Window;
            if (Window <= 0.0f) continue;

            float  Dist     = sqrt(DistSqr);
            float3 L        = ToLight / max(Dist, 1e-4f);
            float  DClamped = max(Dist, Lp.ColorSourceRadius.w);
            float  Atten    = Window / (DClamped * DClamped);

            if (Lp.DirCosOuter.w > -1.5f) {
                float SpotMask = saturate((dot(-L, Lp.DirCosOuter.xyz) - Lp.DirCosOuter.w)
                                          * Lp.SpotParams.x);
                Atten *= SpotMask * SpotMask;
            }
            if (Atten <= 0.0f) continue;

            // Sombra local: spot projeta pelo ShadowMatrix (slice 2D, F3a); point vai pelo
            // cube array com refZ do eixo dominante (F3b). Normal offset pequeno nos dois
            // contra acne de contato.
            // Visibilidade BASE: o que o shadow map entrega. Fica 1 quando a luz nao tem slice.
            float baseVis = 1.0f;
            if (Lp.SpotParams.y >= 0.0f) {
                float3 offPos = worldPos + N * 0.02f;
                float  shadow = 1.0f;
                if (Lp.DirCosOuter.w > -1.5f) {
                    float4 sp = mul(float4(offPos, 1.0f), Lp.ShadowMatrix);
                    if (sp.w > 0.0f) {
                        float3 uvz = sp.xyz / sp.w;
                        if (all(uvz.xy > 0.0f) && all(uvz.xy < 1.0f) &&
                            uvz.z > 0.0f && uvz.z < 1.0f) {
                            // refZ pelo caminho LINEAR (dist ao longo do eixo do cone),
                            // com bias em metros — nao pelo uvz.z (bias NDC ~ peter-panning).
                            float viewZ = dot(offPos - Lp.PosInvRadius.xyz,
                                              Lp.DirCosOuter.xyz);
                            float farP  = max(1.0f / Lp.PosInvRadius.w,
                                              LightParams2.y * 2.0f);
                            float refZ  = LocalNdcDepth(viewZ - LocalShadowBias(viewZ), farP);
                            shadow = LocalShadowPCF(uvz.xy, refZ, Lp.SpotParams.y);
                        }
                    }
                } else {
                    shadow = LocalCubeShadowPCF(offPos - Lp.PosInvRadius.xyz,
                                                Lp.SpotParams.y,
                                                max(1.0f / Lp.PosInvRadius.w,
                                                    LightParams2.y * 2.0f));
                }
                // Fade do slot: a luz que perde o slice nao vira sem-sombra de um frame pro
                // outro (a luz passaria a vazar parede num piscar). O slice segue valido
                // enquanto o fade cai, e o mapa some suave.
                baseVis = lerp(1.0f, shadow, Lp.SpotParams.z);
            }
            Atten *= baseVis;
            if (Atten <= 0.0f) continue;

            // F4: especular de area — SourceRadius alarga o highlight (representative point);
            // o difuso segue no centro (L) e a energia do lobo e renormalizada.
            float3 Ls;
            float  SpecEnergy = AreaSphereSpecular(Lp.ColorSourceRadius.w, Roughness,
                                                   ToLight, V, N, Ls);
            Lighting += BRDF_DirectArea(N, V, L, Ls, SpecEnergy,
                                        Lp.ColorSourceRadius.rgb * Atten,
                                        DiffuseColor, SpecularColor, Roughness, a2,
                                        TransColor);
        }
        if (LightParams.w > 0.5f)
            Lighting += DirectLocalTex.Load(int3(px, 0)).rgb;
    }

    float3 Ambient = float3(0.0f, 0.0f, 0.0f);

    bool UseGI         = DDGIGridCount.w > 0.5f;
    bool UseReSTIR     = ReflectionParams.w > 0.5f; // ReSTIR GI ativo -> substitui o difuso do DDGI
    bool UseAtmoAmbient = SkyAmbientColor.w > 0.5f;

    if (UseAtmoAmbient && !UseGI) {
        float  hemi       = saturate(N.y * 0.5f + 0.5f);
        float3 ambientCol = lerp(GroundAmbientColor.rgb, SkyAmbientColor.rgb, hemi);
        // Sem (1 - Metallic) aqui: ele ja esta no DiffuseColor (convencao em BRDF.hlsli).
        Ambient += DiffuseColor * ambientCol * AODiffuse * GroundAmbientColor.w;
    }

    if (IBLParams.w > 0.5f) {
        float3 RotN = RotateY(N, IBLParams.y);
        float3 R    = reflect(-V, N);
        float3 RotR = RotateY(R, IBLParams.y);

        float3 F     = F_SchlickRoughness(SpecularColor, NoV, Roughness);
        float3 KdIBL = (1.0f - F); // o (1 - Metallic) vive no DiffuseColor

        float3 Irradiance  = IrradianceMap.SampleLevel(IBLSampler, RotN, 0.0f).rgb;
        float3 DiffuseIBL  = KdIBL * DiffuseColor * Irradiance;

        float  Mip         = Roughness * IBLParams.z;
        float3 Prefiltered = PrefilteredMap.SampleLevel(IBLSampler, RotR, Mip).rgb;
        float2 BRDF        = BRDFLut.SampleLevel(IBLSampler, float2(NoV, Roughness), 0.0f).rg;
        float3 SpecularIBL = Prefiltered * (F * BRDF.x + BRDF.y) * SpecAmbientScale;

        float3 IBLContrib = SpecularIBL * AOSpec;
        if (!(UseGI || UseAtmoAmbient)) IBLContrib += DiffuseIBL * AODiffuse;
        Ambient += IBLContrib * IBLParams.x;
    }

    float3 DbgGI = float3(0.0f, 0.0f, 0.0f);
    if (UseGI || UseReSTIR) {
        float3 gi;
        // Folhagem (two-sided, estilo UE "two-sided foliage"): indireto = frente + 0.6*tras
        // (transmissao pela folha — mesmo fator do TransColor no direto e do SubsurfaceColor
        // do loader). ADITIVO, nao media: hera colada na parede tem o lado -N preto (dentro do
        // predio) e a media derrubava o indireto pela metade. Mantem o Chebyshev do caminho
        // comum: os pisos defensivos (DDGICommon) ja evitam o colapso a preto que motivava
        // pula-lo, e SEM ele a trilinear mistura probes de DENTRO da parede (hera mais escura
        // que a propria parede). Folhagem segue FORA do gather per-pixel do ReSTIR (se auto-
        // oclui em folhagem densa; o Lumen pula screen traces backface pelo mesmo motivo).
        bool foliageFill = IsFoliage && UseGI;
        if (foliageFill) {
            // Fator do lado de tras = intensidade de subsurface do material (era 0.6 fixo —
            // mesmo default do loader). max3 do premul: tint nao escurece o fill, so a cor
            // da transmissao direta.
            float backFill = max(g.Subsurface.r, max(g.Subsurface.g, g.Subsurface.b));
            gi = SampleSceneDDGI(worldPos, N) + backFill * SampleSceneDDGI(worldPos, -N);
        } else if (UseReSTIR) {
            // (Subsurface NAO cai aqui fora: solido segue o gather normal e ganha o termo de
            // tras logo abaixo — so a folhagem pula o ReSTIR, por auto-oclusao das cards.)
            // ReSTIR full-res no A0-A3 (Load por pixel); o A4 passa p/ meia-res + upsample bilateral.
            // ReflectionParams.w == 2 -> a t16 aponta p/ a OUT_DIFF do NRD. O RELAX entrega
            // radiancia LINEAR (o REBLUR entregava YCoCg e exigia desempacotar aqui), entao os
            // dois caminhos — NRD ligado ou ReSTIR cru — sao lidos igual.
            gi = ReSTIRGITex.Load(int3(px, 0)).rgb;
        } else {
            gi = SampleSceneDDGI(worldPos, N);
        }

        // Subsurface (ID 2, solido translucido): caminho normal de GI (ReSTIR/GTAO seguem) +
        // termo do lado de TRAS do DDGI escalado pela intensidade do material — cortina/vela
        // acesa pela luz que chega do outro lado, mesmo raciocinio do fill da folhagem mas
        // SOMADO ao gather frontal em vez de substitui-lo.
        if (g.ShadingModel == SMILE_SHADINGMODEL_SUBSURFACE && UseGI) {
            float backSSS = max(g.Subsurface.r, max(g.Subsurface.g, g.Subsurface.b));
            if (backSSS > 0.0f)
                gi += backSSS * SampleSceneDDGI(worldPos, -N);
        }
        DbgGI = gi;

        // Intensidade do GI: usa o slider do DDGI quando ha grid; senao (ReSTIR sem DDGI) cai em 1.
        float giIntensity = (UseGI && DDGIParams.x > 0.0f) ? DDGIParams.x : 1.0f;
        // idem: metallic ja aplicado no DiffuseColor
        Ambient += DiffuseColor * gi * giIntensity * AODiffuse;

        if (IBLParams.w <= 0.5f) {
            float3 Fa = F_SchlickRoughness(SpecularColor, NoV, Roughness);
            Ambient += Fa * gi * giIntensity * AOSpec * SpecAmbientScale;
        }
    }

    // Emissivo NAO entra aqui: o geometry pass ja o escreveu no SceneColor (SV_Target4 do
    // G-buffer) e este passe SOMA por cima (PSO aditivo). Nos modos de debug (SSAO/GI) o
    // Renderer troca p/ o PSO opaco — o return substitui a tela, como antes da dieta.
    float3 FinalColor = Lighting + Ambient;

    if (DDGIGridCount.w > 1.5f) return float4(DbgGI, 1.0f); 

    if (CSM_DebugEnabled()) {
        int ci = CSM_SelectCascade(worldPos, N);
        if (ci >= 0)
            FinalColor = lerp(FinalColor, CSM_CascadeColor(ci), 0.45f);
    }

    return float4(FinalColor, 1.0f);
}
