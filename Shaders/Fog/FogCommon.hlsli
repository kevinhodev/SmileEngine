#ifndef SMILE_FOG_COMMON_HLSLI
#define SMILE_FOG_COMMON_HLSLI

// Matematica pura da atmosfera (sem cbuffer) — daqui sai a conversao direcao -> uv do
// sky-view LUT usada pela ancoragem do height fog. O AtmosphereCommon.hlsli NAO serve: ele
// declara o proprio b0, que colide com o FogCB.
#include "../Atmosphere/AtmosphereMath.hlsli"

cbuffer FogCB : register(b0) {
    float4 ExponentialFogParameters;     
    float4 ExponentialFogParameters2;    
    float4 ExponentialFogParameters3;   
    float4 FogInscatteringColor;        
    float4 DirectionalInscatteringColor; 
    float4 InscatteringLightDirection;   
    row_major float4x4 InvViewProj;
    float4 CameraWorldPos;
    float4 AerialParams;
    float4 ScreenParams;
    float4 DepthParams;
    float4 VolFogParams;   // froxel fog: x=B, y=O, z=S (slice = log2(z*B+O)*S), w=GridSizeZ
    float4 VolFogParams2;  // x = alcance (m), y = ligado (>0.5), zw unused
    float4 CamForwardVF;   // xyz = frente da camera (exclusao do analitico + slice por dist)
    float4 SkyFogParams;   // x = view height (km), y = raio do planeta (km),
                           // z = HeightFogSkyContribution (0 = cor chapada, comportamento antigo)
    float4 SkyFogSunDir;   // xyz = direcao P/ o SOL (o LUT e dobrado no azimute dele)
};

Texture3D<float4> AerialVolume     : register(t1);
// Sun shafts: inscatter direcional volumétrico meia-res (raymarch CSM + temporal).
// Quando AerialParams.w > 0.5, substitui o termo analítico DirectionalInscattering
// (o CPU zera InscatteringLightDirection.w pra desligar o analítico junto).
Texture2D<float4> VolumetricShafts : register(t2);
// Froxel volumetric fog integrado (VolumetricFogIntegrate.cs): rgb = inscatter
// acumulado ate o slice, a = transmitancia. Cobre 0..VolFogParams2.x; alem disso
// o height fog analitico continua (excluido do range coberto, receita da UE).
Texture3D<float4> VolumetricFogTex : register(t3);
// Sky-view LUT da atmosfera — a MESMA textura que o sky PS amostra. Ver
// GetExponentialHeightFog: o inscatter do fog converge p/ a cor do ceu naquela direcao.
Texture2D<float4> SkyViewLUT       : register(t4);
SamplerState      LinearClampSampler : register(s0);

float CalculateLineIntegralShared(float falloff, float rayDirZ, float rayOriginTerms) {
    float Falloff = max(-127.0f, falloff * rayDirZ);
    float LineIntegral       = (1.0f - exp2(-Falloff)) / Falloff;
    float LineIntegralTaylor = 0.6931472f - 0.5f * (0.6931472f * 0.6931472f) * Falloff;
    return rayOriginTerms * (abs(Falloff) > 0.01f ? LineIntegral : LineIntegralTaylor);
}

// excludeDist: alem do StartDistance do usuario, exclui o trecho coberto pelo froxel
// volumetric fog (0 = sem exclusao). Com o alvo DENTRO do range excluido, o rayLength
// fica negativo e o saturate(exp2(...)) zera o fog analitico — mesmo comportamento da UE.
float4 GetExponentialHeightFog(float3 worldPosRelCam, float excludeDist) {
    const float MinFogOpacity = FogInscatteringColor.w;

    float3 c2r = worldPosRelCam;
    float  len = max(length(c2r), 1e-4f);
    float3 dir = c2r / len;

    float rayLength       = len;
    float rayDirZ         = c2r.y;
    float rayOriginTerms  = ExponentialFogParameters.x;
    float rayOriginTerms2 = ExponentialFogParameters2.x;

    float startDist = max(ExponentialFogParameters.w, excludeDist);
    // SEM guard de len > startDist (igual a UE): alvo dentro do range excluido da
    // excludeT > 1 -> rayLength negativo -> saturate(exp2(-integral)) = 1 = sem fog.
    if (startDist > 0.0f) {
        float excludeT = startDist / len;
        float startY   = CameraWorldPos.y + excludeT * c2r.y;
        rayLength      = (1.0f - excludeT) * len;
        rayDirZ        = c2r.y - excludeT * c2r.y;
        float exp1     = max(-127.0f, ExponentialFogParameters.y  * (startY - ExponentialFogParameters3.y));
        float exp2t    = max(-127.0f, ExponentialFogParameters2.y * (startY - ExponentialFogParameters2.w));
        rayOriginTerms  = ExponentialFogParameters3.x * exp2(-exp1);  
        rayOriginTerms2 = ExponentialFogParameters2.z * exp2(-exp2t); 
    }

    float shared1 = CalculateLineIntegralShared(ExponentialFogParameters.y,  rayDirZ, rayOriginTerms);
    float shared2 = CalculateLineIntegralShared(ExponentialFogParameters2.y, rayDirZ, rayOriginTerms2);
    float sharedIntegral = shared1 + shared2;

    float lineIntegral = sharedIntegral * rayLength;
    float expFogFactor = max(saturate(exp2(-lineIntegral)), MinFogOpacity);
    
    float3 dirInscatter = float3(0.0f, 0.0f, 0.0f);
    if (InscatteringLightDirection.w >= 0.0f) {
        float  d = saturate(dot(dir, InscatteringLightDirection.xyz));
        float3 dirColor = DirectionalInscatteringColor.rgb * pow(d, DirectionalInscatteringColor.w);
        float  dirStart = InscatteringLightDirection.w;
        float  dirIntegral = sharedIntegral * max(rayLength - dirStart, 0.0f);
        float  dirFogFactor = saturate(exp2(-dirIntegral));
        dirInscatter = dirColor * (1.0f - dirFogFactor);
    }

    float cutoff = ExponentialFogParameters3.w;
    if (cutoff > 0.0f && len > cutoff) {
        expFogFactor = 1.0f;
        dirInscatter = float3(0.0f, 0.0f, 0.0f);
    }

    // ANCORAGEM NA ATMOSFERA (HeightFogContribution da UE, SkyAtmosphereCommon.ush:356-365
    // consumido em HeightFogCommon.ush:190).
    //
    // Com a cor chapada, os dois lados da linha do horizonte convergiam p/ numeros sem relacao
    // nenhuma: acima, o SkyView LUT na direcao do horizonte (que segue o TOD); abaixo,
    // FogInscatteringColor.rgb * (1 - MinFogOpacity) — uma CONSTANTE. De dia (0.42,0.55,0.70)
    // x 0.85 fica perto do ceu e passa despercebido; de noite o ceu no horizonte vale ~0.005 e
    // a agua distante saturava em ~0.5: 100x mais clara, a faixa clara na linha do oceano.
    //
    // Amostrar o MESMO LUT que o sky PS resolve por construcao: no limite de distancia a
    // geometria converge p/ o mesmo valor do pixel de ceu logo acima, em qualquer hora do dia.
    // Nao ha fator de escala — o sky PS escreve o valor cru do LUT no HDR, entao as duas
    // pontas ja vivem na mesma unidade.
    //
    // Contribuicao 0 preserva o comportamento historico bit a bit (e o botao do A/B).
    float3 fogTint = FogInscatteringColor.rgb;
    if (SkyFogParams.z > 0.0f) {
        float2 skyUv  = AtmoWorldDirToSkyViewUv(dir, SkyFogSunDir.xyz,
                                                SkyFogParams.x, SkyFogParams.y);
        float3 fogSky = SkyViewLUT.SampleLevel(LinearClampSampler, skyUv, 0.0f).rgb;
        fogTint = lerp(FogInscatteringColor.rgb, fogSky, SkyFogParams.z);
    }

    float3 fogColor = fogTint * (1.0f - expFogFactor) + dirInscatter;
    return float4(fogColor, expFogFactor);
}

float4 SampleAerialPerspective(float2 screenUV, float tDepthKm, float apDepthKm) {
    float linW    = saturate(tDepthKm / max(apDepthKm, 1e-4f));
    float nonLinW = sqrt(linW);

    // Contagem de slices do CB (AerialParams.y), nao mais um 16.0 literal que so por acaso
    // batia com o kAerialSlices do bake.
    const float Slices = max(AerialParams.y, 1.0f);
    const float HalfSliceDepth = 0.70710678f;
    float nonLinSlice = nonLinW * Slices;
    float weight = 1.0f;
    if (nonLinSlice < HalfSliceDepth)
        weight = saturate(nonLinSlice * nonLinSlice * 2.0f);

    float4 ap = AerialVolume.SampleLevel(LinearClampSampler, float3(screenUV, nonLinW), 0.0f);
    ap.rgb *= weight;
    ap.a    = 1.0f - weight * (1.0f - ap.a);
    return ap; 
}

#endif 