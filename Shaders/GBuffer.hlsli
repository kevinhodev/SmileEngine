#ifndef SMILE_GBUFFER_HLSLI
#define SMILE_GBUFFER_HLSLI

#define SMILE_SHADINGMODEL_DEFAULTLIT 0u
#define SMILE_SHADINGMODEL_FOLIAGE    1u
#define SMILE_SHADINGMODEL_COUNT      2u

float2 GBuffer_OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 o = (n.z >= 0.0f) ? n.xy : ((1.0f - abs(n.yx)) * sign(n.xy));
    return o * 0.5f + 0.5f;
}

float3 GBuffer_OctDecode(float2 f) {
    f = f * 2.0f - 1.0f;
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    return normalize(n);
}

float GBuffer_EncodeShadingModel(uint id) {
    return ((float)id + 0.5f) / (float)SMILE_SHADINGMODEL_COUNT;
}
uint GBuffer_DecodeShadingModel(float a) {
    return min((uint)(a * (float)SMILE_SHADINGMODEL_COUNT), SMILE_SHADINGMODEL_COUNT - 1u);
}

struct GBufferOutput {
    float4 A        : SV_Target0; 
    float4 B        : SV_Target1; 
    float4 C        : SV_Target2; 
    float2 Velocity : SV_Target3; 
};

GBufferOutput EncodeGBuffer(float3 baseColor, float ao, float3 worldNormal,
                            float roughness, float metallic, float3 emissive, uint shadingModel) {
    GBufferOutput o;
    o.A = float4(baseColor, ao);
    o.B = float4(GBuffer_OctEncode(worldNormal), roughness, metallic);
    o.C = float4(emissive, GBuffer_EncodeShadingModel(shadingModel));
    o.Velocity = float2(0.0f, 0.0f); 
    return o;
}

struct GBufferData {
    float3 BaseColor;
    float  AO;
    float3 WorldNormal;
    float  Roughness;
    float  Metallic;
    float3 Emissive;
    uint   ShadingModel;
};

GBufferData DecodeGBuffer(float4 a, float4 b, float4 c) {
    GBufferData g;
    g.BaseColor    = a.rgb;
    g.AO           = a.a;
    g.WorldNormal  = GBuffer_OctDecode(b.rg);
    g.Roughness    = b.b;
    g.Metallic     = b.a;
    g.Emissive     = c.rgb;
    g.ShadingModel = GBuffer_DecodeShadingModel(c.a);
    return g;
}

#endif 
