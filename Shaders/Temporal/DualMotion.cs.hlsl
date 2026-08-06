#include "TemporalMotionCommon.hlsli"

cbuffer TemporalMotionCB : register(b0) {
    row_major float4x4 InvViewProj;
    row_major float4x4 ViewProj;
    row_major float4x4 PrevViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params; // x=frame slot, y=history valid, z=position reject scale, w=instance count
};

Texture2D<float2> RasterVelocity : register(t0);
Texture2D<float4> CurrentSurface : register(t1);
Texture2D<float4> PreviousSurface : register(t2);
StructuredBuffer<FTemporalInstanceTransform> InstanceTransforms : register(t3);
RWTexture2D<float2> ReliableMotion : register(u0);

bool InBounds(float2 uv) { return all(uv > 0.0f) && all(uv < 1.0f); }

float4 LoadSurfaceAtUv(Texture2D<float4> tex, float2 uv) {
    const int2 p = clamp(int2(uv * ScreenParams.xy), int2(0, 0), int2(ScreenParams.xy) - 1);
    return tex.Load(int3(p, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    const float2 baseMotion = RasterVelocity.Load(int3(px, 0));
    ReliableMotion[px] = baseMotion;
    if (Params.y < 0.5f) return;

    const float4 current = CurrentSurface.Load(int3(px, 0));
    uint currentId;
    if (!TemporalDecodeInstance(current.w, (uint)Params.w, currentId)) return;

    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 prevUv = uv - baseMotion;
    if (!InBounds(prevUv)) return;

    const float3 expectedPrevious = TemporalTransformPoint(
        current.xyz, InstanceTransforms[currentId].CurrentToPrevious);
    const float4 previousAtBase = LoadSurfaceAtUv(PreviousSurface, prevUv);
    uint occluderId;
    if (!TemporalDecodeInstance(previousAtBase.w, (uint)Params.w, occluderId)) return;

    const float reject = Params.z * max(length(current.xyz - CameraPos.xyz), 1.0f);
    const float baseError = length(previousAtBase.xyz - expectedPrevious);
    if (baseError <= reject) return; // correspondencia convencional ainda e valida

    // Eq. 25.6: Y e o fetch convencional no frame anterior; Z e o mesmo oclusor levado de
    // volta ao frame atual. A translacao relativa X-Z desloca Y para uma regiao de fundo.
    const float3 occluderCurrent = TemporalTransformPoint(
        previousAtBase.xyz, InstanceTransforms[occluderId].PreviousToCurrent);
    float2 zUv;
    if (!TemporalProjectUv(occluderCurrent, ViewProj, zUv)) return;
    const float2 dualPrevUv = prevUv + (uv - zUv);
    if (!InBounds(dualPrevUv)) return;

    // A validacao no dominio de mundo evita trocar uma desoclusao por outro objeto qualquer.
    const float4 previousAtDual = LoadSurfaceAtUv(PreviousSurface, dualPrevUv);
    uint backgroundId;
    if (!TemporalDecodeInstance(previousAtDual.w, (uint)Params.w, backgroundId)) return;
    const float dualError = length(previousAtDual.xyz - expectedPrevious);
    if (dualError >= min(baseError, reject * 4.0f)) return;

    ReliableMotion[px] = uv - dualPrevUv;
}
