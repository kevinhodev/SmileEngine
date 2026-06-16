#include "GTAOCommon.hlsli"

Texture2D<float>   DepthTex : register(t0);
Texture2D<float>   AOIn     : register(t1);
RWTexture2D<float> AOOut    : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint W = (uint)ScreenParams.x;
    const uint H = (uint)ScreenParams.y;
    if (tid.x >= W || tid.y >= H) return;

    const int2 ipx = int2(tid.xy);
    const int2 dir = (Params2.w < 0.5f) ? int2(1, 0) : int2(0, 1);

    const float cd = GTAO_LinearizeDepth(DepthTex.Load(int3(ipx, 0)));
    float sum  = AOIn.Load(int3(ipx, 0));
    float wsum = 1.0f;

    [unroll] for (int k = 1; k <= 4; ++k) {
        [unroll] for (int s = -1; s <= 1; s += 2) {
            int2 p = clamp(ipx + dir * (k * s), int2(0, 0), int2((int)W - 1, (int)H - 1));
            float dd = GTAO_LinearizeDepth(DepthTex.Load(int3(p, 0)));
 
            float wd = exp2(-abs(dd - cd) * 16.0f / max(cd, 1.0f));
            float w  = wd * (1.0f - float(k) * 0.18f);
            sum  += AOIn.Load(int3(p, 0)) * w;
            wsum += w;
        }
    }
    AOOut[tid.xy] = sum / max(wsum, 1e-4f);
}
