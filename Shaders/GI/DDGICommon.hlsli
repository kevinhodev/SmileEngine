#ifndef SMILE_DDGI_COMMON_HLSLI
#define SMILE_DDGI_COMMON_HLSLI

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979323846f
#endif

#ifndef DDGI_IRRADIANCE_GAMMA
#define DDGI_IRRADIANCE_GAMMA 1.5f
#endif

// Espelhado em Smile/Graphics/RayTracing/RTMasks.h.
#define SMILE_RT_MASK_OPAQUE      0x01u
#define SMILE_RT_MASK_ALPHATEST   0x02u
#define SMILE_RT_MASK_TRANSLUCENT 0x04u
#define SMILE_RT_MASK_ALL         0xFFu

// O gather difuso ignora translucidos; transmissao nao e modelada pelos RayQuery.
#define SMILE_RT_MASK_GATHER      (SMILE_RT_MASK_OPAQUE | SMILE_RT_MASK_ALPHATEST)

#define INSTGEO_FLAG_ALPHATEST 1u  // clip por albedo.a vs AlphaCutoff (folhagem; FORCE_NON_OPAQUE na TLAS)
#define INSTGEO_FLAG_EMISSIVE  2u  // tem mapa emissivo (EmissiveMapIndex valido)
#define INSTGEO_FLAG_FOLIAGE   4u  // ShadingModel Foliage (two-sided + transmissao no PT)
#define INSTGEO_FLAG_MRMAP     8u  // tem mapa metallic-roughness (MrMapIndex valido)
#define INSTGEO_FLAG_SPECPACK  16u // o mapa MR e "Specular" (R=AO, G=rough, B=metal); senao R=metal
#define INSTGEO_FLAG_METALMAP  32u // tem mapa Metalness separado (MetalMapIndex valido; R=metal)
#define INSTGEO_FLAG_ROUGHMAP  64u // tem mapa Roughness separado (RoughMapIndex valido; R=rough)
#define INSTGEO_FLAG_TRANSLUCENT 128u // material Blend; usado pelo BvhDebug

// Layout de 84 bytes espelhado por FRTInstanceGeo.
struct InstanceGeo {
    float4 BaseColor;
    uint   VertexSrv;
    uint   IndexSrv;
    uint   TriangleSrv;
    uint   AlbedoIndex;
    uint   HasAlbedo;
    uint   TwoSidedRT;   // TwoSided ou AlphaTest para o RT
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

// Payload pre-cozido espelhado por FRTTriangle; exige bump de kCookedVersion ao mudar.
struct RTTriangle {
    uint FaceNormalOct;      // octaedrica SNORM16x2, espaco LOCAL
    uint VertexNormalOct[3]; // idem, por vertice do triangulo
    uint UV[3];              // half2 por vertice
    uint Flags;              // bit 0 = normal de face valida (RTTRI_FLAG_FACE_NORMAL_VALID)
};

#define RTTRI_FLAG_FACE_NORMAL_VALID 0x1u

float RT_UnpackSnorm16(uint _Packed) {
    int S = (int)(_Packed & 0xFFFFu);
    if (S > 32767) S -= 65536;
    return max((float)S / 32767.0f, -1.0f);
}

// Espelho de Smile::RTOctEncodeSnorm16.
float3 RT_OctDecode(uint _Enc) {
    float X = RT_UnpackSnorm16(_Enc & 0xFFFFu);
    float Y = RT_UnpackSnorm16(_Enc >> 16);
    float3 N = float3(X, Y, 1.0f - abs(X) - abs(Y));
    float  T = saturate(-N.z);
    N.x += (N.x >= 0.0f) ? -T : T;
    N.y += (N.y >= 0.0f) ? -T : T;
    return normalize(N);
}

float2 RT_UnpackHalf2(uint _Packed) {
    return float2(f16tof32(_Packed & 0xFFFFu), f16tof32(_Packed >> 16));
}

// Acesso bindless ao payload fica em RTTriangleAccess.hlsli; este header tambem serve ao raster.

float3 RT_InterpolatedNormalObj(RTTriangle _T, float2 _Bary) {
    return RT_OctDecode(_T.VertexNormalOct[0]) * (1.0f - _Bary.x - _Bary.y)
         + RT_OctDecode(_T.VertexNormalOct[1]) * _Bary.x
         + RT_OctDecode(_T.VertexNormalOct[2]) * _Bary.y;
}

float2 RT_InterpolatedUV(RTTriangle _T, float2 _Bary) {
    return RT_UnpackHalf2(_T.UV[0]) * (1.0f - _Bary.x - _Bary.y)
         + RT_UnpackHalf2(_T.UV[1]) * _Bary.x
         + RT_UnpackHalf2(_T.UV[2]) * _Bary.y;
}

// Retorna false para triangulo degenerado ou transformacao singular.
bool RT_FaceNormal(RTTriangle _T, float3x4 _WorldToObject, out float3 _OutFaceN) {
    if ((_T.Flags & RTTRI_FLAG_FACE_NORMAL_VALID) == 0u) {
        _OutFaceN = float3(0.0f, 0.0f, 1.0f);
        return false;
    }
    float3 NObj  = RT_OctDecode(_T.FaceNormalOct);
    float3 NWrld = mul(NObj, (float3x3)_WorldToObject);
    float  NLen  = length(NWrld);
    _OutFaceN = (NLen > 0.0f) ? (NWrld / NLen) : float3(0.0f, 0.0f, 1.0f);
    return NLen > 0.0f;
}

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

// Todos os passes devem usar o mesmo par (frame, probeIdx).
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

// Conversoes inversas entre coordenadas geometricas e armazenamento toroidal.
int3 DDGI_StorageCoord(int3 c, int3 scroll, int3 count) {
    int3 s = c + scroll;
    return s - count * (int3)(s >= count);
}
int3 DDGI_GeometricCoord(int3 s, int3 scroll, int3 count) {
    int3 c = s - scroll;
    return c + count * (int3)(c < 0);
}

// Detecta em inteiro se o slot representava outro ponto no ultimo update.
bool DDGI_NewlyExposed(int3 cGeo, int3 scrollDelta, int3 count) {
    int3 old = cGeo + scrollDelta;
    return any(old < 0) || any(old >= count);
}

// Espelhado por FDDGI::kTraceProbesPerRow.
#define DDGI_TRACE_PROBES_PER_ROW 256

// Espelhado por FDDGI::kDispatchGroupsX.
#define DDGI_DISPATCH_GROUPS_X 1024

int DDGI_DispatchGroupsX(int numProbes) {
    int rows = max((numProbes + DDGI_DISPATCH_GROUPS_X - 1) / DDGI_DISPATCH_GROUPS_X, 1);
    return max((numProbes + rows - 1) / rows, 1);
}

int DDGI_ProbeFromGroup(uint2 gid, int numProbes) {
    return (int)gid.x + (int)gid.y * DDGI_DispatchGroupsX(numProbes);
}

int2 DDGI_TraceTexel(int probeIdx, int rayIdx, int numProbes, int raysPerProbe) {
    int perRow = min(numProbes, DDGI_TRACE_PROBES_PER_ROW);
    perRow     = max(perRow, 1);
    return int2((probeIdx % perRow) * raysPerProbe + rayIdx, probeIdx / perRow);
}

int3 DDGI_ProbeCoord(int idx, int3 count) {
    int xy = count.x * count.y;
    int z  = idx / xy;
    int r  = idx - z * xy;
    int y  = r / count.x;
    int x  = r - y * count.x;
    return int3(x, y, z);
}

// Derivado da largura para evitar estado duplicado no cbuffer.
int DDGI_TilesPerRow(float atlasW, int tile) {
    return max((int)atlasW / (tile + 2), 1);
}

// O atlas usa bandas XZ e empilha cascatas em linhas, preservando a vizinhanca 2x2 do gather.
int DDGI_TileRowsPerCascade(int3 count, int tilesPerRow) {
    int zRowsPerBand = max(tilesPerRow / max(count.x, 1), 1);
    int bands        = (count.z + zRowsPerBand - 1) / zRowsPerBand;
    return bands * count.y;
}

// cGeo e geometrica; a funcao aplica o scroll antes de enderecar o atlas.
int2 DDGI_TileOrigin(int3 cGeo, int3 scroll, int3 count, int tile, int tilesPerRow, int cascade) {
    int3 c     = DDGI_StorageCoord(cGeo, scroll, count);
    int plane  = c.x + c.z * count.x;
    int band   = plane / tilesPerRow;
    int col    = plane - band * tilesPerRow;
    int row    = c.y + band * count.y + cascade * DDGI_TileRowsPerCascade(count, tilesPerRow);
    int stride = tile + 2;
    return int2(col * stride, row * stride) + 1;
}

// Buffers usam indice global; geometria e vizinhanca usam indice local da cascata.
int  DDGI_ProbesPerCascade(int3 count) { return count.x * count.y * count.z; }
int  DDGI_CascadeOfProbe(int globalIdx, int3 count) {
    return globalIdx / max(DDGI_ProbesPerCascade(count), 1);
}
int  DDGI_LocalProbeIndex(int globalIdx, int3 count) {
    int per = max(DDGI_ProbesPerCascade(count), 1);
    return globalIdx - (globalIdx / per) * per;
}
int  DDGI_GlobalProbeIndex(int localIdx, int cascade, int3 count) {
    return localIdx + cascade * DDGI_ProbesPerCascade(count);
}
int  DDGI_GlobalProbeFromGeo(int3 cGeo, int3 scroll, int cascade, int3 count) {
    return DDGI_GlobalProbeIndex(DDGI_ProbeLinear(DDGI_StorageCoord(cGeo, scroll, count), count),
                                 cascade, count);
}

#define DDGI_MAX_CASCADES 4
// Largura do blend fino->grosso em celulas da cascata de origem.
#define DDGI_CASCADE_BLEND_CELLS 2.5f

// Mede a posicao crua para o bias nao deslocar a selecao entre cascatas.
float DDGI_CascadeWeight(float3 rawPos, float3 gridMin, float spacing, int3 count) {
    float3 gridMax = gridMin + (float3)(count - 1) * spacing;
    float  margin  = 0.5f * spacing;
    float3 inside  = min(rawPos - (gridMin - margin), (gridMax + margin) - rawPos);
    float  d       = min(inside.x, min(inside.y, inside.z));
    return saturate(d / max(DDGI_CASCADE_BLEND_CELLS * spacing, 1e-4f));
}

// Next == Primary indica que nao ha segundo gather.
struct DDGICascadeChoice {
    int   Primary;
    int   Next;
    float PrimaryWeight;
};

// Escolhe a cascata mais fina; a grossa nao desvanece e serve de fallback.
DDGICascadeChoice DDGI_SelectCascade(float3 rawPos, float4 cascades[DDGI_MAX_CASCADES],
                                     int cascadeCount, int3 count) {
    DDGICascadeChoice ch;
    const int coarse = max(cascadeCount - 1, 0);
    for (int c = 0; c < coarse; ++c) {
        float w = DDGI_CascadeWeight(rawPos, cascades[c].xyz, cascades[c].w, count);
        if (w > 0.0f) {
            ch.Primary = c; ch.Next = c + 1; ch.PrimaryWeight = w;
            return ch;
        }
    }
    ch.Primary = coarse; ch.Next = coarse; ch.PrimaryWeight = 1.0f;
    return ch;
}

float3 DDGI_ProbeWorldPos(int3 c, float3 gridMin, float spacing) {
    return gridMin + (float3)c * spacing;
}

// Ambos os atlas testam a mesma posicao relocada, mas usam histereses proprias.
float DDGI_RegionalHysteresis(int3 probeCoord, float3 probeOffset, float3 gridMin, float spacing,
                              float4 invMin, float3 invMax, float regionalHyst, float hyst) {
    if (invMin.w <= 0.5f) return hyst;
    float3 probePos = DDGI_ProbeWorldPos(probeCoord, gridMin, spacing) + probeOffset;
    if (all(probePos >= invMin.xyz) && all(probePos <= invMax))
        return min(hyst, regionalHyst);
    return hyst;
}

// Mudanca relativa simetrica com banda morta; resultado em [0,1].
float DDGI_RelChange3(float3 a, float3 b, float floorV) {
    float3 d = max(abs(a - b) - floorV, 0.0f);
    float3 m = max(max(abs(a), abs(b)), floorV);
    float3 r = d / m;
    return max(r.x, max(r.y, r.z));
}

// A curva apenas reduz a histerese e fica acima do ruido comum do estimador.
float DDGI_AdaptiveHysteresis(float rel, float hyst) {
    const float kRelModerate  = 0.50f;
    const float kRelBig       = 0.80f;
    const float kHystModerate = 0.90f;
    const float kHystBig      = 0.50f;
    if (rel <= kRelModerate) return hyst;
    float t = saturate((rel - kRelModerate) / (kRelBig - kRelModerate));
    return min(hyst, lerp(kHystModerate, kHystBig, t));
}

float3 DDGI_SampleProbe(Texture2D<float4> atlas, SamplerState samp, int2 tileOriginPx,
                        int tile, float2 atlasInvSize, float3 dir) {
    float2 oct = DDGI_OctEncode(dir) * 0.5f + 0.5f;
    float2 px  = (float2)tileOriginPx + oct * tile; // a borda copiada recebe o bilinear
    float3 s   = atlas.SampleLevel(samp, px * atlasInvSize, 0.0f).rgb;
    return pow(max(s, 0.0f), DDGI_IRRADIANCE_GAMMA);
}

float3 SampleDDGIIrradiance(Texture2D<float4> atlas, SamplerState samp, float3 worldPos,
                            float3 N, float3 gridMin, float spacing, int3 count,
                            int tile, float2 atlasInvSize, int tilesPerRow, int cascade,
                            int3 scroll) {
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

        int2   tileOrigin = DDGI_TileOrigin(c, scroll, count, tile, tilesPerRow, cascade);
        float3 irr = DDGI_SampleProbe(atlas, samp, tileOrigin, tile, atlasInvSize, N);
        sum  += irr * w;
        wsum += w;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

float2 DDGI_SampleProbeRG(Texture2D<float4> distAtlas, SamplerState samp, int2 tileOriginPx,
                          int tile, float2 atlasInvSize, float3 dir) {
    float2 oct = DDGI_OctEncode(dir) * 0.5f + 0.5f;
    float2 px  = (float2)tileOriginPx + oct * tile;
    return distAtlas.SampleLevel(samp, px * atlasInvSize, 0.0f).rg;
}

// Bias de auto-sombra na normal e na vista; maxMeters = 0 remove o teto absoluto.
float3 DDGI_SurfaceBias(float3 N, float3 V, float spacing, float scale, float maxMeters) {
    float s = 0.75f * spacing * scale;
    if (maxMeters > 0.0f) s = min(s, maxMeters);
    return (N * 0.2f + V * 0.8f) * s;
}

// Peso 1 dentro da gaiola e fade externo em fadeProbes celulas; 0 desativa.
float DDGI_VolumeWeight(float3 worldPos, float3 gridMin, float spacing, int3 count,
                        float fadeProbes) {
    if (fadeProbes <= 0.0f) return 1.0f;
    float  margin  = 0.5f * spacing;
    float3 gridMax = gridMin + (float3)(count - 1) * spacing;
    float3 inside  = min(worldPos - (gridMin - margin), (gridMax + margin) - worldPos);
    float  d       = min(inside.x, min(inside.y, inside.z));
    return saturate(1.0f + min(d, 0.0f) / (fadeProbes * spacing));
}

// Contrato compartilhado pelo gather e pelo diagnostico pontual.
struct DDGITapCheb {
    int3  Coord;
    uint  Index;
    bool  Ignored;
    float DistToProbe;
    float Mean;
    float Mean2;
    float Trilinear;
    float Visibility;
    float Weight;
};

DDGITapCheb DDGI_EvaluateTapCheb(
        int i, int3 base, float3 frac, float3 biasPos, float3 rawPos, float3 N,
        float3 gridMin, float spacing, int3 count,
        Texture2D<float4> distAtlas, SamplerState samp, int distTile, float2 distInvSize,
        Buffer<float4> probeData, uint skipMode, int tilesPerRow, int cascade, int3 scroll) {
    int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    // Grampeia a geometria antes de converter para o armazenamento toroidal.
    int3 c   = clamp(base + off, int3(0, 0, 0), count - 1);

    DDGITapCheb tap;
    tap.Ignored    = false;
    tap.Visibility = 1.0f;

    uint index = (uint)DDGI_GlobalProbeFromGeo(c, scroll, cascade, count);
    if (skipMode != 0u && probeData[index].w < 0.0f) {
        if (skipMode >= 2u) {
            const int3 axisMask[3] = { int3(1,0,0), int3(0,1,0), int3(0,0,1) };
            bool found = false;
            [loop] for (int sd = 1; sd < 3 && !found; ++sd) {
                for (int ax = 0; ax < 3; ++ax) {
                    int  dir = (off[ax] != 0) ? 1 : -1;
                    int3 sc  = clamp(c + axisMask[ax] * (dir * sd), int3(0, 0, 0), count - 1);
                    uint candidate = (uint)DDGI_GlobalProbeFromGeo(sc, scroll, cascade, count);
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

    // Chebyshev e backface usam a mesma origem relocada do trace.
    float3 probePos     = DDGI_ProbeWorldPos(c, gridMin, spacing) + probeData[index].xyz;
    float3 probeToPoint = biasPos - probePos;
    tap.DistToProbe     = length(probeToPoint);
    float3 dirPP        = probeToPoint / max(tap.DistToProbe, 1e-4f);

    // Backface usa a superficie crua; distancia e Chebyshev usam o ponto viesado.
    float3 rawToProbe  = rawPos - probePos;
    float3 dirBackface = rawToProbe / max(length(rawToProbe), 1e-4f);

    // Pisos defensivos impedem que todos os oito pesos colapsem em geometria densa.
    float backface = dot(-dirBackface, N) * 0.5f + 0.5f;
    float w = backface * backface + 0.05f;

    int2   distOrigin = DDGI_TileOrigin(c, scroll, count, distTile, tilesPerRow, cascade);
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

    // Atenua pesos baixos sem corte abrupto.
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
        Buffer<float4> probeData, uint skipMode, int tilesPerRow, int cascade, int3 scroll) {
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
                                               distTile, distInvSize, probeData, skipMode,
                                               tilesPerRow, cascade, scroll);
        if (tap.Ignored) continue;

        int2   irrOrigin = DDGI_TileOrigin(tap.Coord, scroll, count, irrTile, tilesPerRow, cascade);
        float3 irr = DDGI_SampleProbe(irrAtlas, samp, irrOrigin, irrTile, irrInvSize, N);
        sum  += irr * tap.Weight;
        wsum += tap.Weight;
    }
    return (wsum > 0.0f) ? (sum / wsum) : float3(0.0f, 0.0f, 0.0f);
}

// Wrappers centrais de selecao e blend; fallback externo continua a cargo do chamador.
float3 SampleDDGIIrradianceCascaded(
        Texture2D<float4> atlas, SamplerState samp, float3 rawPos, float3 N,
        float4 cascades[DDGI_MAX_CASCADES], float4 scrolls[DDGI_MAX_CASCADES],
        int cascadeCount, int3 count,
        int tile, float2 atlasInvSize, int tilesPerRow) {
    DDGICascadeChoice ch = DDGI_SelectCascade(rawPos, cascades, cascadeCount, count);
    float3 primary = SampleDDGIIrradiance(atlas, samp, rawPos, N,
                                          cascades[ch.Primary].xyz, cascades[ch.Primary].w,
                                          count, tile, atlasInvSize, tilesPerRow, ch.Primary,
                                          (int3)scrolls[ch.Primary].xyz);
    if (ch.Next == ch.Primary || ch.PrimaryWeight >= 0.999f) return primary;

    float3 next = SampleDDGIIrradiance(atlas, samp, rawPos, N,
                                       cascades[ch.Next].xyz, cascades[ch.Next].w,
                                       count, tile, atlasInvSize, tilesPerRow, ch.Next,
                                       (int3)scrolls[ch.Next].xyz);
    return lerp(next, primary, ch.PrimaryWeight);
}

// Recalcula o bias por cascata porque ele depende do espacamento selecionado.
float3 SampleDDGIIrradianceChebCascaded(
        Texture2D<float4> irrAtlas, Texture2D<float4> distAtlas, SamplerState samp,
        float3 rawPos, float3 N, float3 V,
        float4 cascades[DDGI_MAX_CASCADES], float4 scrolls[DDGI_MAX_CASCADES],
        int cascadeCount, int3 count,
        int irrTile, float2 irrInvSize, int distTile, float2 distInvSize,
        Buffer<float4> probeData, uint skipMode, int tilesPerRow,
        float biasScale, float biasMax) {
    DDGICascadeChoice ch = DDGI_SelectCascade(rawPos, cascades, cascadeCount, count);

    float3 primary = SampleDDGIIrradianceCheb(
        irrAtlas, distAtlas, samp, rawPos, N,
        cascades[ch.Primary].xyz, cascades[ch.Primary].w, count,
        irrTile, irrInvSize, distTile, distInvSize,
        DDGI_SurfaceBias(N, V, cascades[ch.Primary].w, biasScale, biasMax),
        probeData, skipMode, tilesPerRow, ch.Primary, (int3)scrolls[ch.Primary].xyz);
    if (ch.Next == ch.Primary || ch.PrimaryWeight >= 0.999f) return primary;

    float3 next = SampleDDGIIrradianceCheb(
        irrAtlas, distAtlas, samp, rawPos, N,
        cascades[ch.Next].xyz, cascades[ch.Next].w, count,
        irrTile, irrInvSize, distTile, distInvSize,
        DDGI_SurfaceBias(N, V, cascades[ch.Next].w, biasScale, biasMax),
        probeData, skipMode, tilesPerRow, ch.Next, (int3)scrolls[ch.Next].xyz);
    return lerp(next, primary, ch.PrimaryWeight);
}

#endif
