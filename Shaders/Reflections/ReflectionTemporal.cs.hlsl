#include "../GI/DDGICommon.hlsli" 

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

Texture2D<float4> Resolved    : register(t0); 
Texture2D<float4> GBuffer     : register(t1);
Texture2D<float>  Depth       : register(t2);
Texture2D<float4> HistoryPrev : register(t3); 
Texture2D<float4> GlossyMotion : register(t4);

RWTexture2D<float4> RWHistory : register(u0); 

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

    float4 current = Resolved.Load(int3(px, 0)); 
    if (deviceZ <= 0.0f || combineAlpha <= 0.0f) {
        RWHistory[px] = float4(current.rgb, 0.0f);
        return;
    }

    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;

    float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
    float3 V = normalize(CameraPos.xyz - worldPos);
    float3 R = reflect(-V, N);
    float3 reprojPos = (roughness < 0.15f) ? (worldPos + R * current.a) : worldPos;
    float3 history = current.rgb;
    float  frames  = 0.0f;
    float  historyConfidence = 1.0f;
    float2 prevUv = 0.0f;
    bool hasReprojection = false;
    const float4 reliable = GlossyMotion.Load(int3(px, 0));
    if (reliable.w > 0.5f && reliable.z > 0.01f) {
        // O centro estocastico pode continuar valido mesmo quando a projecao especular
        // deterministica cai atras da camera anterior; portanto ele e avaliado primeiro.
        prevUv = uv - reliable.xy;
        historyConfidence = saturate(reliable.z);
        hasReprojection = true;
    } else {
        const float4 prevClip = mul(float4(reprojPos, 1.0f), PrevViewProj);
        if (prevClip.w > 0.0f) {
            const float2 prevNdc = prevClip.xy / prevClip.w;
            prevUv = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
            hasReprojection = true;
        }
    }
    if (hasReprojection && all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
        const float4 h = HistoryPrev.SampleLevel(LinearClamp, prevUv, 0.0f);
        history = h.rgb;
        frames  = h.a;
    }

    const float planeThresh = max(length(worldPos - CameraPos.xyz) * 0.05f, 1e-3f);
    float3 cC = RGBToYCoCg(max(current.rgb, 0.0f));
    float3 m1 = cC, m2 = cC * cC;       
    float  wn = 1.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) continue;
        int2 c = clamp(px + int2(x, y), int2(0, 0), int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));
        float cz = Depth.Load(int3(c, 0)).r;
        if (cz <= 0.0f) continue;                         
        float2 cuv  = (c + 0.5f) * ScreenParams.zw;
        float2 cndc = float2(cuv.x * 2.0f - 1.0f, 1.0f - cuv.y * 2.0f);
        float4 cwH  = mul(float4(cndc, cz, 1.0f), InvViewProj);
        float3 cWorld = cwH.xyz / cwH.w;
        if (length(cWorld - worldPos) > planeThresh) continue;   
        float3 s = RGBToYCoCg(max(Resolved.Load(int3(c, 0)).rgb, 0.0f));
        m1 += s; m2 += s * s; wn += 1.0f;
    }
    m1 /= wn; m2 /= wn;
    float3 sigma  = sqrt(max(m2 - m1 * m1, 0.0f));
    float  gamma  = TemporalParams.y;
    float3 boxMin = m1 - gamma * sigma;
    float3 boxMax = m1 + gamma * sigma;
    float3 histYCoCg = clamp(RGBToYCoCg(history), boxMin, boxMax);
    history = max(YCoCgToRGB(histYCoCg), 0.0f);

    float3 curRGB = max(YCoCgToRGB(clamp(RGBToYCoCg(max(current.rgb, 0.0f)), boxMin, boxMax)), 0.0f);

    // A Eq. 25.4 reduz a dependencia temporal em receptores nao planares. Converter a confianca
    // em comprimento de historico preserva o acumulador existente e aumenta alpha suavemente.
    frames = min(frames * historyConfidence + 1.0f, TemporalParams.x);
    float  alpha  = 1.0f / frames;
    float3 result = lerp(history, curRGB, alpha);

    RWHistory[px] = float4(result, frames);
}
