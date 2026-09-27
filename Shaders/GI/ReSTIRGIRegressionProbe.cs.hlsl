#include "DDGICommon.hlsli"
#include "../GBuffer.hlsli"
#include "../Reflections/GGXSample.hlsli"
#include "ReSTIRReservoir.hlsli"
#include "ReSTIRGIReconstruction.hlsli"
#include "../Lighting/MeshLightCoverage.hlsli"
cbuffer Params : register(b0) { uint CaseCount; };
StructuredBuffer<float4> Cases : register(t0);
StructuredBuffer<FMeshLightTask> Tasks : register(t1);
StructuredBuffer<FTriangleLightGPU> Lights : register(t2);
RWStructuredBuffer<float4> Result : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= CaseCount) return;
    float4 c = Cases[id.x];
    float4 value = 0;
    if (c.w == 0) value.xyz = ResUnpackNormal(ResPackNormal(c.xyz));
    if (c.w == 1) value.xyz = GBuffer_OctDecode(round(GBuffer_OctEncode(c.xyz) * 1023.0f) / 1023.0f);
    if (c.w == 2) value.xyz = DDGI_OctDecode(DDGI_OctEncode(c.xyz));
    if (c.w == 3) {
        value = c.y;
        // Reprojection stays geometry-valid but misses every phase: no fresh sample.
        for (uint frame = 0; frame < (uint)c.x; ++frame)
            value = ReSTIRGI_UpdateReconstruction(value, c.z);
    }
    if (c.w == 4) value.x = MeshLight_CoversHit(Tasks, Lights, (uint)c.z, (uint)c.x, (uint)c.y);
    Result[id.x] = value;
}
