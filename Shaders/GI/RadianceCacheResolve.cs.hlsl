// Resolve do World Radiance Cache: uma thread por entrada da tabela.
//
// Junta o que foi acumulado NESTE frame (`Accum`) com o historico (`Resolved`), aplica o teto de
// amostras, envelhece as celulas que ninguem tocou e despeja as que passaram do prazo. Zera o
// acumulador no fim para o proximo frame.
//
// A separacao acumulacao/resolvido e o que impede a auto-referencia: o `RC_Query` do
// RadianceCache.hlsli le SEMPRE de `Resolved`, que so contem frames anteriores. Ver o cabecalho
// daquele arquivo.
//
// Politica de eviccao e teto de amostras vem da SHaRC (RTXGI 2.0) e do [Sousa2025]
// (SAMPLE_NUM_MAX 64, STALE_FRAME_NUM_MAX 64).
//
// MODOS (StatsParams.x). O mesmo .cso e despachado duas vezes por frame porque os contadores de
// ocupacao precisam comecar zerados e nao ha como zerar e acumular no MESMO dispatch (nada
// ordena as duas coisas entre grupos). Reusar a PSO com um modo evita um segundo shader so
// para escrever quatro zeros.
//   0 = resolve  — dispatch de Capacity/64 grupos
//   1 = clear    — dispatch de 1 grupo, so a thread 0 trabalha

#include "RadianceCache.hlsli"

cbuffer RadianceCacheCB : register(b0) {
    float4 CacheParams;  // x = capacity, y = maxAccumSamples, z = staleFrameMax, w = resetAll
    // y = PISO DE CONFIANCA, o mesmo que a consulta usa. O resolve precisa dele para contar
    // quantas celulas estao de fato utilizaveis: `tem amostra` e `encerra um caminho` deixaram de
    // ser a mesma coisa quando o piso entrou, e o estado global Filling/Active vai ler o segundo.
    float4 StatsParams;  // x = modo (0 resolve, 1 clear), y = piso de confianca, zw livres
};

RWStructuredBuffer<uint>  Entries  : register(u0);
RWStructuredBuffer<uint>  Accum    : register(u1); // 4 uints por entrada: rgb em ponto fixo + N
RWStructuredBuffer<uint4> Resolved : register(u2); // xyz = asuint(radiancia), w = N | frames<<16
RWStructuredBuffer<uint>  Stats    : register(u3); // ver RC_STAT_* no RadianceCache.hlsli

// Reducao por grupo: 1 M de InterlockedAdd por frame saturaria a porta de atomicos do buffer.
// Com o reduce em groupshared sobra UM atomico por grupo de 64 — 16 k no total na capacidade
// padrao, que e ruido.
groupshared uint SharedOccupied[64];
groupshared uint SharedHasSamples[64];
groupshared uint SharedSamples[64];
groupshared uint SharedEvicted[64];
groupshared uint SharedConfident[64];

[numthreads(64, 1, 1)]
void main(uint3 did : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID) {
    if (StatsParams.x > 0.5f) {
        if (did.x == 0u) {
            Stats[RC_STAT_OCCUPIED]    = 0u;
            Stats[RC_STAT_HAS_SAMPLES] = 0u;
            Stats[RC_STAT_SAMPLES]     = 0u;
            Stats[RC_STAT_EVICTED]     = 0u;
            Stats[RC_STAT_CONFIDENT]   = 0u;
            // Os contadores de query sao escritos pelos TRACES, que rodam antes do resolve. Zerar
            // aqui apagaria a medida do proprio frame; eles sao zerados no clear do frame seguinte,
            // que e o que o painel le. Ver RC_STAT_QUERY_*.
            Stats[RC_STAT_QUERIES] = 0u;
            Stats[RC_STAT_HITS]    = 0u;
            // Idem para o detalhe: misses por motivo vem dos traces, insercoes vem do passe de
            // update — os dois rodam antes daqui, e a copia do anel os pega deste frame.
            Stats[RC_STAT_MISS_SHORT]   = 0u;
            Stats[RC_STAT_MISS_CONE]    = 0u;
            Stats[RC_STAT_MISS_NOENTRY] = 0u;
            Stats[RC_STAT_MISS_EMPTY]   = 0u;
            Stats[RC_STAT_MISS_WARMING] = 0u;
            Stats[RC_STAT_MISS_STALE]   = 0u;
            Stats[RC_STAT_INS_TRIES]     = 0u;
            Stats[RC_STAT_INS_FULL]      = 0u;
            Stats[RC_STAT_INS_CONTENDED] = 0u;
            Stats[RC_STAT_INS_RETRIES]   = 0u;
            Stats[RC_STAT_INS_CAPPED]    = 0u;
            Stats[RC_STAT_INS_PROBES]    = 0u;
            // PROBEMAX e escrito por InterlockedMax, entao o zero e obrigatorio e nao cosmetico:
            // sem ele o maximo do frame nunca desceria e o painel mostraria o pior caso da sessao
            // inteira como se fosse o de agora.
            Stats[RC_STAT_INS_PROBEMAX] = 0u;
            // O produtor. PATH_DEPTH tambem e InterlockedMax, e vale a mesma observacao.
            Stats[RC_STAT_PATHS]        = 0u;
            Stats[RC_STAT_PATH_VERTS]   = 0u;
            Stats[RC_STAT_PATH_DEPTH]   = 0u;
            Stats[RC_STAT_TERM_SKY]     = 0u;
            Stats[RC_STAT_TERM_CACHE]   = 0u;
            Stats[RC_STAT_TERM_KILLED]  = 0u;
            Stats[RC_STAT_TERM_MISS]    = 0u;
            Stats[RC_STAT_TERM_NOQUERY] = 0u;
            Stats[RC_STAT_TERM_LOBE]    = 0u;
            Stats[RC_STAT_TERM_OTHER]   = 0u;
            // Fonte do terminal do render (quarto regime). Zerados no MESMO dispatch de clear que
            // os de query, e nao no de varredura: como eles, sao escritos pelos TRACES, que rodam
            // antes do resolve — zera-los na varredura apagaria a medida do proprio frame.
            Stats[RC_STAT_SRC_TOTAL]      = 0u;
            Stats[RC_STAT_SRC_CACHE]      = 0u;
            Stats[RC_STAT_SRC_DDGI]       = 0u;
            Stats[RC_STAT_SRC_ZERO]       = 0u;
            Stats[RC_STAT_SRC_INELIGIBLE] = 0u;
        }
        return;
    }

    const uint idx = did.x;
    const bool inRange = idx < (uint)CacheParams.x;
    const uint base = idx * 4u;

    // evictedOut e uma TAXA POR FRAME (quantas cairam neste resolve), nao um acumulado — e o que
    // diz se a evicção esta dando conta do fluxo de celulas novas. Zero com a camera andando
    // significa tabela que so cresce.
    uint occupied = 0u, hasSamples = 0u, samplesOut = 0u, evictedOut = 0u, confident = 0u;
    // Piso de confianca do frame. O `max(...,1)` faz "0" e "1" significarem a mesma coisa, como no
    // RC_QueryInner: uma amostra e o minimo para a celula ter radiancia.
    const uint minSamples = max((uint)StatsParams.y, 1u);

    // Reset global: usado quando a cena troca ou a camera teleporta — o conteudo descreve outro
    // lugar do mundo e envelhecer celula por celula levaria STALE_FRAME_MAX frames.
    if (inRange && CacheParams.w != 0.0f) {
        Entries[idx]  = RC_INVALID_CHECKSUM;
        Resolved[idx] = uint4(0u, 0u, 0u, 0u);
        Accum[base + 0u] = 0u; Accum[base + 1u] = 0u;
        Accum[base + 2u] = 0u; Accum[base + 3u] = 0u;
    } else if (inRange && Entries[idx] != RC_INVALID_CHECKSUM &&
               Entries[idx] != RC_TOMBSTONE_CHECKSUM) {
        const uint newSamples = Accum[base + 3u];

        const uint4 prev          = Resolved[idx];
        const float3 prevRadiance = asfloat(prev.xyz);
        const uint   prevSamples  = prev.w & 0xFFFFu;
        const uint   prevFrames   = prev.w >> 16u;

        float3 radiance = prevRadiance;
        uint   samples  = prevSamples;
        uint   frames   = prevFrames;
        bool   evicted  = false;

        if (newSamples > 0u) {
            // Media corrente. `Accum` guarda a SOMA das amostras deste frame em ponto fixo, entao
            // a combinacao com o historico e ponderada pelas contagens — nao um lerp de constante
            // fixa.
            const float3 newSum = float3((float)Accum[base + 0u],
                                         (float)Accum[base + 1u],
                                         (float)Accum[base + 2u]) / RC_RADIANCE_SCALE;
            const float total = (float)prevSamples + (float)newSamples;
            radiance = (prevRadiance * (float)prevSamples + newSum) / total;

            // Teto de amostras: com N ilimitado a celula congela e para de responder a mudanca de
            // iluminacao (TOD, luz acesa/apagada). Limitar N transforma a media corrente numa
            // media movel exponencial de constante 1/N — e o `maxAccumulatedFrames` da SHaRC.
            samples = min((uint)total, (uint)CacheParams.y);
            frames  = 0u;
        } else {
            frames = prevFrames + 1u;
            evicted = frames > (uint)CacheParams.z;
        }

        if (evicted) {
            // Nao volte a `vazio`: isto pode estar no meio de uma cadeia de sondagem linear.
            // Tombstone mantem as colisoes posteriores encontraveis e RC_Insert o reutiliza.
            Entries[idx]  = RC_TOMBSTONE_CHECKSUM;
            Resolved[idx] = uint4(0u, 0u, 0u, 0u);
            evictedOut    = 1u;
        } else {
            Resolved[idx] = uint4(asuint(radiance),
                                  (samples & 0xFFFFu) | (min(frames, 0xFFFFu) << 16u));
            occupied   = 1u;
            // As DUAS contagens, e nao uma. `hasSamples` diz que a celula tem radiancia;
            // `confident` diz que ela encerra um caminho. Entre 1 amostra e o piso a celula conta
            // na primeira e nao na segunda — e e a segunda que descreve o cache utilizavel.
            hasSamples = samples > 0u ? 1u : 0u;
            confident  = samples >= minSamples ? 1u : 0u;
            samplesOut = samples;
        }

        Accum[base + 0u] = 0u; Accum[base + 1u] = 0u;
        Accum[base + 2u] = 0u; Accum[base + 3u] = 0u;
    }

    SharedOccupied[tid.x]   = occupied;
    SharedHasSamples[tid.x] = hasSamples;
    SharedSamples[tid.x]    = samplesOut;
    SharedEvicted[tid.x]    = evictedOut;
    SharedConfident[tid.x]  = confident;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (tid.x < stride) {
            SharedOccupied[tid.x]   += SharedOccupied[tid.x + stride];
            SharedHasSamples[tid.x] += SharedHasSamples[tid.x + stride];
            SharedSamples[tid.x]    += SharedSamples[tid.x + stride];
            SharedEvicted[tid.x]    += SharedEvicted[tid.x + stride];
            SharedConfident[tid.x]  += SharedConfident[tid.x + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid.x == 0u) {
        uint ignored;
        if (SharedOccupied[0]   > 0u) InterlockedAdd(Stats[RC_STAT_OCCUPIED],    SharedOccupied[0],   ignored);
        if (SharedHasSamples[0] > 0u) InterlockedAdd(Stats[RC_STAT_HAS_SAMPLES], SharedHasSamples[0], ignored);
        if (SharedSamples[0]    > 0u) InterlockedAdd(Stats[RC_STAT_SAMPLES],     SharedSamples[0],    ignored);
        if (SharedEvicted[0]    > 0u) InterlockedAdd(Stats[RC_STAT_EVICTED],     SharedEvicted[0],    ignored);
        if (SharedConfident[0]  > 0u) InterlockedAdd(Stats[RC_STAT_CONFIDENT],   SharedConfident[0],  ignored);
    }
}
