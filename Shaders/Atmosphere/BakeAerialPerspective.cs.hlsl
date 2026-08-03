#include "AtmosphereCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

Texture2D<float4>   TransmittanceLUT : register(t0);
Texture2D<float4>   MultiScatterLUT  : register(t1);
RWTexture3D<float4> OutAerial        : register(u0);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 VolWH = float2(32.0f, 32.0f);
    const float  Slices = max(kAerialSlices, 1.0f);
    if (id.x >= 32u || id.y >= 32u || (float)id.z >= Slices) return;

    float2 uv  = (float2(id.xy) + 0.5f) / VolWH;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farH = mul(float4(ndc, SMILE_NDC_FAR, 1.0f), InvViewProj); 
    float3 farW = farH.xyz / farH.w;
    float3 worldDir = normalize(farW - CameraWorldPos.xyz);
    
    float sliceN = ((float)id.z + 0.5f) / Slices;
    sliceN *= sliceN;                               
    float startKm = kAerialStartKm;
    float tKm     = startKm + sliceN * (kAerialDepthKm - startKm); 

    // MESMA origem do BakeSkyView.cs.hlsl: o view height unico (kViewHeight), sobre o eixo Y,
    // com XZ ZERADOS. Duas correcoes num gesto so.
    //
    // (1) Reproduzir a formula a mao aqui deixava o valor divergir do sky-view — o piso do
    //     clamp do view height (0,05 km) nao existia nesta conta, entao os dois bakes usavam
    //     raios diferentes. Agora ha um valor so, calculado na CPU.
    //
    // (2) O XZ absoluto de mundo somava uma altitude FANTASMA de (X²+Z²)/2R e inclinava o "up"
    //     da atmosfera junto: 8 cm a 1 km da origem, mas 786 m a 100 km. O aerial perspective
    //     mudava de cor conforme a camera se afastava da origem da cena. Direcoes sao
    //     invariantes a translacao e o meio e esfericamente simetrico, entao zerar XZ nao muda
    //     mais nada no shader — worldDir ja vem do InvViewProj e continua igual.
    float3 camKm = float3(0.0f, kViewHeight, 0.0f);

    float3 sunDir = normalize(SunDir.xyz);

    int   steps = max(1, (int)(((float)id.z + 1.0f) * max(kAerialSamples, 1.0f)));
    float dt    = tKm / (float)steps;

    float cosSunView = dot(worldDir, sunDir);
    float rPhase = RayleighPhase(cosSunView);
    float mPhase = MiePhaseHG(kMiePhaseG, cosSunView);

    // Lua = 2a luz atmosferica. Aqui os froxels vivem em world-space (sem a dobra azimutal do
    // sky-view LUT), entao a lua usa as fases direcionais completas.
    float3 moonDir     = normalize(MoonDir.xyz);
    float  cosMoonView = dot(worldDir, moonDir);
    float  rPhaseMoon  = RayleighPhase(cosMoonView);
    float  mPhaseMoon  = MiePhaseHG(kMiePhaseG, cosMoonView);

    float3 L            = float3(0.0f, 0.0f, 0.0f);
    float3 transmittance = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (int i = 0; i < steps; ++i) {
        float  t   = (i + 0.5f) * dt;
        float3 p   = camKm + t * worldDir;
        float  hgt = length(p);
        float  alt = hgt - kBottomR;
        float3 up  = p / max(hgt, 1e-4f);

        float3 rS, mS, ext;
        SampleMedium(alt, rS, mS, ext);
        float3 safeExt = max(ext, 1e-6f);
        float3 sampleT = exp(-ext * dt);

        float  sunZen   = dot(up, sunDir);
        float3 sunTrans = SampleTransmittanceToTop(TransmittanceLUT, hgt, sunZen);
        float  earthShadow = (RaySphereNearest(p, sunDir, kBottomR) > 0.0f) ? 0.0f : 1.0f;

        float3 phaseScatter = rS * rPhase + mS * mPhase;
        float3 psiMs        = SampleMultiScatterLUT(MultiScatterLUT, hgt, sunZen);
        float3 multiScatter = (rS + mS) * psiMs;

        float3 inScatter = kSunIlluminance *
                           (earthShadow * sunTrans * phaseScatter + multiScatter);

        if (kMoonSkyIll > 0.0f) {
            float  moonZen    = dot(up, moonDir);
            float3 moonTrans  = SampleTransmittanceToTop(TransmittanceLUT, hgt, moonZen);
            float  moonShadow = (RaySphereNearest(p, moonDir, kBottomR) > 0.0f) ? 0.0f : 1.0f;
            float3 psiMsMoon  = SampleMultiScatterLUT(MultiScatterLUT, hgt, moonZen);
            float3 inScatterM = moonShadow * moonTrans * (rS * rPhaseMoon + mS * mPhaseMoon)
                              + (rS + mS) * psiMsMoon;
            inScatter += kMoonSkyIll * inScatterM;
        }

        float3 sInt      = (inScatter - inScatter * sampleT) / safeExt;
        L += transmittance * sInt;

        transmittance *= sampleT;
    }

    float avgT = dot(transmittance, float3(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f));
    OutAerial[id] = float4(L, avgT);
}
