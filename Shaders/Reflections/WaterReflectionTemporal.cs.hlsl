#include "../GBuffer.hlsli"

cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 ReflectParams;
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;
    float4 HalfScreenParams;
    row_major float4x4 PrevViewProj;
    float4 PrevCameraPos;
    float4 TemporalParams;
};

Texture2D<float4> Current     : register(t0);
Texture2D<float4> GBuffer     : register(t1);
Texture2D<float>  Depth       : register(t2);
Texture2D<float4> HistoryPrev : register(t3);
Texture2D<float4> Motion      : register(t4);
RWTexture2D<float4> RWHistory : register(u0);

SamplerState LinearClamp : register(s0);

float3 RGBToYCoCg(float3 c) {
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
                  0.5f * c.r - 0.5f * c.b,
                 -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float3 YCoCgToRGB(float3 c) {
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const int2 px = (int2)DTid.xy;
    if (px.x >= (int)ScreenParams.x || px.y >= (int)ScreenParams.y) return;

    const float4 gb = GBuffer.Load(int3(px, 0));
    if (GBuffer_DecodeShadingModel(gb.a) != SMILE_SHADINGMODEL_WATER ||
        Depth.Load(int3(px, 0)).r <= 0.0f) {
        RWHistory[px] = 0.0f;
        return;
    }

    const float4 currentSignal = Current.Load(int3(px, 0));
    const float3 current = max(currentSignal.rgb, 0.0f);
    const float currentHitDist = max(currentSignal.a, 0.0f);
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float4 mv = Motion.Load(int3(px, 0));
    const float2 prevUv = uv - mv.xy;

    float3 history = current;
    bool historyValid = false;
    if (mv.w > 0.5f && all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
        const float4 h = HistoryPrev.SampleLevel(LinearClamp, prevUv, 0.0f);
        // Alpha guarda hitDist+1: 0 continua sendo history invalido e 1 representa miss/ceu.
        // O caminho antigo guardava apenas numero de frames e aceitava um hit historico em outro
        // predio, criando uma segunda reflexao deslocada durante movimento de camera/onda.
        if (h.a >= 1.0f) {
            const float previousHitDist = max(h.a - 1.0f, 0.0f);
            const bool currentGeometry = currentHitDist > 1.0e-3f;
            const bool previousGeometry = previousHitDist > 1.0e-3f;
            const float hitTolerance = max(0.75f, currentHitDist * 0.12f);
            const bool sameHitClass = currentGeometry == previousGeometry;
            const bool compatibleDistance = !currentGeometry ||
                abs(previousHitDist - currentHitDist) <= hitTolerance;
            historyValid = sameHitClass && compatibleDistance;
        }
        if (historyValid) {
            history = max(h.rgb, 0.0f);
        }
    }

    float3 center = RGBToYCoCg(current);
    float3 m1 = center;
    float3 m2 = center * center;
    float count = 1.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) continue;
        const int2 q = clamp(px + int2(x, y), int2(0, 0), int2(ScreenParams.xy) - 1);
        const float4 qgb = GBuffer.Load(int3(q, 0));
        if (GBuffer_DecodeShadingModel(qgb.a) != SMILE_SHADINGMODEL_WATER) continue;
        const float3 s = RGBToYCoCg(max(Current.Load(int3(q, 0)).rgb, 0.0f));
        m1 += s;
        m2 += s * s;
        count += 1.0f;
    }
    m1 /= count;
    m2 /= count;
    const float3 sigma = sqrt(max(m2 - m1 * m1, 0.0f));
    const float gamma = max(TemporalParams.y, 0.5f);
    const float3 boxMin = m1 - gamma * sigma;
    const float3 boxMax = m1 + gamma * sigma;
    history = max(YCoCgToRGB(clamp(RGBToYCoCg(history), boxMin, boxMax)), 0.0f);

    // A TAA/RR posterior ja faz acumulacao longa. Aqui basta um filtro curto e rejeitado por
    // hit-distance; empilhar ate 12 frames com motion apenas do receiver gerava trailing duplo.
    const float historyWeight = historyValid ? 0.55f * saturate(mv.z) : 0.0f;
    RWHistory[px] = float4(lerp(current, history, historyWeight), currentHitDist + 1.0f);
}
