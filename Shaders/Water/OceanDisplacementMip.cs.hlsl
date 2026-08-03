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
    const int2 base = int2(id.xy) * 2;
    float4 sum = 0.0f;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            const int2 p = min(base + int2(x, y), int2(int(sw) - 1, int(sh) - 1));
            sum += SrcMip.Load(int3(p, 0));
        }
    }
    DstMip[int2(id.xy)] = sum * 0.25f;
}
