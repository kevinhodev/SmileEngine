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

// ID no alpha de 2 BITS do RGB10A2 (niveis 0, 1/3, 2/3, 1): o encode (id+0.5)/COUNT cai no
// nivel certo por arredondamento p/ COUNT potencia de 2 (2 ou 4 modelos). Se COUNT crescer
// alem de 4, o ID precisa de casa nova (stencil ou canal de 8 bits no GBufferC).
float GBuffer_EncodeShadingModel(uint id) {
    return ((float)id + 0.5f) / (float)SMILE_SHADINGMODEL_COUNT;
}
uint GBuffer_DecodeShadingModel(float a) {
    return min((uint)(a * (float)SMILE_SHADINGMODEL_COUNT), SMILE_SHADINGMODEL_COUNT - 1u);
}

// Dieta do G-buffer (192 -> 128 bpp com velocity, paridade com UE/Flax):
//   A (RGBA8)    .rgb = BaseColor  .a = AO do material
//   B (RGB10A2)  .rg  = OctNormal  .b = Roughness      .a = ShadingModelID (2 bits)
//   C (RGBA8)    .r   = Metallic   .gba = Subsurface (tint x intensidade, PREMULTIPLICADO;
//                       folhagem default = 0.6 branco — o mesmo fator que era hardcoded no
//                       deferred; DefaultLit escreve 0 e nada transmite)
//   Emissive     -> escrito DIRETO no SceneColor HDR (SV_Target4); o deferred lighting
//                   SOMA a luz por cima (blend aditivo, estilo UE) em vez de reescrever.
struct GBufferOutput {
    float4 A        : SV_Target0;
    float4 B        : SV_Target1;
    float4 C        : SV_Target2;
    float2 Velocity : SV_Target3;
    float4 Emissive : SV_Target4; // SceneColor HDR (RGBA16F)
};

GBufferOutput EncodeGBuffer(float3 baseColor, float ao, float3 worldNormal,
                            float roughness, float metallic, float3 emissive, uint shadingModel,
                            float3 subsurface) {
    GBufferOutput o;
    o.A = float4(baseColor, ao);
    o.B = float4(GBuffer_OctEncode(worldNormal), roughness,
                 GBuffer_EncodeShadingModel(shadingModel));
    o.C = float4(metallic, saturate(subsurface));
    o.Velocity = float2(0.0f, 0.0f);
    o.Emissive = float4(emissive, 0.0f);
    return o;
}

struct GBufferData {
    float3 BaseColor;
    float  AO;
    float3 WorldNormal;
    float  Roughness;
    float  Metallic;
    uint   ShadingModel;
    float3 Subsurface; // tint x intensidade (premul); multiplica o BaseColor na transmissao
};

GBufferData DecodeGBuffer(float4 a, float4 b, float4 c) {
    GBufferData g;
    g.BaseColor    = a.rgb;
    g.AO           = a.a;
    g.WorldNormal  = GBuffer_OctDecode(b.rg);
    g.Roughness    = b.b;
    g.ShadingModel = GBuffer_DecodeShadingModel(b.a);
    g.Metallic     = c.r;
    g.Subsurface   = c.gba;
    return g;
}

#endif
