#ifndef SMILE_REGIR_COMMON
#define SMILE_REGIR_COMMON

#include "../LightsCommon.hlsli"

static const uint REGIR_INVALID_LIGHT = 0xFFFFFFFFu;

struct FReGIRReservoir {
    uint  LightIndex;
    float AverageWeight;
    float SampleTarget;
    uint  HistoryLength;
};

uint ReGIRHash(uint v) {
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

float ReGIRRand(inout uint state) {
    state = ReGIRHash(state);
    return float(state & 0x00FFFFFFu) / 16777216.0f;
}

float ReGIRLuminance(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Target barato da primeira etapa: energia no centro da celula, sem normal/BRDF/visibilidade.
// O FALLOFF fica ancorado no centro, com a distancia limitada pela meia-diagonal para que uma luz
// dentro da celula nao domine arbitrariamente os slots (e o vies de discretizacao que o cap. 23
// aceita de proposito).
//
// Ja o ALCANCE — janela e cone — e testado contra a CAIXA inteira da celula, e nao contra o centro.
// Motivo: o estimador de duas etapas so cobre a integral se o suporte desta pdf contiver o da etapa
// 2. Como `win` e `cone` sao cortes DUROS, avalia-los so no centro faz uma luz que ilumina algum
// ponto da celula sem alcancar o centro receber target zero em TODAS as celulas: ela nunca entra em
// slot nenhum e simplesmente SOME do indireto. Isso e perda de energia, nao ruido — nenhum denoiser
// recupera. Com a grade esticada na AABB da cena as celulas ficam grandes e o caso e comum
// (luminaria pequena, spot estreito). O preco e trocar esse vies por um pouco de variancia: uma luz
// que so encosta na celula entra no pool com peso cheio.
float ReGIRGridTarget(FPunctualLight lt, float3 cellCenter, float3 halfCell) {
    const float minDistanceSquared = dot(halfCell, halfCell);

    float3 toL = lt.PosInvRadius.xyz - cellCenter;
    float distSq = dot(toL, toL);
    float dist = sqrt(distSq);
    float3 L = toL / max(dist, 1e-4f);

    // Janela pela distancia ao ponto MAIS PROXIMO da caixa: zero so quando a esfera de influencia
    // da luz nao encosta na celula em ponto nenhum.
    float3 dBox = max(abs(toL) - halfCell, 0.0f);
    float win = dot(dBox, dBox) * lt.PosInvRadius.w * lt.PosInvRadius.w;
    win = saturate(1.0f - win * win);
    win *= win;

    float denom = max(max(distSq, minDistanceSquared),
                      lt.ColorSourceRadius.w * lt.ColorSourceRadius.w);

    float cone = 1.0f;
    if (lt.DirCosOuter.w > -1.5f) {
        if (distSq > minDistanceSquared) {
            // Cone alargado pelo raio angular da esfera que envolve a celula:
            // cos(theta - alpha) = cos theta * cos alpha + sin theta * sin alpha.
            const float cosSpot = dot(-L, lt.DirCosOuter.xyz);
            const float sinCell = saturate(sqrt(minDistanceSquared / distSq));
            const float cosCell = sqrt(saturate(1.0f - sinCell * sinCell));
            const float sinSpot = sqrt(saturate(1.0f - cosSpot * cosSpot));
            const float sm = saturate(((cosSpot * cosCell + sinSpot * sinCell)
                                       - lt.DirCosOuter.w) * lt.SpotParams.x);
            cone = sm * sm;
        }
        // Luz dentro da esfera que envolve a celula: o cone pode varrer qualquer ponto dela.
    }
    return ReGIRLuminance(lt.ColorSourceRadius.rgb) * win * cone / max(denom, 1e-8f);
}

#endif
