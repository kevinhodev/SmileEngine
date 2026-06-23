cbuffer TAAConstants : register(b0) {
    row_major float4x4 InvViewProj;   
    row_major float4x4 PrevViewProj;  
    float4 Params0;                   
    float4 Params1;                  
    float4 Params2;                   
    float4 CameraPos;                 
};

Texture2D    Input        : register(t0); 
Texture2D    InputHistory : register(t1); 
Texture2D<float> Depth    : register(t2); 
Texture2D    Velocity     : register(t3); 
SamplerState LinearClamp  : register(s0);

float3 RGBToYCoCg(float3 c) {
    return float3(
         0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
         0.5f  * c.r              - 0.5f  * c.b,
        -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float  TaaLuma(float3 c)        { return 0.25f * c.r + 0.5f * c.g + 0.25f * c.b; }
float3 Tonemap(float3 c)        { return c / (1.0f + TaaLuma(c)); }
float3 InvTonemap(float3 c)     { return c / max(1.0f - TaaLuma(c), 1e-4f); }

float3 YCoCgToRGB(float3 c) {
    float t = c.x - c.z; // Y - Cg
    return float3(t + c.y, c.x + c.z, t - c.y);
}

float LinearizeReverseZ(float d, float nearZ, float farZ) {
    return (nearZ * farZ) / (nearZ + d * (farZ - nearZ));
}

float3 ClipToAABB(float3 color, float3 mn, float3 mx) {
    float3 center  = 0.5f * (mx + mn);
    float3 extents = 0.5f * (mx - mn) + 1e-5f;
    float3 shift   = color - center;
    float3 unit    = abs(shift / extents);
    float  maxUnit = max(unit.x, max(unit.y, unit.z));
    return (maxUnit > 1.0f) ? (center + shift / maxUnit) : color;
}

float3 SampleHistoryCatmullRom(float2 uv, float2 texSize, float2 invTexSize) {
    float2 samplePos = uv * texSize;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12     = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 texPos0  = (texPos1 - 1.0f)      * invTexSize;
    float2 texPos3  = (texPos1 + 2.0f)      * invTexSize;
    float2 texPos12 = (texPos1 + offset12)  * invTexSize;

    float3 r = 0.0f;
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos0.x,  texPos0.y),  0).rgb * (w0.x  * w0.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos12.x, texPos0.y),  0).rgb * (w12.x * w0.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos3.x,  texPos0.y),  0).rgb * (w3.x  * w0.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos0.x,  texPos12.y), 0).rgb * (w0.x  * w12.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos12.x, texPos12.y), 0).rgb * (w12.x * w12.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos3.x,  texPos12.y), 0).rgb * (w3.x  * w12.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos0.x,  texPos3.y),  0).rgb * (w0.x  * w3.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos12.x, texPos3.y),  0).rgb * (w12.x * w3.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos3.x,  texPos3.y),  0).rgb * (w3.x  * w3.y);
    return max(r, 0.0f); 
}

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSOutput {
    float4 color : SV_Target0;
    float4 debug : SV_Target1;
};

PSOutput main(VSOutput input) {
    float2 uv  = input.uv;
    float2 inv = Params0.xy;

    float3 m1 = 0.0f; 
    float3 m2 = 0.0f; 
    float3 centerYCoCg = 0.0f;

    float nearZ = Params2.z, farZ = Params2.w;
    float neighDepthMin = 1e30f, neighDepthMax = 0.0f, centerDepth = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float2 tuv = uv + float2(x, y) * inv;
            float3 c = RGBToYCoCg(Tonemap(Input.SampleLevel(LinearClamp, tuv, 0).rgb));
            m1 += c;
            m2 += c * c;
            float dRaw = Depth.SampleLevel(LinearClamp, tuv, 0).r;
            if (dRaw > 0.0f) {
                float dLin    = LinearizeReverseZ(dRaw, nearZ, farZ);
                neighDepthMin = min(neighDepthMin, dLin);
                neighDepthMax = max(neighDepthMax, dLin);
            }
            if (x == 0 && y == 0) { centerYCoCg = c; centerDepth = dRaw; }
        }
    }

    float3 mean  = m1 / 9.0f;
    float3 sigma = sqrt(max(m2 / 9.0f - mean * mean, 0.0f));
    float  gamma = Params1.x;
    float3 boxMin = mean - gamma * sigma;
    float3 boxMax = mean + gamma * sigma;

    float3 curRaw = centerYCoCg;
    centerYCoCg = lerp(centerYCoCg, ClipToAABB(centerYCoCg, boxMin, boxMax), Params2.x);

    float  depth = centerDepth; 
    float2 ndc   = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH    = mul(float4(ndc, depth, 1.0f), InvViewProj);
    float3 world = wH.xyz / wH.w;

    float4 pClip = mul(float4(world, 1.0f), PrevViewProj);
    float2 pNdc  = pClip.xy / pClip.w;
    float2 camPrevUV = float2(pNdc.x * 0.5f + 0.5f, 0.5f - pNdc.y * 0.5f);

    float2 velocity = Velocity.SampleLevel(LinearClamp, uv, 0).rg;
    float2 prevUV   = (depth > 0.0f) ? (uv - velocity) : camPrevUV;

    float onScreen = (prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
                      prevUV.y >= 0.0f && prevUV.y <= 1.0f) ? 1.0f : 0.0f;

    #define kTAAVelScale       0.15f 
    #define kTAAMarginVelScale 0.5f  
    #define kTAAMotionMargin   0.25f 
    #define kDisocclDepthTol   0.1f  

    float velPx  = length((prevUV - uv) / inv);
    float motion = saturate(velPx * kTAAVelScale);
    float blend  = lerp(Params0.z, Params1.w, motion);
    float histW  = blend * Params0.w * onScreen;

    float linCur    = LinearizeReverseZ(depth, nearZ, farZ); 
    float histDepth = InputHistory.SampleLevel(LinearClamp, prevUV, 0).a;
    float outside   = max(max(neighDepthMin - histDepth, histDepth - neighDepthMax), 0.0f);
    float depthMis  = outside / max(histDepth, 1e-3f);
    float disoccl   = (depth > 0.0f) ? saturate(depthMis / kDisocclDepthTol - 1.0f) : 0.0f;
    histW *= (1.0f - disoccl);

    float3 historyRaw = RGBToYCoCg(Tonemap(SampleHistoryCatmullRom(prevUV, 1.0f / inv, inv)));

    float  marginMotion   = saturate(velPx * kTAAMarginVelScale);
    float  edgeContrast   = sigma.x;
    float  aabbMargin     = lerp(Params2.y, kTAAMotionMargin, marginMotion) * edgeContrast;
    float3 historyClamped = ClipToAABB(historyRaw, boxMin - aabbMargin, boxMax + aabbMargin);

    float3 resolvedYCoCg = lerp(centerYCoCg, historyClamped, histW);

    PSOutput o;
    o.color = float4(max(InvTonemap(YCoCgToRGB(resolvedYCoCg)), 0.0f), linCur);

    float3 displayYCoCg = resolvedYCoCg + (resolvedYCoCg - mean) * Params1.y;
    float3 displayRGB   = max(InvTonemap(YCoCgToRGB(displayYCoCg)), 0.0f);

    int dbg = (int)(Params1.z + 0.5f);
    float3 d = displayRGB; 
    if      (dbg == 1) { float px = length((prevUV - uv) / inv); d = float3(0.0f, saturate(px / 4.0f), saturate(px / 4.0f)); }
    else if (dbg == 2) { d = float3(saturate(abs(curRaw.x - historyRaw.x) * 4.0f), 0.0f, 0.0f); }
    else if (dbg == 3) { d = float3(0.0f, 0.0f, saturate(length(historyClamped - historyRaw) * 4.0f)); }
    else if (dbg == 4) { d = histW.xxx; }
    else if (dbg == 5) { d = saturate(sigma.x * 4.0f).xxx; }
    else if (dbg == 6) { d = max(InvTonemap(YCoCgToRGB(historyRaw)), 0.0f); }
    else if (dbg == 7) { d = max(InvTonemap(YCoCgToRGB(curRaw)), 0.0f); }
    o.debug = float4(d, 1.0f);

    return o;
}
