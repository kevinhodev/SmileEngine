#ifndef SMILE_VOLUMETRIC_FOG_COMMON_HLSLI
#define SMILE_VOLUMETRIC_FOG_COMMON_HLSLI

// Froxel volumetric fog (estilo UE: MaterialSetup -> LightScattering -> FinalIntegration).
// Grid FIXO (kVolFogW x kVolFogH x kVolFogZ) alinhado ao frustum: XY = UV de tela,
// Z = view-depth com distribuicao log (CalculateGridZParams da UE) ate MaxDistance.
// A densidade e a MESMA 2-exponencial do height fog (colapsada na camera, x0.6931
// converte exp2 -> natural) — a transmitancia emenda com o analitico alem do alcance.

#define kVolFogW 160
#define kVolFogH 90
#define kVolFogZ 64

cbuffer VolFogCB : register(b0) {
    float4 GridZParams;    // x=B, y=O, z=S (slice = log2(z*B+O)*S), w = GridSizeZ
    float4 GridSize;       // xyz = dims do grid (float), w = frame (jitter futuro, F2)
    row_major float4x4 InvViewProj; // SEM jitter, com translacao
    float4 CameraWorldPos; // xyz = camera, w = extinction scale
    float4 CamForward;     // xyz = frente da camera, w = MaxDistance (m)
    float4 SunDirPhase;    // xyz = dir PARA a key light, w = g da fase HG
    float4 SunColorInt;    // rgb = cor x intensidade da key light, w unused
    float4 FogDensityP;    // x = dens1 colapsada na camera (exp2), y = falloff1,
                           // z = dens2 colapsada, w = falloff2
    float4 AlbedoAmb;      // rgb = albedo do meio, w = intensidade do ambiente
    float4 DDGIGridMin;    // xyz = origem do grid DDGI, w = spacing
    float4 DDGIGridCount;  // xyz = probes por eixo, w = DDGI on (>0.5)
    float4 DDGIParams;     // x = intensidade GI, y = tile, z = atlasW, w = atlasH
    float4 AmbientFallback;// rgb = SkyAmbient da atmosfera (DDGI off), w unused
    // F2 — temporal: reprojeta o CENTRO do voxel pelo VP unjittered do frame anterior
    // e mistura com a história (ping-pong); jitter Halton integra ao longo dos frames.
    row_major float4x4 PrevViewProj; // frame anterior, SEM jitter
    float4 PrevCamPos;     // xyz = camera do frame anterior
    float4 PrevCamForward; // xyz = frente do frame anterior (slice da história em view-Z)
    float4 TemporalParams; // x = peso da história (0 = inválida/off), y = amostras em
                           // history-miss, zw unused
    float4 FrameJitterOffsets[8]; // xyz = offset do voxel por amostra (Halton 2/3/5)
    // F3 — luzes puntuais (mesma lista FGPULight do deferred, root SRV):
    float4 LightParamsVF;  // x = nº de luzes, y = 1/res do atlas spot, z = depth bias (m),
                           // w = intensidade das luzes no fog (0 desliga o loop)
    float4 LightParamsVF2; // x = near das projecoes locais, yzw unused
};

// Profundidade (view-Z linear) do slice — inverso de VolFog_SliceFromDepth.
float VolFog_DepthFromSlice(float slice) {
    return (exp2(slice / GridZParams.z) - GridZParams.y) / GridZParams.x;
}

float VolFog_SliceFromDepth(float viewZ) {
    return log2(viewZ * GridZParams.x + GridZParams.y) * GridZParams.z;
}

// Posicao em mundo do froxel (offset em [0,1]^3 dentro da celula) + view-depth do slice.
// Reconstrucao por raio: NDC xy -> direcao, avanca viewZ/cos(angulo com a frente).
float3 VolFog_WorldPos(uint3 cell, float3 cellOffset, out float viewZ) {
    float2 uv  = (float2(cell.xy) + cellOffset.xy) / GridSize.xy;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 pH  = mul(float4(ndc, 0.5f, 1.0f), InvViewProj);
    float3 dir = normalize(pH.xyz / pH.w - CameraWorldPos.xyz);

    viewZ = VolFog_DepthFromSlice(max((float)cell.z + cellOffset.z, 0.0f));
    float cosA = max(dot(dir, CamForward.xyz), 0.1f);
    return CameraWorldPos.xyz + dir * (viewZ / cosA);
}

// Densidade do height fog na altura do ponto (2-exponencial colapsada na camera;
// mesma conta do raymarch dos sun shafts). Retorna extincao em unidades NATURAIS.
float VolFog_Extinction(float3 worldPos) {
    float dh = worldPos.y - CameraWorldPos.y;
    float dens = FogDensityP.x * exp2(-FogDensityP.y * dh) +
                 FogDensityP.z * exp2(-FogDensityP.w * dh);
    return max(dens * 0.6931472f * CameraWorldPos.w, 0.0f);
}

// Henyey-Greenstein (mesma dos sun shafts).
float VolFog_PhaseHG(float g, float cosTheta) {
    return (1.0f - g * g) /
           (12.566371f * pow(abs(1.0f + g * g - 2.0f * g * cosTheta), 1.5f));
}

#endif
