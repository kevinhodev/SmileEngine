#include "../../../Shaders/GI/DDGICommon.hlsli"
#include "../../../Shaders/Reflections/GGXSample.hlsli"
#include "../../../Shaders/GI/ReSTIRReservoir.hlsli"
RWStructuredBuffer<float4> Out : register(u0);
[numthreads(1,1,1)] void main() {
  Out[0] = float4(ResUnpackNormal(ResPackNormal(float3(0,0,-1))), 0);
  Out[1] = float4(ResUnpackNormal(ResPackNormal(normalize(float3(1,0,-1)))), 0);
}
