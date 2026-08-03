#include "../GBuffer.hlsli"

Texture2D<float4> Resolved : register(t0);
Texture2D<float4> GBufferB : register(t1);
RWTexture2D<float> OutSpecHit : register(u0);

static const float kMaxSpecHitDist = 60000.0f;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint width, height;
    OutSpecHit.GetDimensions(width, height);
    if (dtid.x >= width || dtid.y >= height) return;
    const float4 gb = GBufferB.Load(int3(dtid.xy, 0));
    if (GBuffer_DecodeShadingModel(gb.a) != SMILE_SHADINGMODEL_WATER) return;
    OutSpecHit[dtid.xy] = clamp(Resolved.Load(int3(dtid.xy, 0)).a, 0.0f, kMaxSpecHitDist);
}
