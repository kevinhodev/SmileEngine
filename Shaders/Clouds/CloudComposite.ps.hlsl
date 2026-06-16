Texture2D<float4> CloudRT : register(t0);

struct PSInput {
    float4 pos : SV_POSITION;
};

float4 main(PSInput input) : SV_TARGET {
    int3 px = int3((int)input.pos.x, (int)input.pos.y, 0);
    float4 cloud = CloudRT.Load(px);

    float3 L = cloud.rgb;           
    float  T = cloud.a;             

    return float4(L, T);
}
