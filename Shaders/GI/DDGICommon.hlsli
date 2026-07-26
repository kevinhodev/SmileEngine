#ifndef SMILE_DDGI_COMMON_HLSLI
#define SMILE_DDGI_COMMON_HLSLI

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979323846f
#endif

#ifndef DDGI_IRRADIANCE_GAMMA
#define DDGI_IRRADIANCE_GAMMA 1.5f
#endif

// Instance masks da TLAS (RaytracingScene.cpp seta por instancia). Raios normais usam ALL;
// shadow rays podem usar so OPAQUE p/ pular folhagem/alpha-test (modo rapido, toggle no editor).
#define SMILE_RT_MASK_OPAQUE    0x01u
#define SMILE_RT_MASK_ALPHATEST 0x02u
#define SMILE_RT_MASK_ALL       0xFFu

// Flags do InstanceGeo (bitmask em Flags).
#define INSTGEO_FLAG_ALPHATEST 1u  // clip por albedo.a vs AlphaCutoff (folhagem; FORCE_NON_OPAQUE na TLAS)
#define INSTGEO_FLAG_EMISSIVE  2u  // tem mapa emissivo (EmissiveMapIndex valido)
#define INSTGEO_FLAG_FOLIAGE   4u  // ShadingModel Foliage (two-sided + transmissao no PT)
#define INSTGEO_FLAG_MRMAP     8u  // tem mapa metallic-roughness (MrMapIndex valido; G=rough, B=metal)

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
    uint   GeoPad0; uint GeoPad1;
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
float3 DDGI_SurfaceBias(float3 N, float3 V, float spacing) {
    return (N * 0.2f + V * 0.8f) * (0.75f * spacing * 0.2f);
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
        int i, int3 base, float3 frac, float3 biasPos, float3 N,
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

    // Pesos DEFENSIVOS com piso (receita do Flax, DDGI.hlsl): backface e Chebyshev se
    // auto-sabotam em geometria densa/fina (miolo de sebe, cantos, frestas) — os pisos
    // garantem que wsum nunca colapsa a zero => nunca retorna preto absoluto, so escurece.
    float backface = dot(-dirPP, N) * 0.5f + 0.5f;
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
        DDGITapCheb tap = DDGI_EvaluateTapCheb(i, base, frac, biasPos, N, gridMin, spacing,
                                               count, distAtlas, samp, distTile, distInvSize,
                                               probeData, skipMode);
        if (tap.Ignored) continue;

        int2   irrOrigin = DDGI_TileOrigin(tap.Coord, count, irrTile);
        float3 irr = DDGI_SampleProbe(irrAtlas, samp, irrOrigin, irrTile, irrInvSize, N);
        sum  += irr * tap.Weight;
        wsum += tap.Weight;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

#endif 
