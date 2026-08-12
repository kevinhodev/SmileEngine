#include "DDGICommon.hlsli"

#define DDGI_RAYS      64 
#define DDGI_DIST_TILE 14
#define DDGI_DIST_SHARP 50.0f

// Declarado ate o FIM do DDGICB (era so o prefixo ate o DistAtlasParams) porque a invalidacao
// regional mora nos ultimos campos. Mesma razao do DDGIUpdate: prefixo truncado le por offset e
// funciona, mas qualquer campo novo depois passa a exigir a cadeia inteira mesmo assim.
cbuffer DDGICB : register(b0) {
    float4 GridMinSpacing;
    float4 GridCountRays;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColorHyst;
    float4 TraceParams;
    float4 DistAtlasParams;
    float4 MiscParams;
    float4 MiscParams2;
    float4 RayEpsA;
    float4 RayEpsB;
    float4 GIDistParams;
    float4 GIBiasParams;
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;
    float4 InvalidateMin;     // xyz = min da caixa de invalidacao, w = 1 se ativa (irradiancia)
    float4 InvalidateMaxHyst; // xyz = max da caixa, w = hysteresis regional da IRRADIANCIA
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 MiscParams3;       // x = hysteresis regional DESTE atlas, y = 1 se a janela dele esta
                              // aberta (e mais longa que a da irradiancia — ver DDGI.h)
    // Cascatas: a posicao da sonda no teste de invalidacao regional e o clamp dos momentos saem
    // daqui, nao do GridMinSpacing (que e a GROSSA).
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
};

Texture2D<float4>   ProbesTrace : register(t0);
Buffer<float4>      ProbeData   : register(t1); // w>=1 = probe recem-ativado/relocado
RWTexture2D<float2> DistAtlas   : register(u0);

// Tabela de copia da borda octaedrica (wrap com fold; do Wicked, mesma que o Flax usa):
// uint4(srcX, srcY, dstX, dstY) em coords do tile COM borda (0..15 p/ interior 14).
#define DDGI_BORDER_COUNT 60
static const uint4 kBorderOffsets[DDGI_BORDER_COUNT] = {
    uint4(14,1, 1,0), uint4(13,1, 2,0), uint4(12,1, 3,0), uint4(11,1, 4,0), uint4(10,1, 5,0),
    uint4(9,1, 6,0),  uint4(8,1, 7,0),  uint4(7,1, 8,0),  uint4(6,1, 9,0),  uint4(5,1, 10,0),
    uint4(4,1, 11,0), uint4(3,1, 12,0), uint4(2,1, 13,0), uint4(1,1, 14,0),

    uint4(14,14, 1,15), uint4(13,14, 2,15), uint4(12,14, 3,15), uint4(11,14, 4,15), uint4(10,14, 5,15),
    uint4(9,14, 6,15),  uint4(8,14, 7,15),  uint4(7,14, 8,15),  uint4(6,14, 9,15),  uint4(5,14, 10,15),
    uint4(4,14, 11,15), uint4(3,14, 12,15), uint4(2,14, 13,15), uint4(1,14, 14,15),

    uint4(1,14, 0,1),  uint4(1,13, 0,2),  uint4(1,12, 0,3),  uint4(1,11, 0,4),  uint4(1,10, 0,5),
    uint4(1,9, 0,6),   uint4(1,8, 0,7),   uint4(1,7, 0,8),   uint4(1,6, 0,9),   uint4(1,5, 0,10),
    uint4(1,4, 0,11),  uint4(1,3, 0,12),  uint4(1,2, 0,13),  uint4(1,1, 0,14),

    uint4(14,14, 15,1), uint4(14,13, 15,2), uint4(14,12, 15,3), uint4(14,11, 15,4), uint4(14,10, 15,5),
    uint4(14,9, 15,6),  uint4(14,8, 15,7),  uint4(14,7, 15,8),  uint4(14,6, 15,9),  uint4(14,5, 15,10),
    uint4(14,4, 15,11), uint4(14,3, 15,12), uint4(14,2, 15,13), uint4(14,1, 15,14),

    uint4(14,14, 0,0), uint4(1,14, 15,0), uint4(14,1, 0,15), uint4(1,1, 15,15)
};

[numthreads(DDGI_DIST_TILE, DDGI_DIST_TILE, 1)]
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID) {
    int numProbes = (int)AtlasParams.w;
    // Grade 2D de grupos (ver DDGI_ProbeFromGroup): o dispatch 1D parava em 65535 sondas.
    int probeIdx  = DDGI_ProbeFromGroup(Gid.xy, numProbes);
    if (probeIdx >= numProbes) return;

    int3 count = (int3)GridCountRays.xyz;
    // Indice GLOBAL -> (cascata, indice local). A geometria da sonda e sempre LOCAL; so o atlas e
    // os buffers falam em global. Com uma cascata os dois coincidem — e por isso a F6.1 nao muda
    // pixel nenhum.
    int  cascade  = DDGI_CascadeOfProbe(probeIdx, count);
    int  localIdx = DDGI_LocalProbeIndex(probeIdx, count);
    int3 pc    = DDGI_ProbeCoord(localIdx, count);
    int  tile  = (int)DistAtlasParams.x;
    int2 tileOrigin = DDGI_TileOrigin(pc, count, tile,
                                      DDGI_TilesPerRow(DistAtlasParams.y, tile), cascade);
    int2 local      = int2(GTid.xy);

    float2 octUV    = ((float2)local + 0.5f) / (float)tile;
    float3 texelDir = DDGI_OctDecode(octUV * 2.0f - 1.0f);

    // Clamp na convencao do paper (G3D): 1.5 * ||spacing do grid|| = 1.5*sqrt(3)*spacing ~= 2.6.
    // Cobre a diagonal da gaiola 2x2x2 (sqrt(3)*spacing) + bias/offset com folga; hits alem disso
    // nao interessam ao Chebyshev (o ponto amostrado esta sempre dentro da gaiola) e so inflavam
    // media/variancia, deixando o teste permissivo demais (leak atraves de parede) com 4.0.
    // Da CASCATA: o clamp dos momentos e 2,6 espacamentos, e o da grossa deixaria a fina medindo
    // visibilidade quatro vezes alem da propria gaiola.
    float distMax = GICascadeGridMinSpacing[cascade].w * 2.6f;

    uint  frame = (uint)TraceParams.x;
    float sumD = 0.0f, sumD2 = 0.0f, wsum = 0.0f;

    [loop]
    for (int r = 0; r < DDGI_RAYS; ++r) {
        float ad = ProbesTrace[DDGI_TraceTexel(probeIdx, r, numProbes, DDGI_RAYS)].a;
        if (ad < -1e8f) continue; 
        float3 rdir = DDGI_RayDirection(r, DDGI_RAYS, frame, (uint)probeIdx);
        float  w    = pow(max(0.0f, dot(texelDir, rdir)), DDGI_DIST_SHARP);
        if (w <= 0.0f) continue;

        float d = min(abs(ad), distMax);
        sumD  += w * d;
        sumD2 += w * d * d;
        wsum  += w;
    }

    float2 result = (wsum > 0.0f) ? float2(sumD / wsum, sumD2 / wsum) : float2(distMax, distMax * distMax);

    int2   texel = tileOrigin + local;
    float2 prev  = DistAtlas[texel];
    // Probe recem-ativado/relocado: distancias antigas sao de outra posicao — reset (hyst 0).
    //
    // Histerese PROPRIA (DistAtlasParams.w), mais alta que a da irradiancia e SEM o detector
    // adaptativo do DDGIUpdate. Os dois motivos sao o mesmo fato: com o expoente 50 acima, o
    // lobo de cada texel tem 9,5 graus a meio peso (cos^50 = 0.5 => 0.5^(1/50) = cos 9,51),
    // ou 0,69% da esfera. Com 64 raios isso da ~0,44 raio esperado por texel por frame — o
    // texel guarda, na pratica, a distancia do raio que por acaso caiu mais perto, e as
    // direcoes giram todo frame.
    //
    // Ou seja: aqui a mistura temporal nao e atraso, e a RECONSTRUCAO do estimador, com ~1/(1-h)
    // amostras efetivas. Descer junto com a irradiancia cortaria pela metade as amostras que
    // formam os momentos que o Chebyshev consome, e a precisao deles ja e o gargalo. E um
    // detector de mudanca por frame dispararia com o proprio ruido de reamostragem, nao com
    // mudanca de cena.
    //
    // O corolario, agora implementado: sem detector, quem invalida este atlas e o EVENTO
    // explicito — a mesma caixa que o DDGIUpdate consome, com a mesma posicao relocada da sonda
    // (DDGI_RegionalHysteresis). Antes disso, criar ou remover geometria preservava ~99% dos
    // momentos velhos por update: a iluminacao respondia e a visibilidade nao, e o que se via
    // era mancha — luz nova pesada por uma visibilidade de uma cena que nao existe mais.
    //
    // A hysteresis regional daqui e a PROPRIA (MiscParams3.x), mais alta que a da irradiancia,
    // pelo mesmo motivo da base: ~0,44 raio por texel por frame. A regional da irradiancia (0.90)
    // deixaria 19 amostras efetivas de um estimador que e ordens de grandeza mais ruidoso — o
    // flicker de iluminacao viraria flicker de VISIBILIDADE, que e pior (mancha de sombra
    // piscando em vez de brilho piscando).
    float  hyst  = (ProbeData[probeIdx].w >= 1.0f) ? 0.0f : DistAtlasParams.w;
    // Janela propria: a do dist e mais longa, entao o flag de "aberta" nao pode ser o da
    // irradiancia. So o w do InvalidateMin e substituido; a CAIXA e a mesma.
    const float4 distInvMin = float4(InvalidateMin.xyz, MiscParams3.y);
    // Grid da CASCATA da sonda: o teste compara a posicao de mundo dela contra a caixa, e
    // reconstrui-la com a origem/espacamento da grossa poria a sonda da fina em outro lugar.
    hyst = DDGI_RegionalHysteresis(pc, ProbeData[probeIdx].xyz, GICascadeGridMinSpacing[cascade].xyz,
                                   GICascadeGridMinSpacing[cascade].w, distInvMin, InvalidateMaxHyst.xyz,
                                   MiscParams3.x, hyst);
    DistAtlas[texel] = lerp(result, prev, hyst);

    // Copia a borda de 1px do tile (fold octaedrico) p/ o bilinear ser continuo na costura.
    DeviceMemoryBarrierWithGroupSync();
    uint groupIdx  = GTid.y * DDGI_DIST_TILE + GTid.x;
    int2 padOrigin = tileOrigin - 1;
    [loop]
    for (uint b = groupIdx; b < DDGI_BORDER_COUNT; b += DDGI_DIST_TILE * DDGI_DIST_TILE) {
        uint4 bo = kBorderOffsets[b];
        DistAtlas[padOrigin + int2(bo.zw)] = DistAtlas[padOrigin + int2(bo.xy)];
    }
}
