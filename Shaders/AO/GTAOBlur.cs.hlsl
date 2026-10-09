#include "GTAOCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

Texture2D<float>   DepthTex : register(t0);
Texture2D<float>   AOIn     : register(t1);
Texture2D<float4>  NormalTex : register(t2);
RWTexture2D<float> AOOut    : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // Opera no espaco do alvo AO (full ou meia-res, Params3.z); depth full-res lido
    // no pixel par correspondente (ipx*scale) so p/ o peso bilateral.
    const uint scale = max((uint)Params3.z, 1u);
    const uint W = (uint)ScreenParams.x;
    const uint H = (uint)ScreenParams.y;
    const int  AOW = int((W + scale - 1u) / scale);
    const int  AOH = int((H + scale - 1u) / scale);
    if (tid.x >= (uint)AOW || tid.y >= (uint)AOH) return;

    const int2 ipx = int2(tid.xy);
    const int2 dir = (Params2.w < 0.5f) ? int2(1, 0) : int2(0, 1);

    const int2 fullPx = ipx * int(scale);
    const float rawDepth = DepthTex.Load(int3(fullPx, 0));
    if (SmileIsSky(rawDepth)) { AOOut[tid.xy] = 1.0f; return; }
    const float3 centerP = GTAO_ViewPos(float2(fullPx), rawDepth);
    const float3 centerN = GTAO_ViewNormal(NormalTex.Load(int3(fullPx, 0)).xyz);
    float sum  = AOIn.Load(int3(ipx, 0));
    float wsum = 1.0f;

    [unroll] for (int s = -1; s <= 1; s += 2) {
        float3 previousP = centerP;
        float3 previousN = centerN;
        float pathWeight = 1.0f;
        [unroll] for (int k = 1; k <= 4; ++k) {
            int2 p = ipx + dir * (k * s);
            if (any(p < 0) || any(p >= int2(AOW, AOH))) break;
            int2 fp = p * int(scale);
            float sd = DepthTex.Load(int3(fp, 0));
            if (SmileIsSky(sd)) break;
            float3 sampleP = GTAO_ViewPos(float2(fp), sd);
            float3 sampleN = GTAO_ViewNormal(NormalTex.Load(int3(fp, 0)).xyz);
            // Wide taps must not jump over a thin foreground silhouette.
            pathWeight *= GTAO_BilateralWeight(previousP, previousN, sampleP, sampleN);
            float w = pathWeight * GTAO_BilateralWeight(centerP, centerN, sampleP, sampleN)
                      * (1.0f - float(k) * 0.18f);
            sum  += AOIn.Load(int3(p, 0)) * w;
            wsum += w;
            previousP = sampleP;
            previousN = sampleN;
        }
    }
    AOOut[tid.xy] = sum / max(wsum, 1e-4f);
}
