#ifndef SMILE_DDGI_COMMON_HLSLI
#define SMILE_DDGI_COMMON_HLSLI

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979323846f
#endif

#ifndef DDGI_IRRADIANCE_GAMMA
#define DDGI_IRRADIANCE_GAMMA 1.5f
#endif

// Instance masks da TLAS (RaytracingScene.cpp seta por instancia; espelhado em
// Smile/Graphics/RTMasks.h). Cada instancia carrega UM bit de categoria, e cada PASSE escolhe
// quais categorias enxerga — o modelo do Lumen (RayTracingDefinitions.h), onde o bit OPAQUE e
// descrito como "used by reflection, shadow, AO and GI tracing passes".
#define SMILE_RT_MASK_OPAQUE      0x01u
#define SMILE_RT_MASK_ALPHATEST   0x02u
// Translucido (material Blend: vidro, vitrine). Sem este bit o vidro entrava na TLAS junto com os
// opacos e, como nenhum RayQuery implementa transmissao, virava PAREDE para o GI — interior de
// ambiente envidracado ficava escuro demais. O raster ja o exclui do G-buffer (passe forward).
#define SMILE_RT_MASK_TRANSLUCENT 0x04u
#define SMILE_RT_MASK_ALL         0xFFu

// Gather difuso e visibilidade do GI: opaco + alpha-test, SEM translucido. Equivale ao
// RAY_TRACING_MASK_OPAQUE do Lumen, que traca o gather com ele sozinho
// (LumenHardwareRayTracingCommon.ush:195). Aproxima transmissao TOTAL pelo vidro — grosseiro, mas
// muito mais perto da fisica que bloquear 100%. Refracao/Fresnel/absorcao ficam p/ depois.
#define SMILE_RT_MASK_GATHER      (SMILE_RT_MASK_OPAQUE | SMILE_RT_MASK_ALPHATEST)

// Flags do InstanceGeo (bitmask em Flags).
#define INSTGEO_FLAG_ALPHATEST 1u  // clip por albedo.a vs AlphaCutoff (folhagem; FORCE_NON_OPAQUE na TLAS)
#define INSTGEO_FLAG_EMISSIVE  2u  // tem mapa emissivo (EmissiveMapIndex valido)
#define INSTGEO_FLAG_FOLIAGE   4u  // ShadingModel Foliage (two-sided + transmissao no PT)
#define INSTGEO_FLAG_MRMAP     8u  // tem mapa metallic-roughness (MrMapIndex valido)
#define INSTGEO_FLAG_SPECPACK  16u // o mapa MR e "Specular" (R=AO, G=rough, B=metal); senao R=metal
#define INSTGEO_FLAG_METALMAP  32u // tem mapa Metalness separado (MetalMapIndex valido; R=metal)
#define INSTGEO_FLAG_ROUGHMAP  64u // tem mapa Roughness separado (RoughMapIndex valido; R=rough)

// 80 bytes — casa campo-a-campo com DDGIInstanceGeo (DDGI.cpp). Campos alem do BaseColor/geometria
// alimentam o ReSTIR PT (emissivo, alpha-test, metal/rough); os shaders antigos ignoram os novos.
// VertexSrv/IndexSrv = indices bindless (ResourceDescriptorHeap) do VB/IB originais do mesh,
// 0-based por mesh — substituem os merged buffers (t4/t5 aposentados).
struct InstanceGeo {
    float4 BaseColor;
    uint   VertexSrv;
    uint   IndexSrv;
    uint   AlbedoIndex;
    uint   HasAlbedo;
    uint   TwoSidedRT;   // = FMaterial::IsTwoSidedForRT: TwoSided OU AlphaTest. NAO e a flag crua
                         // do material — cutout tem so um lado de geometria, entao p/ o RT ele e
                         // two-sided tanto no culling da TLAS quanto aqui.
    uint   Flags;
    float  AlphaCutoff;
    float  RoughnessFactor;
    float4 EmissiveFactor; // rgb = EmissiveFactor * EmissiveStrength; w = MetallicFactor
    uint   EmissiveMapIndex;
    uint   MrMapIndex;
    uint   MetalMapIndex; // mapa Metalness separado (slot +6); valido sob INSTGEO_FLAG_METALMAP
    uint   RoughMapIndex; // mapa Roughness separado (slot +7); valido sob INSTGEO_FLAG_ROUGHMAP
};

struct DDGIVertex {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
};

float3 DDGI_SphericalFibonacci(float i, float n) {
    const float PHI = 1.61803398875f;
    float phi = 2.0f * SMILE_PI * frac(i * (PHI - 1.0f));
    float cosTheta = 1.0f - (2.0f * i + 1.0f) / n;
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float DDGI_Hash(uint x) {
    x = (x ^ 61u) ^ (x >> 16); x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15;
    return (float)(x & 0x00FFFFFFu) / (float)0x01000000;
}

float3x3 DDGI_RandomRotation(uint frame) {
    float a = DDGI_Hash(frame * 3u + 0u) * 6.2831853f;
    float b = DDGI_Hash(frame * 3u + 1u) * 6.2831853f;
    float c = DDGI_Hash(frame * 3u + 2u) * 6.2831853f;
    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b), cc = cos(c), sc = sin(c);
    float3x3 rz = float3x3(ca, -sa, 0, sa, ca, 0, 0, 0, 1);
    float3x3 ry = float3x3(cb, 0, sb, 0, 1, 0, -sb, 0, cb);
    float3x3 rx = float3x3(1, 0, 0, 0, cc, -sc, 0, sc, cc);
    return mul(rz, mul(ry, rx));
}

// Rotacao por frame E por probe (Flax randomiza por-probe via quaternion): com rotacao unica
// por frame todos os probes amostram as MESMAS direcoes e o alias fica sincronizado no grid
// inteiro (flicker estruturado). Trace/Update/UpdateDist/Relocate do mesmo frame tem que passar
// o MESMO (frame, probeIdx) p/ reconstruir as direcoes tracadas.
float3 DDGI_RayDirection(int rayIdx, int numRays, uint frame, uint probeIdx) {
    float3x3 rot = DDGI_RandomRotation(frame + probeIdx * 2654435761u);
    return normalize(mul(rot, DDGI_SphericalFibonacci((float)rayIdx, (float)numRays)));
}

float2 DDGI_OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 o = (n.z >= 0.0f) ? n.xy : ((1.0f - abs(n.yx)) * sign(n.xy));
    return o; 
}

float3 DDGI_OctDecode(float2 f) {
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    return normalize(n);
}

int  DDGI_ProbeLinear(int3 c, int3 count) { return c.x + c.y * count.x + c.z * count.x * count.y; }

int3 DDGI_ProbeCoord(int idx, int3 count) {
    int xy = count.x * count.y;
    int z  = idx / xy;
    int r  = idx - z * xy;
    int y  = r / count.x;
    int x  = r - y * count.x;
    return int3(x, y, z);
}

// Origem do INTERIOR do tile: cada tile tem 1px de borda octaedrica de cada lado (stride =
// tile+2, como paper/RTXGI/Flax) — a borda recebe copia com o fold do octaedro (tabelas nos
// passes de update) p/ o bilinear ser continuo na costura em vez do clamp achatar o anel externo.
int2 DDGI_TileOrigin(int3 c, int3 count, int tile) {
    int tileCol = c.x + c.z * count.x;
    int tileRow = c.y;
    int stride  = tile + 2;
    return int2(tileCol * stride, tileRow * stride) + 1;
}

float3 DDGI_ProbeWorldPos(int3 c, float3 gridMin, float spacing) {
    return gridMin + (float3)c * spacing;
}

float3 DDGI_SampleProbe(Texture2D<float4> atlas, SamplerState samp, int2 tileOriginPx,
                        int tile, float2 atlasInvSize, float3 dir) {
    float2 oct = DDGI_OctEncode(dir) * 0.5f + 0.5f;
    float2 px  = (float2)tileOriginPx + oct * tile; // sem clamp: bilinear cruza p/ a borda copiada
    float3 s   = atlas.SampleLevel(samp, px * atlasInvSize, 0.0f).rgb;
    return pow(max(s, 0.0f), DDGI_IRRADIANCE_GAMMA);
}

float3 SampleDDGIIrradiance(Texture2D<float4> atlas, SamplerState samp, float3 worldPos,
                            float3 N, float3 gridMin, float spacing, int3 count,
                            int tile, float2 atlasInvSize) {
    float3 g    = (worldPos - gridMin) / spacing;
    int3   base = (int3)floor(g);
    float3 frac = saturate(g - (float3)base);

    float3 sum  = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);

        float3 tw = lerp(1.0f - frac, frac, (float3)off);
        float  w  = tw.x * tw.y * tw.z;
        if (w <= 0.0f) continue;

        int2   tileOrigin = DDGI_TileOrigin(c, count, tile);
        float3 irr = DDGI_SampleProbe(atlas, samp, tileOrigin, tile, atlasInvSize, N);
        sum  += irr * w;
        wsum += w;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

float2 DDGI_SampleProbeRG(Texture2D<float4> distAtlas, SamplerState samp, int2 tileOriginPx,
                          int tile, float2 atlasInvSize, float3 dir) {
    float2 oct = DDGI_OctEncode(dir) * 0.5f + 0.5f;
    float2 px  = (float2)tileOriginPx + oct * tile; // sem clamp: bilinear cruza p/ a borda copiada
    return distAtlas.SampleLevel(samp, px * atlasInvSize, 0.0f).rg;
}

// Self-shadow bias do paper (e do Flax, GetDDGISurfaceBias): desloca o ponto de amostragem na
// normal E na direcao da camera — o componente de view e o que evita dark banding/shadow leak
// em parede vista de raspao, onde bias so-normal nao tira o ponto da zona de auto-oclusao.
//
// `scale` = o `bias` do Flax (0.2 = legado). `maxMeters` = TETO ABSOLUTO em metros; 0 desliga o
// teto e reproduz o comportamento historico bit a bit.
//
// Por que o teto existe: a formula original escala com o espacamento do grid, e o grid daqui e
// dimensionado pela AABB da cena inteira (DDGI.cpp: spacing = maxExt/23). No Bistro isso da
// spacing = 8,02 m medido, ou seja 0.75*8.02*0.2 = 1,20 m de deslocamento — o ponto de
// amostragem atravessa parede e le a celula do outro lado. O Flax nao sofre disso porque a
// cascata mais fina tem spacing de ~1 m; o RTXGI resolveu tornando normalBias/viewBias
// absolutos (metros), independentes do grid. O teto e a versao barata dessa correcao: preserva
// o comportamento em cena pequena e corta o absurdo em cena grande.
float3 DDGI_SurfaceBias(float3 N, float3 V, float spacing, float scale, float maxMeters) {
    float s = 0.75f * spacing * scale;
    if (maxMeters > 0.0f) s = min(s, maxMeters);
    return (N * 0.2f + V * 0.8f) * s;
}

// Peso do volume: 1 em TODO ponto dentro do volume, caindo a 0 ao longo de `fadeProbes` celulas
// DEPOIS da borda, do lado de fora. `fadeProbes = 0` desliga (1 em todo lugar = historico).
//
// Por que existe: o gather CLAMPA as coordenadas do grid, entao um ponto fora do volume nao
// falha — ele le as probes da borda e as estende ao infinito. O terreno fica fora de proposito
// (SceneLoader: um terreno de km esticaria o grid), e o deferred desliga o IBL difuso quando o
// GI esta ligado, entao hoje o terreno inteiro recebe a irradiancia da ultima fileira de probes.
// O Flax trata isso com FallbackIrradiance e um peso de cascata que cai na borda
// (DDGI.hlsl:315-345); aqui, com um volume so, o fallback e o ambiente hemisferico da atmosfera,
// que e exatamente o que o deferred usaria se o GI estivesse desligado.
//
// O fade e para FORA, e nao para dentro como no Flax, porque os volumes sao diferentes: la ele e
// folgado e centrado na camera, entao desvanecer nas bordas internas nao custa nada. Aqui ele e
// justo (AABB da cena + meia celula), entao TODA a geometria fica perto de alguma face — o chao
// nasce a meia celula da face inferior. Fade para dentro lavaria o piso inteiro com ambiente
// hemisferico, que e pior que o problema original.
float DDGI_VolumeWeight(float3 worldPos, float3 gridMin, float spacing, int3 count,
                        float fadeProbes) {
    if (fadeProbes <= 0.0f) return 1.0f;
    // Meia celula de margem dos DOIS lados. Nao e folga arbitraria: o grid e ancorado em
    // AABBMin - 0.5*spacing e o numero de probes e ceil(extensao/spacing) + 1, entao a ultima
    // fileira cai em AABBMax - 0.5*spacing quando a extensao e multipla do espacamento — que e
    // exatamente o caso do eixo dominante (spacing = maxExt/23, count = 24). Sem a margem, os
    // ultimos ~4 m da PROPRIA cena entrariam como "fora" e a face AABBMax sairia com peso 0,5.
    // A mesma margem do lado negativo mantem o teste simetrico; la nao ha cena mesmo.
    float  margin  = 0.5f * spacing;
    float3 gridMax = gridMin + (float3)(count - 1) * spacing;
    float3 inside  = min(worldPos - (gridMin - margin), (gridMax + margin) - worldPos);
    float  d       = min(inside.x, min(inside.y, inside.z)); // <0 = fora daquela face
    return saturate(1.0f + min(d, 0.0f) / (fadeProbes * spacing));
}

// Um tap do gather com Chebyshev: tudo que decide o peso de UMA das 8 probes da celula.
// SampleDDGIIrradianceCheb consome so o Weight; o diagnostico pontual (DDGIDebugPoint.cs)
// publica os intermediarios. Os dois passam por DDGI_EvaluateTapCheb — mexer no peso sem
// mexer no diagnostico deixaria a ferramenta MENTINDO, que e pior que nao ter ferramenta.
struct DDGITapCheb {
    int3  Coord;        // pode diferir de base+off: o fallback procura vizinho ativo
    uint  Index;        // linear de Coord
    bool  Ignored;      // probe inativa e sem substituta -> nao contribui
    float DistToProbe;
    float Mean;         // 1o momento na direcao probe->ponto
    float Mean2;        // 2o momento (sigma sai daqui, so o debug usa)
    float Trilinear;    // ja com o piso de 0.001
    float Visibility;   // Chebyshev^3 com piso; 1.0 quando nao ha oclusao a testar
    float Weight;       // final: backface * visibilidade * crush * trilinear
};

DDGITapCheb DDGI_EvaluateTapCheb(
        int i, int3 base, float3 frac, float3 biasPos, float3 rawPos, float3 N,
        float3 gridMin, float spacing, int3 count,
        Texture2D<float4> distAtlas, SamplerState samp, int distTile, float2 distInvSize,
        Buffer<float4> probeData, uint skipMode) {
    int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);

    DDGITapCheb tap;
    tap.Ignored    = false;
    tap.Visibility = 1.0f;

    uint index = (uint)DDGI_ProbeLinear(c, count);
    if (skipMode != 0u && probeData[index].w < 0.0f) {
        if (skipMode >= 2u) {
            const int3 axisMask[3] = { int3(1,0,0), int3(0,1,0), int3(0,0,1) };
            bool found = false;
            [loop] for (int sd = 1; sd < 3 && !found; ++sd) {
                for (int ax = 0; ax < 3; ++ax) {
                    int  dir = (off[ax] != 0) ? 1 : -1;
                    int3 sc  = clamp(c + axisMask[ax] * (dir * sd), int3(0, 0, 0), count - 1);
                    uint candidate = (uint)DDGI_ProbeLinear(sc, count);
                    if (probeData[candidate].w >= 0.0f) {
                        c = sc; index = candidate; found = true; break;
                    }
                }
            }
            tap.Ignored = !found;
        } else {
            tap.Ignored = true;
        }
    }
    tap.Coord = c;
    tap.Index = index;

    // Posicao REAL do probe (grid + offset de relocacao): o trace dispara os raios e o
    // dist atlas mede distancias a partir do probe relocado — o Chebyshev e o peso de
    // backface tem que usar a mesma origem, senao o teste de visibilidade compara contra
    // a posicao errada exatamente nos probes que foram movidos por estar perto de parede.
    float3 probePos     = DDGI_ProbeWorldPos(c, gridMin, spacing) + probeData[index].xyz;
    float3 probeToPoint = biasPos - probePos;
    tap.DistToProbe     = length(probeToPoint);
    float3 dirPP        = probeToPoint / max(tap.DistToProbe, 1e-4f);

    // Direcao do teste de backface: da posicao SEM bias, como o Flax (DDGI.hlsl:210-215). O
    // teste pergunta de que lado da superficie REAL a probe esta, e o bias e um deslocamento
    // artificial que, quando grande, muda a resposta — com 1,2 m o ponto viesado ja podia estar
    // do outro lado da parede, e ai a probe de la parecia "de frente". A distancia/Chebyshev
    // continuam medidos do ponto viesado, que e a separacao que o Flax faz.
    float3 rawToProbe  = rawPos - probePos;
    float3 dirBackface = rawToProbe / max(length(rawToProbe), 1e-4f);

    // Pesos DEFENSIVOS com piso (receita do Flax, DDGI.hlsl): backface e Chebyshev se
    // auto-sabotam em geometria densa/fina (miolo de sebe, cantos, frestas) — os pisos
    // garantem que wsum nunca colapsa a zero => nunca retorna preto absoluto, so escurece.
    float backface = dot(-dirBackface, N) * 0.5f + 0.5f;
    float w = backface * backface + 0.05f;

    int2   distOrigin = DDGI_TileOrigin(c, count, distTile);
    float2 md = DDGI_SampleProbeRG(distAtlas, samp, distOrigin, distTile, distInvSize, dirPP);
    tap.Mean  = md.x;
    tap.Mean2 = md.y;
    if (tap.DistToProbe > tap.Mean) {
        float variance = abs(tap.Mean * tap.Mean - tap.Mean2);
        float d        = tap.DistToProbe - tap.Mean;
        float cheb     = variance / max(variance + d * d, 1e-8f);
        tap.Visibility = max(cheb * cheb * cheb, 0.05f);
        w *= tap.Visibility;
    }
    w = max(w, 1e-6f);

    // Curva de crush suave p/ pesos baixos ("inject a small portion of light"): mantem a
    // penalizacao dos probes ocluidos sem a transicao dura do corte a zero.
    const float minWeightThreshold = 0.2f;
    if (w < minWeightThreshold)
        w *= (w * w) / (minWeightThreshold * minWeightThreshold);

    float3 trilin = lerp(1.0f - frac, frac, (float3)off);
    tap.Trilinear = max(trilin.x * trilin.y * trilin.z, 0.001f);
    w *= tap.Trilinear;

    tap.Weight = tap.Ignored ? 0.0f : w;
    return tap;
}

float3 SampleDDGIIrradianceCheb(
        Texture2D<float4> irrAtlas, Texture2D<float4> distAtlas, SamplerState samp,
        float3 worldPos, float3 N, float3 gridMin, float spacing, int3 count,
        int irrTile, float2 irrInvSize, int distTile, float2 distInvSize, float3 biasVec,
        Buffer<float4> probeData, uint skipMode) {
    float3 biasPos = worldPos + biasVec;
    float3 g    = (biasPos - gridMin) / spacing;
    int3   base = (int3)floor(g);
    float3 frac = saturate(g - (float3)base);

    float3 sum  = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        DDGITapCheb tap = DDGI_EvaluateTapCheb(i, base, frac, biasPos, worldPos, N,
                                               gridMin, spacing, count, distAtlas, samp,
                                               distTile, distInvSize, probeData, skipMode);
        if (tap.Ignored) continue;

        int2   irrOrigin = DDGI_TileOrigin(tap.Coord, count, irrTile);
        float3 irr = DDGI_SampleProbe(irrAtlas, samp, irrOrigin, irrTile, irrInvSize, N);
        sum  += irr * tap.Weight;
        wsum += tap.Weight;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

#endif 
