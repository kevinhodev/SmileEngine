#ifndef SMILE_SUN_SHAFTS_COMMON
#define SMILE_SUN_SHAFTS_COMMON

cbuffer SunShaftCB : register(b0) {
    row_major float4x4 InvViewProj;
    row_major float4x4 PrevViewProj;
    float4 CameraWorldPos;
    float4 SunDirectionIntensity;
    float4 SunColorPhase;
    float4 FogLayer1;
    float4 FogLayer2;
    float4 GridParams;
    float4 TemporalParams;
};

float ShaftSliceDepth(float sliceCoordinate) {
    float z = saturate(sliceCoordinate / GridParams.z);
    return GridParams.w * z * z;
}

float3 ShaftWorldPosition(uint3 gridCoordinate, float3 cellOffset) {
    float2 uv = (float2(gridCoordinate.xy) + cellOffset.xy) / GridParams.xy;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farH = mul(float4(ndc, 0.0f, 1.0f), InvViewProj);
    float3 farWorld = farH.xyz / max(farH.w, 1e-6f);
    float viewDepth = ShaftSliceDepth(float(gridCoordinate.z) + cellOffset.z);
    return CameraWorldPos.xyz +
           (farWorld - CameraWorldPos.xyz) * (viewDepth / max(FogLayer2.w, 1e-4f));
}

float ShaftDensity(float worldY) {
    float e1 = clamp(-FogLayer1.y * (worldY - FogLayer1.z), -126.0f, 126.0f);
    float e2 = clamp(-FogLayer2.y * (worldY - FogLayer2.z), -126.0f, 126.0f);
    return max(FogLayer1.x * exp2(e1) + FogLayer2.x * exp2(e2), 0.0f);
}

float HenyeyGreenstein(float g, float cosTheta) {
    float gg = g * g;
    float d = max(1.0f + gg - 2.0f * g * cosTheta, 1e-4f);
    return (1.0f - gg) / (12.5663706f * d * sqrt(d));
}

#endif
