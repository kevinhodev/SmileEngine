// CloudComposite.ps.hlsl
// Loads the raymarched cloud RT (1:1 with the screen) and blends it over the sky
// already in the render target. Output is (tonemapped luminance, transmittance);
// the pipeline blend is configured as: final = src.rgb + dst.rgb * src.a, i.e. an
// "over" composite where transmittance is the cloud's alpha pass-through.
// NOTE: the sky in the RT is already tonemapped, so we tonemap the cloud here too
// and blend in display space (approximate — a unified HDR target is future work).

Texture2D<float4> CloudRT : register(t0);

struct PSInput {
    float4 pos : SV_POSITION;
};

float4 main(PSInput input) : SV_TARGET {
    int3 px = int3((int)input.pos.x, (int)input.pos.y, 0);
    float4 cloud = CloudRT.Load(px);

    float3 L = cloud.rgb;            // scattered luminance (linear, premultiplied)
    float  T = cloud.a;             // transmittance

    return float4(L, T);
}
