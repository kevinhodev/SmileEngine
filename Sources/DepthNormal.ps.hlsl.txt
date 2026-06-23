struct PSInput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target0 {
    float3 n = normalize(input.worldNormal);
    return float4(n * 0.5f + 0.5f, 1.0f); 
}
