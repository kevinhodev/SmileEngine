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
// A distancia e limitada pela meia-diagonal da celula para que uma luz dentro dela nao domine
// arbitrariamente os 64 slots. Janela de alcance e cone continuam identicos ao shading real.
float ReGIRGridTarget(FPunctualLight lt, float3 cellCenter, float minDistanceSquared) {
    float3 toL = lt.PosInvRadius.xyz - cellCenter;
    float distSq = dot(toL, toL);
    float dist = sqrt(distSq);
    float3 L = toL / max(dist, 1e-4f);

    float win = distSq * lt.PosInvRadius.w * lt.PosInvRadius.w;
    win = saturate(1.0f - win * win);
    win *= win;

    float denom = max(max(distSq, minDistanceSquared),
                      lt.ColorSourceRadius.w * lt.ColorSourceRadius.w);
    float cone = 1.0f;
    if (lt.DirCosOuter.w > -1.5f) {
        float sm = saturate((dot(-L, lt.DirCosOuter.xyz) - lt.DirCosOuter.w) * lt.SpotParams.x);
        cone = sm * sm;
    }
    return ReGIRLuminance(lt.ColorSourceRadius.rgb) * win * cone / max(denom, 1e-8f);
}

#endif
