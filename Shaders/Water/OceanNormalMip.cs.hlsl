#include "OceanFFTCommon.hlsli"

Texture2D<float4>   SrcMip : register(t0);
RWTexture2D<float4> DstMip : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint dw, dh;
    DstMip.GetDimensions(dw, dh);
    if (id.x >= dw || id.y >= dh) return;

    uint sw, sh;
    SrcMip.GetDimensions(sw, sh);

    int2 b = int2(id.xy) * 2;
    float4 moments = 0.0f;

    [unroll] for (int oy = 0; oy < 2; ++oy) {
        [unroll] for (int ox = 0; ox < 2; ++ox) {
            int2  sp = min(b + int2(ox, oy), int2(int(sw) - 1, int(sh) - 1));
            moments += SrcMip.Load(int3(sp, 0));
        }
    }

    DstMip[int2(id.xy)] = moments * 0.25f;
}
