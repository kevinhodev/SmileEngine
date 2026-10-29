#ifndef SMILE_RESTIR_NEIGHBOR_SELECTION_HLSLI
#define SMILE_RESTIR_NEIGHBOR_SELECTION_HLSLI

// Selecao de vizinhos guiada por compatibilidade para o reuso espacial do ReSTIR.
// Referencia: Junkins, Kettunen, Lin, Ramamoorthi, Wyman, "Compatibility-Guided Neighbor
// Selection for ReSTIR", Proc. ACM CGIT 9(4), HPG 2026 (doi 10.1145/3820024).
//
// O sorteio uniforme no disco seguido de teste binario de normal/posicao desperdica taps: perto de
// silhueta, folhagem ou geometria fina, a maioria cai fora do limiar e o pixel reusa pouco ou nada.
// Aqui um pool maior de candidatas e pontuado por um escore CONTINUO de compatibilidade geometrica
// e os K vizinhos saem dele por amostragem ponderada. O paper reporta 6-29% menos SMAPE e 22-49%
// menos covariancia temporal por 2-5% de custo.
//
// A escolha depende so da geometria primaria (normal, posicao) e nunca do conteudo dos
// reservoirs. Isso preserva a correcao MIS dos passes espaciais: o denominador continua somando
// os mesmos dominios que participaram do merge.
//
// Contrato com quem inclui: definir ANTES do #include
//   bool RNS_LoadSurface(int2 px, out float3 normal, out float3 position)
// devolvendo false para fundo/ceu. O pixel recebido esta no dominio do reservoir (no GI em meia
// resolucao, o de trace); o loader faz a traducao para o G-buffer.
//
// Depende de GGX_PCG e GGX_ConcentricDisk (Reflections/GGXSample.hlsli).

#define RNS_MAX_NEIGHBORS 8

// Tamanho do pool e raio de busca. O raio e multiplo do raio do disco classico para que o mesmo
// knob do editor continue governando a escala do reuso; com o default de 16 px resulta em 48 px,
// a mesma ordem dos 50 px da implementacao de referencia.
static const uint  RNS_CANDIDATE_COUNT = 32u;
static const float RNS_SEARCH_RADIUS_SCALE = 3.0f;

// Constantes do escore (secao 5 do paper): omega e o angulo solido do footprint que normaliza a
// distancia mundo pela distancia da camera; beta e o expoente do termo de normal.
static const float RNS_FOOTPRINT_SOLID_ANGLE = 0.05f;
static const float RNS_NORMAL_EXPONENT = 8.0f;
// Candidata "boa": quando K delas foram vistas a busca termina cedo, como no paper.
static const float RNS_GOOD_SCORE = 0.5f;
// Piso proprio da engine. Os target functions do MIS do DI nao incluem visibilidade, entao uma
// candidata quase incompativel (outra superficie atras de uma silhueta) nunca deve ser escolhida
// so por falta de alternativa. O valor corresponde a ~7 footprints de distancia com normais iguais.
static const float RNS_MIN_SCORE = 1.0e-3f;

float RNS_CompatibilityScore(float3 centerNormal, float3 centerPos, float centerCamDist,
                             float3 neighborNormal, float3 neighborPos) {
    const float normalSim = pow(saturate(dot(centerNormal, neighborNormal)), RNS_NORMAL_EXPONENT);
    // Raio do footprint: disco que subtende omega a distancia d, s = sqrt(omega * d^2 / pi).
    const float footprint = max(centerCamDist * sqrt(RNS_FOOTPRINT_SOLID_ANGLE / SMILE_PI),
                                1.0e-6f);
    const float posSim = exp(-distance(centerPos, neighborPos) / footprint);
    return normalSim * posSim;
}

float RNS_Rand(inout uint state) {
    state = GGX_PCG(state);
    return ((state & 0x00FFFFFFu) + 0.5f) * (1.0f / 16777216.0f);
}

// Escolhe ate `count` vizinhos DISTINTOS e devolve quantos encontrou.
//
// Variacao sobre a referencia: em vez de K reservoirs independentes sobre o mesmo stream (que
// sorteiam com reposicao e repetem vizinho com frequencia quando a busca para cedo), usa as chaves
// de Efraimidis-Spirakis, log(u)/w, e guarda as K maiores. A distribuicao e a da amostragem
// ponderada SEM reposicao, com um aleatorio por candidata em vez de K.
uint RNS_SelectNeighbors(int2 centerPx, int2 extent, float diskRadius, uint count,
                         float3 centerNormal, float3 centerPos, float centerCamDist,
                         uint seed, out int2 picks[RNS_MAX_NEIGHBORS]) {
    count = min(count, (uint)RNS_MAX_NEIGHBORS);
    float keys[RNS_MAX_NEIGHBORS];
    [unroll]
    for (uint k = 0u; k < RNS_MAX_NEIGHBORS; ++k) {
        keys[k] = -3.402823466e+38f;
        picks[k] = centerPx;
    }
    if (count == 0u) return 0u;

    const float searchRadius = max(diskRadius * RNS_SEARCH_RADIUS_SCALE, 1.0f);
    uint state = seed;
    uint picked = 0u;
    uint good = 0u;

    [loop]
    for (uint c = 0u; c < RNS_CANDIDATE_COUNT; ++c) {
        const float2 e = float2(RNS_Rand(state), RNS_Rand(state));
        const int2 qpx = centerPx + int2(round(GGX_ConcentricDisk(e) * searchRadius));
        if (any(qpx < 0) || any(qpx >= extent) || all(qpx == centerPx)) continue;

        float3 qn, qx;
        if (!RNS_LoadSurface(qpx, qn, qx)) continue;
        const float score = RNS_CompatibilityScore(centerNormal, centerPos, centerCamDist,
                                                   qn, qx);
        const float u = RNS_Rand(state);
        if (score < RNS_MIN_SCORE) continue;
        if (score >= RNS_GOOD_SCORE) ++good;

        // Mesmo pixel sorteado duas vezes: manter a chave maior equivale a uma unica candidata
        // com a melhor das duas sortes, e evita dois dominios identicos no merge.
        const float key = log(u) / score;
        uint slot = picked;
        [loop]
        for (uint d = 0u; d < picked; ++d)
            if (all(picks[d] == qpx)) slot = d;

        // Insercao ordenada (chave decrescente). Pixel novo entra no fim da lista (ou disputa a
        // ultima posicao, se cheia); repetido so sobe se a chave nova for maior.
        bool insert;
        if (slot < picked) {
            insert = key > keys[slot];
        } else if (picked < count) {
            insert = true;
            ++picked;
        } else {
            slot = count - 1u;
            insert = key > keys[slot];
        }
        if (insert) {
            [loop]
            while (slot > 0u && keys[slot - 1u] < key) {
                keys[slot] = keys[slot - 1u];
                picks[slot] = picks[slot - 1u];
                --slot;
            }
            keys[slot] = key;
            picks[slot] = qpx;
        }

        if (good >= count) break;
    }
    return picked;
}

#endif
