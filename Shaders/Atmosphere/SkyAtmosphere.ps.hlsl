#include "AtmosphereCommon.hlsli"

Texture2D<float4> SkyViewLUT       : register(t0);
Texture2D<float4> TransmittanceLUT : register(t1);
Texture2D<float4> MoonTex          : register(t2); 

struct PSInput {
    float4 pos    : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

float3 Hash33(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float3 StarField(float3 dir, float time) {
    float a = time * 0.004f;
    float c = cos(a), s = sin(a);
    float3 d = float3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

    const float density = 200.0f;
    float3 cell   = floor(d * density);
    float3 rnd    = Hash33(cell);
    float  present = step(0.992f, rnd.x);              
    float3 starDir = normalize(cell + 0.5f + (rnd - 0.5f) * 0.8f); 

    float3 delta = starDir - d;
    float  core  = exp(-dot(delta, delta) * 1.2e6f);  
    float  bright = 0.15f + 0.45f * rnd.y;            
    float  twinkle = 0.65f + 0.35f * sin(time * 3.0f + rnd.z * 6.2831853f);
    float3 tint = lerp(float3(1.0f, 0.82f, 0.65f), float3(0.7f, 0.8f, 1.0f), rnd.z);
    return present * core * bright * twinkle * tint;
}

float3 MoonDisk(float3 viewDir, float3 moonDir, float cosRadius, float3 sunDir, float brightness) {
    float cosToMoon = dot(viewDir, moonDir);
    if (cosToMoon <= cosRadius) return float3(0.0f, 0.0f, 0.0f);

    float3 up0   = abs(moonDir.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up0, moonDir));
    float3 up    = cross(moonDir, right);

    float  rad = sqrt(max(1.0f - cosRadius * cosRadius, 1e-6f)); 
    float2 uv  = float2(dot(viewDir, right), dot(viewDir, up)) / rad; 
    float  rr  = dot(uv, uv);
    if (rr > 1.0f) return float3(0.0f, 0.0f, 0.0f);

    float  z      = sqrt(max(1.0f - rr, 0.0f));
    float3 fwd    = -moonDir;                            
    float3 normal = normalize(right * uv.x + up * uv.y + fwd * z);

    float  nx  = dot(normal, right), ny = dot(normal, up), nz = dot(normal, fwd);
    float2 muv = float2(0.5f + atan2(nx, nz) / (2.0f * PI),
                        0.5f - asin(clamp(ny, -1.0f, 1.0f)) / PI);
    float3 albedo = MoonTex.SampleLevel(LinearClampSampler, muv, 0.0f).rgb;
    albedo = pow(max(albedo, 0.0f), 2.2f);              
    albedo = max((float3)0.0f, 0.13f + (albedo - 0.13f) * 1.5f);

    float  ndl        = dot(normal, sunDir);
    float  lit        = smoothstep(-0.05f, 0.18f, ndl); 
    float  earthshine = 0.03f;                        
    float  edge       = smoothstep(1.0f, 0.92f, rr);    
    return (lit + earthshine) * edge * brightness * albedo;
}

float4 main(PSInput input) : SV_TARGET {
    float4 worldFar = mul(float4(input.clipXY, 1.0f, 1.0f), InvViewProjNoTrans);
    float3 viewDir  = normalize(worldFar.xyz);

    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 sunDir   = normalize(SunDir.xyz);

    float viewZenithCos = dot(viewDir, up);

    float3 viewHoriz = viewDir - up * viewZenithCos;
    float3 sunHoriz  = sunDir  - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));

    float2 uv = SkyViewParamsToUv(viewZenithCos, lightViewCos, kViewHeight);
    float3 L  = SkyViewLUT.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;

    float  cosToSun = dot(viewDir, sunDir);
    float3 sunTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, dot(up, sunDir));

    float core = smoothstep(kSunDiskCos, lerp(kSunDiskCos, 1.0f, 0.5f), cosToSun);
    float glare = pow(saturate(cosToSun), 350.0f);

    L += sunTrans * (kSunDiskInt * core + kSunGlareInt * glare);

    float night = MoonParams.z;
    if (night > 0.001f) {
        float3 viewTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, viewZenithCos);
        float  aboveHorizon = smoothstep(-0.02f, 0.06f, viewDir.y);

        float3 stars = StarField(viewDir, MoonParams.w) * MoonParams.y;
        float3 moon  = MoonDisk(viewDir, MoonDir.xyz, MoonDir.w, sunDir, MoonParams.x);

        L += (stars + moon) * viewTrans * (night * aboveHorizon);
    }

    return float4(L, 1.0f);
}