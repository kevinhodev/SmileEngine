#include "SunShaftsCommon.hlsli"

cbuffer CSMCB : register(b1) {
    row_major float4x4 WorldToShadow[4];
    float4 CSMTexelWorld;
    float4 CSMParams;
    float4 CSMParams2;
    float4 CSMParams3;
    float4 CSMBiasScale;
    float4 CSMDepthRangeWorld;
};

Texture2DArray<float> SunShadowMap : register(t0);
Texture3D<float4> HistoryVolume : register(t1);
SamplerState LinearClamp : register(s0);
SamplerComparisonState ShadowCmp : register(s1);
RWTexture3D<float4> OutScattering : register(u0);

bool CSMInBounds(float3 uvz) {
    return all(uvz > 0.0f) && all(uvz < 1.0f);
}

float SampleCascade(float3 uvz, int cascade) {
    float refZ = uvz.z - CSMParams.y * CSMBiasScale[cascade];
    return SunShadowMap.SampleCmpLevelZero(ShadowCmp, float3(uvz.xy, float(cascade)), refZ);
}

float SampleVolumetricCSM(float3 worldPosition) {
    if (CSMParams.w < 0.5f)
        return 1.0f;

    int count = min((int)CSMParams.x, 4);
    [loop] for (int cascade = 0; cascade < count; ++cascade) {
        float3 uvz = mul(float4(worldPosition, 1.0f), WorldToShadow[cascade]).xyz;
        if (!CSMInBounds(uvz))
            continue;

        float visibility = SampleCascade(uvz, cascade);
        float band = CSMParams2.z;
        if (band > 0.0f) {
            float edge = min(min(uvz.x, 1.0f - uvz.x), min(uvz.y, 1.0f - uvz.y));
            if (edge < band) {
                float outerVisibility = 1.0f;
                if (cascade + 1 < count) {
                    float3 outerUVZ = mul(float4(worldPosition, 1.0f),
                                          WorldToShadow[cascade + 1]).xyz;
                    if (CSMInBounds(outerUVZ))
                        outerVisibility = SampleCascade(outerUVZ, cascade + 1);
                }
                visibility = lerp(outerVisibility, visibility, saturate(edge / band));
            }
        }
        return visibility;
    }
    return 1.0f;
}

float4 EvaluateCell(uint3 gridCoordinate, float3 cellOffset) {
    float3 worldPosition = ShaftWorldPosition(gridCoordinate, cellOffset);
    float3 viewRay = normalize(worldPosition - CameraWorldPos.xyz);
    float density = ShaftDensity(worldPosition.y);
    float visibility = SampleVolumetricCSM(worldPosition);
    float phase = HenyeyGreenstein(SunColorPhase.w,
                                   dot(SunDirectionIntensity.xyz, viewRay));

    const float singleScatteringAlbedo = 0.90f;
    float3 incident = max(SunColorPhase.rgb * SunDirectionIntensity.w, 0.0f);
    float3 scattering = incident * (density * singleScatteringAlbedo * phase * visibility);
    return float4(scattering, density);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id >= uint3(GridParams.xyz)))
        return;

    bool historyValid = TemporalParams.w > 0.5f;
    uint sampleCount = historyValid ? 1u : 4u;
    static const float3 startupOffsets[4] = {
        float3(0.125f, 0.625f, 0.375f),
        float3(0.625f, 0.125f, 0.875f),
        float3(0.375f, 0.875f, 0.125f),
        float3(0.875f, 0.375f, 0.625f)
    };

    float4 current = 0.0f;
    [loop] for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        float3 offset = historyValid
            ? TemporalParams.xyz
            : frac(startupOffsets[sampleIndex] + TemporalParams.xyz);
        current += EvaluateCell(id, offset);
    }
    current /= float(sampleCount);

    if (historyValid) {
        float3 centerWorld = ShaftWorldPosition(id, 0.5f);
        float4 previousClip = mul(float4(centerWorld, 1.0f), PrevViewProj);
        if (previousClip.w > 0.0f) {
            float2 previousUV = previousClip.xy / previousClip.w;
            previousUV = previousUV * float2(0.5f, -0.5f) + 0.5f;
            float previousZ = sqrt(saturate(previousClip.w / GridParams.w));
            float3 previousUVW = float3(previousUV, previousZ);
            if (all(previousUVW >= 0.0f) && all(previousUVW <= 1.0f)) {
                float4 history = HistoryVolume.SampleLevel(LinearClamp, previousUVW, 0.0f);
                current = lerp(current, history, CameraWorldPos.w);
            }
        }
    }

    if (any(isnan(current)) || any(isinf(current)))
        current = 0.0f;
    OutScattering[id] = max(current, 0.0f);
}
