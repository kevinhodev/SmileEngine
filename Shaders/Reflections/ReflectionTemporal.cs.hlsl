// Specular GI — passe TEMPORAL (acumulação entre frames, Fase 3). Porte do LumenReflectionDenoiser
// Temporal: estabiliza o glossy/half-res acumulando ao longo dos frames. Duas peças-chave:
//  (1) REPROJEÇÃO PARALLAX-AWARE — reprojeta pela POSIÇÃO VIRTUAL do reflexo (superfície + dir de
//      reflexão × distância do hit), não pela superfície. Assim o conteúdo refletido é rastreado
//      onde ele estava na tela no frame anterior (o reflexo tem "profundidade", move diferente da
//      superfície). hitDist=0 (miss) degrada p/ reprojeção da superfície.
//  (2) NEIGHBORHOOD CLAMP (YCoCg) — recorta o history à caixa de variância da vizinhança atual
//      (média ± γ·σ) -> rejeita ghosting de desoclusão/movimento.
// Acumula até MaxFrames (contador por pixel em .a); blend = 1/frames. Ping-pong de history.

#include "../GI/DDGICommon.hlsli" // DDGI_OctDecode

cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;     // W, H, 1/W, 1/H
    float4 ReflectParams;    // x = maxRoughnessToTrace, y = roughnessFadeLength, ...
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;
    float4 HalfScreenParams;
    row_major float4x4 PrevViewProj; // VP (sem jitter) do frame anterior
    float4 TemporalParams;   // x = maxFramesAccumulated, y = neighborhoodClampScale (γ)
};

Texture2D<float4> Resolved    : register(t0); // rgb = radiancia reconstruida, a = hitDist
Texture2D<float4> GBuffer     : register(t1);
Texture2D<float>  Depth       : register(t2);
Texture2D<float4> HistoryPrev : register(t3); // rgb = acumulado, a = nº de frames acumulados

RWTexture2D<float4> RWHistory : register(u0); // saída (vira entrada do composite)

SamplerState LinearClamp : register(s0);

float3 RGBToYCoCg(float3 c) {
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
                  0.5f * c.r - 0.5f * c.b,
                  -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}
float3 YCoCgToRGB(float3 c) {
    float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int2 px = (int2)DTid.xy;
    if (px.x >= (int)ScreenParams.x || px.y >= (int)ScreenParams.y) return;

    float4 gb        = GBuffer.Load(int3(px, 0));
    float  roughness = gb.b;
    float  combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));
    float  deviceZ   = Depth.Load(int3(px, 0)).r;

    float4 current = Resolved.Load(int3(px, 0)); // rgb + hitDist
    if (deviceZ <= 0.0f || combineAlpha <= 0.0f) {
        RWHistory[px] = float4(current.rgb, 0.0f);
        return;
    }

    // Geometria do pixel.
    float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 V = normalize(CameraPos.xyz - worldPos);

    // (1) Reprojeção parallax: posição virtual do reflexo = superfície + R × distância do hit.
    float3 R = reflect(-V, N);
    float3 virtualPos = worldPos + R * current.a;
    float4 prevClip = mul(float4(virtualPos, 1.0f), PrevViewProj);
    float3 history = current.rgb;
    float  frames  = 0.0f;
    if (prevClip.w > 0.0f) {
        float2 prevNdc = prevClip.xy / prevClip.w;
        float2 prevUv  = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            float4 h = HistoryPrev.SampleLevel(LinearClamp, prevUv, 0.0f);
            history = h.rgb;
            frames  = h.a;
        }
    }

    // (2) Neighborhood clamp (YCoCg): caixa de variância 3×3 da radiância atual.
    float3 m1 = 0.0f, m2 = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        int2 c = clamp(px + int2(x, y), int2(0, 0), int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));
        float3 s = RGBToYCoCg(max(Resolved.Load(int3(c, 0)).rgb, 0.0f));
        m1 += s; m2 += s * s;
    }
    m1 /= 9.0f; m2 /= 9.0f;
    float3 sigma  = sqrt(max(m2 - m1 * m1, 0.0f));
    float  gamma  = TemporalParams.y;
    float3 boxMin = m1 - gamma * sigma;
    float3 boxMax = m1 + gamma * sigma;
    float3 histYCoCg = clamp(RGBToYCoCg(history), boxMin, boxMax);
    history = max(YCoCgToRGB(histYCoCg), 0.0f);

    // Acumula: contador por pixel; blend = 1/frames.
    frames = min(frames + 1.0f, TemporalParams.x);
    float  alpha  = 1.0f / frames;
    float3 result = lerp(history, current.rgb, alpha);

    RWHistory[px] = float4(result, frames);
}
