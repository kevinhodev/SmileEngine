#include "../GI/DDGICommon.hlsli"

cbuffer TemporalMotionCB : register(b0) {
    row_major float4x4 InvViewProj;
    row_major float4x4 ViewProj;
    row_major float4x4 PrevViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params;
};

Texture2D<float> Depth : register(t0);
RaytracingAccelerationStructure Scene : register(t1);
StructuredBuffer<InstanceGeo> Instances : register(t2);
RWTexture2D<float4> CurrentSurface : register(u0);

SamplerState LinearWrap : register(s1);
#include "../GI/RTAlphaTest.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    const float deviceZ = Depth.Load(int3(px, 0));
    if (deviceZ <= 0.0f) { CurrentSurface[px] = 0.0f; return; }

    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 worldPos = wh.xyz / wh.w;

    const float3 toSurface = worldPos - CameraPos.xyz;
    const float distanceToSurface = length(toSurface);
    if (distanceToSurface <= 1.0e-5f) {
        CurrentSurface[px] = float4(worldPos, 0.0f);
        return;
    }

    RayDesc ray;
    ray.Origin = CameraPos.xyz;
    ray.Direction = toSurface / distanceToSurface;
    ray.TMin = 0.0f;
    ray.TMax = distanceToSurface + max(0.01f, distanceToSurface * 1.0e-3f);
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                         SMILE_RT_MASK_GATHER, ray);
    SMILE_RT_PROCEED(query)

    float encodedInstance = 0.0f;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        encodedInstance = (float)(query.CommittedInstanceID() + 1u);
    CurrentSurface[px] = float4(worldPos, encodedInstance);
}
