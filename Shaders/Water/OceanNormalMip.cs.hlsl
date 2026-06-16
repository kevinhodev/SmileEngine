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
    float3 sum = float3(0.0f, 0.0f, 0.0f);

    [unroll] for (int oy = 0; oy < 2; ++oy) {
        [unroll] for (int ox = 0; ox < 2; ++ox) {
            int2  sp = min(b + int2(ox, oy), int2(int(sw) - 1, int(sh) - 1));
            float4 n = SrcMip.Load(int3(sp, 0));
            float  T = saturate(n.w);
            sum += n.xyz * T;
        }
    }

    float  len = length(sum);
    float  T   = clamp(len * 0.25f, 1e-4f, 1.0f);
    float3 nrm = (len > 1e-6f) ? (sum / len) : float3(0.0f, 1.0f, 0.0f);

    DstMip[int2(id.xy)] = float4(nrm, T);
}
