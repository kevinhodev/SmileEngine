static const float WATER_PI = 3.14159265359f;
static const float WATER_F0 = 0.02f;
static const float WATER_FFT_SIZE = 64.0f;

cbuffer OceanCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 CameraPosition;
    float4 SunDirection; // xyz = direction TO sun, w = intensity
    float4 SunColor;
    float4 WaterParams0; // x=levelY, y=waveAmount, z=waveSize, w=choppiness
    float4 WaterParams1; // x=surfaceExtent, y=normalScale, z=reflScale, w=refrScale
    float4 WaterParams2; // x=foamAmount, y=fogDensity, z=time, w=unused
    float4 GridOrigin;   // x=centerX, z=centerZ, w=cellSize
    float4 DeepColor;
    float4 ShallowColor;
    float4 IBLParams;    // x=intensity, y=rotation, z=maxMip, w=enabled
};

Texture2D<float4> OceanDisplacement : register(t0);
TextureCube<float4> IrradianceMap   : register(t1);
TextureCube<float4> PrefilteredMap  : register(t2);
Texture2D<float2> BRDFLut           : register(t3);

SamplerState WaterWrapSampler : register(s0);
SamplerState WaterClampSampler : register(s1);

float3 RotateY(float3 v, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float2 OceanUV(float2 worldXZ) {
    return worldXZ / max(WaterParams0.z, 1.0f);
}

float4 SampleOceanDisplacement(float2 worldXZ) {
    return OceanDisplacement.SampleLevel(WaterWrapSampler, OceanUV(worldXZ), 0.0f);
}

float3 ComputeOceanNormal(float2 worldXZ) {
    float worldStep = max(WaterParams0.z / WATER_FFT_SIZE, 0.001f);
    float2 dx = float2(worldStep, 0.0f);
    float2 dz = float2(0.0f, worldStep);

    float hL = SampleOceanDisplacement(worldXZ - dx).y * WaterParams0.y;
    float hR = SampleOceanDisplacement(worldXZ + dx).y * WaterParams0.y;
    float hD = SampleOceanDisplacement(worldXZ - dz).y * WaterParams0.y;
    float hU = SampleOceanDisplacement(worldXZ + dz).y * WaterParams0.y;

    float3 tangentX = float3(2.0f * worldStep, (hR - hL) * WaterParams1.y, 0.0f);
    float3 tangentZ = float3(0.0f, (hU - hD) * WaterParams1.y, 2.0f * worldStep);
    return normalize(cross(tangentZ, tangentX));
}

float SchlickFresnel(float NoV) {
    float Fc = pow(1.0f - saturate(NoV), 5.0f);
    return WATER_F0 + (1.0f - WATER_F0) * Fc;
}
