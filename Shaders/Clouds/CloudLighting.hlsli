#ifndef SMILE_CLOUD_LIGHTING_HLSLI
#define SMILE_CLOUD_LIGHTING_HLSLI

static const float kCloudPI = 3.14159265358979f;

float HenyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * kCloudPI * pow(max(denom, 1e-4f), 1.5f));
}

float DualLobeHG(float cosTheta, float g1, float g2, float blend) {
    return lerp(HenyeyGreenstein(cosTheta, g1), HenyeyGreenstein(cosTheta, g2), blend);
}

float PowderTerm(float density, float strength) {
    float powder = 1.0f - exp(-density * 2.0f);
    return lerp(1.0f, powder, strength);
}

float2 CloudTransmittanceUv(float viewHeight, float viewZenithCos, float bottomR, float topR) {
    float H   = sqrt(max(0.0f, topR * topR - bottomR * bottomR));
    float rho = sqrt(max(0.0f, viewHeight * viewHeight - bottomR * bottomR));
    float disc = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0f) + topR * topR;
    float d = max(0.0f, -viewHeight * viewZenithCos + sqrt(max(0.0f, disc)));
    float dMin = topR - viewHeight;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-5f);
    float xR  = rho / max(H, 1e-5f);
    return float2(xMu, xR);
}

float3 SampleAtmoTransmittance(Texture2D<float4> lut, SamplerState samp,
                               float height, float sunZenithCos, float bottomR, float topR) {
    return lut.SampleLevel(samp, CloudTransmittanceUv(height, sunZenithCos, bottomR, topR), 0.0f).rgb;
}

float3 SampleAtmoMultiScatter(Texture2D<float4> lut, SamplerState samp,
                              float height, float sunZenithCos, float bottomR, float topR) {
    float u = saturate(sunZenithCos * 0.5f + 0.5f);
    float v = saturate((height - bottomR) / max(topR - bottomR, 1e-4f));
    return lut.SampleLevel(samp, float2(u, v), 0.0f).rgb;
}

#endif
