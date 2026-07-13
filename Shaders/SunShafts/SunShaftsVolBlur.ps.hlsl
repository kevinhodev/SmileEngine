// Blur gaussiano 3x3 no inscatter volumetrico meia-res, antes do fog consumir.
// O sinal e low-frequency por natureza (midia participante) — o blur mata o residuo
// do jitter IGN do raymarch sem apagar os feixes (que sao penumbrais de qualquer
// forma). Mesma pratica das refs: froxel fog integra/filtra, raymarch screen-space
// filtra bilateral; cru em meia-res vira mancha (visto no A/B da F2).

cbuffer SunShaftsVolCB : register(b0) {
    float4 SunDirPhase;
    float4 SunColorInt;
    float4 FogDensityP;
    float4 MarchParams;
    float4 ScreenParams;   // xy = dims do RT meia-res, zw = 1/dims
    row_major float4x4 InvViewProj;
    float4 CameraWorldPos;
};

Texture2D<float4> Source     : register(t0);
SamplerState      PointClamp : register(s1);

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    float2 uv = svpos.xy * ScreenParams.zw;

    // pesos 1-2-1 separaveis (soma 16)
    float3 acc = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float w = (x == 0 ? 2.0f : 1.0f) * (y == 0 ? 2.0f : 1.0f);
            float2 suv = uv + float2(x, y) * ScreenParams.zw;
            acc += Source.SampleLevel(PointClamp, suv, 0.0f).rgb * w;
        }
    }
    return float4(acc * (1.0f / 16.0f), 1.0f);
}
