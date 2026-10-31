// Current-frame culling. The depth and late chains have independent CB/descriptor slices.
// Rejection replaces a slot with a policy sentinel consumed identically by classify and scatter.
#include "../Scene/GpuSceneTables.hlsli"

cbuffer CullCB : register(b0) {
    row_major float4x4 ViewProjection;
    uint4 Params; // outputs, bounds capacity, frustum enabled, occlusion enabled
    float4 Screen; // render width, height, mip count, reverse-Z bias
};
StructuredBuffer<GpuInstanceBounds> Bounds : register(t0);
Texture2D<float> HZB : register(t1);
RWStructuredBuffer<uint> Slots : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 Tid : SV_DispatchThreadID) {
    if (Tid.x >= Params.x) return;
    uint Slot = Slots[Tid.x];
    if (Slot >= Params.y) return; // invalid slots remain canaries for classify
    float3 C = Bounds[Slot].Center.xyz;
    float3 E = Bounds[Slot].Extent.xyz;
    if (!all(isfinite(C)) || !all(isfinite(E)) || any(E < 0.0) || all(E == 0.0)) return;

    float3 Lo = 1e30, Hi = -1e30;
    bool OutsideLeft = true, OutsideRight = true, OutsideTop = true, OutsideBottom = true;
    bool OutsideNear = true, OutsideFar = true, CrossesNear = false;
    [unroll] for (uint K = 0; K < 8; ++K) {
        float3 P = C + E * (float3(K & 1, (K >> 1) & 1, (K >> 2) & 1) * 2.0 - 1.0);
        float4 Q = mul(float4(P, 1.0), ViewProjection);
        if (!all(isfinite(Q))) return;
        // A pixel of lateral slack covers raster precision at the frustum boundary.
        float2 Slack = 2.0 * abs(Q.w) / max(Screen.xy, 1.0);
        OutsideLeft   = OutsideLeft   && Q.x < -Q.w - Slack.x;
        OutsideRight  = OutsideRight  && Q.x >  Q.w + Slack.x;
        OutsideBottom = OutsideBottom && Q.y < -Q.w - Slack.y;
        OutsideTop    = OutsideTop    && Q.y >  Q.w + Slack.y;
        OutsideNear   = OutsideNear   && Q.z > Q.w;
        OutsideFar    = OutsideFar    && Q.z < 0.0;
        CrossesNear = CrossesNear || Q.w <= 1e-4 || Q.z >= Q.w;
        if (Q.w > 1e-4) {
            Lo = min(Lo, Q.xyz / Q.w);
            Hi = max(Hi, Q.xyz / Q.w);
        }
    }
    if (Params.z != 0 && (OutsideLeft || OutsideRight || OutsideTop || OutsideBottom || OutsideNear || OutsideFar)) {
        Slots[Tid.x] = 0xFFFFFFFEu;
        return;
    }
    if (Params.w == 0 || CrossesNear || Screen.z < 1.0) return;
    if (Hi.x < -1.0 || Lo.x > 1.0 || Hi.y < -1.0 || Lo.y > 1.0 || Hi.z <= 0.0) return;

    float2 MinUV = saturate(float2(Lo.x, -Hi.y) * 0.5 + 0.5);
    float2 MaxUV = saturate(float2(Hi.x, -Lo.y) * 0.5 + 0.5);
    // Outward rounding plus a pixel, using the SAME jittered projection that wrote depth.
    int2 First = max(int2(floor(MinUV * Screen.xy)) - 1, 0);
    int2 Last = min(int2(ceil(MaxUV * Screen.xy)) + 1, int2(Screen.xy) - 1);
    if (any(Last < First)) return;
    First >>= 1; Last >>= 1; // HZB mip 0 is half resolution, power-of-two padded
    uint Mip = 0;
    while (any((Last >> Mip) - (First >> Mip) > 3) && Mip + 1 < uint(Screen.z)) ++Mip;
    First >>= Mip; Last >>= Mip;
    float Farthest = 1.0;
    for (int Y = First.y; Y <= Last.y; ++Y)
        for (int X = First.x; X <= Last.x; ++X)
            Farthest = min(Farthest, HZB.Load(int3(X, Y, Mip)));
    if (Hi.z + Screen.w < Farthest) Slots[Tid.x] = 0xFFFFFFFDu;
}
