// Remodula a saida da instancia DIRETA do RELAX e devolve a soma ao mesmo alvo que o deferred
// ja consome. Assim NRD, RR e o caminho cru compartilham um unico contrato em t20.

#include "../GBuffer.hlsli"

cbuffer ReSTIRDICB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params;
    float4 Sampling;
    float4 Reuse;
    float4 TemporalPolicy;
    float4 RayEpsA;
    float4 RayEpsB;
    row_major float4x4 View;
};

Texture2D<float4> NrdDiffuse  : register(t0);
Texture2D<float4> NrdSpecular : register(t1);
Texture2D<float4> GBufferA    : register(t2);
Texture2D<float4> GBufferB    : register(t3);
Texture2D<float4> GBufferC    : register(t4);
Texture2D<float>  Depth       : register(t5);
RWTexture2D<float4> OutDirect : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;
    if (Depth.Load(int3(px, 0)) <= 0.0f) {
        OutDirect[px] = 0.0f;
        return;
    }

    const GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                        GBufferB.Load(int3(px, 0)),
                                        GBufferC.Load(int3(px, 0)));
    const float3 diffuseAlbedo = g.BaseColor * (1.0f - g.Metallic);
    const float3 f0 = lerp(0.04f.xxx, g.BaseColor, g.Metallic);
    const float4 diff = NrdDiffuse.Load(int3(px, 0));
    const float4 spec = NrdSpecular.Load(int3(px, 0));
    OutDirect[px] = float4(diff.rgb * diffuseAlbedo + spec.rgb * f0,
                           max(diff.a, spec.a));
}
