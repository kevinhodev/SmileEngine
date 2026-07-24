// Guides do DLSS Ray Reconstruction: deriva do G-buffer os buffers de material que o RR usa p/
// separar difuso/especular ao denoisar a cor RUIDOSA. Ver ProgrammingGuideDLSS_RR.md §4.
//   u0 DiffuseAlbedo   = BaseColor*(1-Metallic)              (kBufferTypeAlbedo)
//   u1 SpecularAlbedo  = EnvBRDFApprox2(F0, rough^2, NoV)    (kBufferTypeSpecularAlbedo, §4.2.1)
//   u2 NormalRoughness = normal mundo (xyz) + rough linear (a) (kBufferTypeNormalRoughness, ePacked)
// SRV t0-t3 = G-buffer A,B,C + Depth (a MESMA tabela contigua do deferred, SRVTableStart()).
// CBV b0 = FrameConstants (usamos ate InvViewProj p/ reconstruir worldPos -> NoV).
#include "../GBuffer.hlsli"

cbuffer FrameCB : register(b0) {
    float4 CameraPosition;
    float4 IBLParams;
    float4 Time;
    float4 SunDirection;
    float4 SunColor;
    float4 SkyAmbientColor;
    float4 GroundAmbientColor;
    float4 DDGIGridMin;
    float4 DDGIGridCount;
    float4 DDGIParams;
    float4 DDGIDistParams;
    float4 ReflectionParams;
    float4 MoonDirection;
    float4 MoonColor;
    row_major float4x4 InvViewProj; // FULL (jittered) — reconstrucao de worldPos p/ o NoV
    // resto do FrameConstants nao usado aqui
};

Texture2D<float4> GBufferA : register(t0); // BaseColor (sRGB->linear no Load) + AO
Texture2D<float4> GBufferB : register(t1); // OctNormal + Rough + ShadingModelID
Texture2D<float4> GBufferC : register(t2); // Metallic + subsurface
Texture2D<float>  Depth    : register(t3); // reverse-Z (device)

RWTexture2D<float4> OutDiffuseAlbedo   : register(u0);
RWTexture2D<float4> OutSpecularAlbedo  : register(u1);
RWTexture2D<float4> OutNormalRoughness : register(u2);

// EnvBRDFApprox2 do ProgrammingGuideDLSS_RR.md §4.2.1 (Ray Tracing Gems, cap. 32). alpha = rough^2.
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV) {
    NoV = abs(NoV);
    float4 X; X.x = 1.0f; X.y = NoV; X.z = NoV * NoV; X.w = NoV * X.z;
    float4 Y; Y.x = 1.0f; Y.y = alpha; Y.z = alpha * alpha; Y.w = alpha * Y.z;
    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    float3x3 M2 = float3x3(1.0f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
    float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
    float3x3 M4 = float3x3(1.0f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);
    float bias  = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(SpecularColor.g * 50.0f); // hack: reflectancia especular ~0
    return mad(SpecularColor, max(0.0f, scale), max(0.0f, bias));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint W, H;
    OutDiffuseAlbedo.GetDimensions(W, H);
    if (dtid.x >= W || dtid.y >= H) return;
    int2 px = int2(dtid.xy);

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) {
        // Ceu/background: sem material. Albedo 0 avisa o RR que a cor ja e final (nao demodula/denoisa).
        OutDiffuseAlbedo[px]   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutSpecularAlbedo[px]  = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutNormalRoughness[px] = float4(0.0f, 0.0f, 1.0f, 1.0f);
        return;
    }

    GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                  GBufferB.Load(int3(px, 0)),
                                  GBufferC.Load(int3(px, 0)));

    float2 uv  = (float2(px) + 0.5f) / float2(W, H);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;
    float3 V   = normalize(CameraPosition.xyz - worldPos);
    float  NoV = saturate(dot(g.WorldNormal, V));

    float3 F0    = lerp(float3(0.04f, 0.04f, 0.04f), g.BaseColor, g.Metallic);
    float  rough = max(g.Roughness, 0.04f);

    OutDiffuseAlbedo[px]   = float4(g.BaseColor * (1.0f - g.Metallic), 1.0f);
    OutSpecularAlbedo[px]  = float4(EnvBRDFApprox2(F0, rough * rough, NoV), 1.0f);
    OutNormalRoughness[px] = float4(g.WorldNormal, rough); // ePacked: normal.xyz + rough linear no .a
}
