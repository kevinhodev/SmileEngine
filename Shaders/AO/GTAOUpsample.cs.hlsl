#include "GTAOCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

// Upsample bilateral meia-res -> full-res (fecho do caminho half-res, estilo UE
// GTAOUpsamplePS + peso de depth como no blur): bilinear 2x2 sobre o AO meia-res,
// peso de cada tap modulado por normal e distancia aos planos tangentes do pixel
// full-res central. Sem tap da mesma superficie, o fallback e visibilidade branca.

Texture2D<float>   DepthTex : register(t0); // depth full-res
Texture2D<float>   AOIn     : register(t1); // AO meia-res (pos-blur)
Texture2D<float4>  NormalTex : register(t2); // normal geometrica full-res
RWTexture2D<float> AOOut    : register(u0); // AO full-res final

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint scale = max((uint)Params3.z, 1u);
    const uint W = (uint)ScreenParams.x;
    const uint H = (uint)ScreenParams.y;
    if (tid.x >= W || tid.y >= H) return;

    const int2 ipx = int2(tid.xy);
    const float rawD = DepthTex.Load(int3(ipx, 0));
    if (SmileIsSky(rawD)) { AOOut[tid.xy] = 1.0f; return; }
    const float3 centerP = GTAO_ViewPos(float2(ipx), rawD);
    const float3 centerN = GTAO_ViewNormal(NormalTex.Load(int3(ipx, 0)).xyz);

    const int AOW = int((W + scale - 1u) / scale);
    const int AOH = int((H + scale - 1u) / scale);

    // Posicao do pixel full no grid meia-res (texel AO h representa o pixel full h*scale)
    const float2 hf = float2(ipx) / float(scale);
    const int2   h0 = int2(floor(hf));
    const float2 f  = hf - float2(h0);

    const float2 bw[4] = {
        float2((1.0f - f.x) * (1.0f - f.y), 0.0f),
        float2(f.x          * (1.0f - f.y), 0.0f),
        float2((1.0f - f.x) * f.y,          0.0f),
        float2(f.x          * f.y,          0.0f),
    };
    const int2 taps[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

    float sum = 0.0f, wsum = 0.0f;
    float nearestAO = 1.0f, nearestWeight = 0.0f;
    [unroll] for (int i = 0; i < 4; ++i) {
        int2  hp = clamp(h0 + taps[i], int2(0, 0), int2(AOW - 1, AOH - 1));
        float ao = AOIn.Load(int3(hp, 0));
        int2 fp = hp * int(scale);
        float sd = DepthTex.Load(int3(fp, 0));
        if (SmileIsSky(sd)) continue;
        float3 sampleP = GTAO_ViewPos(float2(fp), sd);
        float3 sampleN = GTAO_ViewNormal(NormalTex.Load(int3(fp, 0)).xyz);
        float bilateral = GTAO_BilateralWeight(centerP, centerN, sampleP, sampleN);
        float w = bw[i].x * bilateral;
        sum  += ao * w;
        wsum += w;
        if (bilateral > nearestWeight) { nearestWeight = bilateral; nearestAO = ao; }
    }
    // If the reduced grid missed this surface, there is no valid occlusion estimate.
    AOOut[tid.xy] = (wsum > 1e-4f) ? (sum / wsum)
                     : ((nearestWeight > 1e-4f) ? nearestAO : 1.0f);
}
