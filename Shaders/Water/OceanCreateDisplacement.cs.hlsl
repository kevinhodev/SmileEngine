#include "OceanFFTCommon.hlsli"

Texture2D<float4>   SpecHD  : register(t0);
RWTexture2D<float4> DispOut : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DISP_MAP_SIZE || id.y >= DISP_MAP_SIZE) return;

    int2 loc = int2(id.xy);

    float sgn = (((loc.x + loc.y) & 1) == 1) ? -1.0f : 1.0f;

    float4 s = SpecHD.Load(int3(loc, 0));
    float  h = sgn * s.x;
    float2 D = sgn * s.zw;

    DispOut[loc] = float4(D.x * ChoppyScale, D.y * ChoppyScale, h * HeightScale, 0.0f);
}
