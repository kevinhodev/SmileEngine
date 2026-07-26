#include "AtmosphereCommon.hlsli"

// Ambient fisico do ceu (estilo "distant sky light" da UE): integra o SkyView LUT cos-weighted
// nos dois hemisferios e devolve E/pi — [0] = ceu (superficies olhando pra cima), [1] = chao
// virtual (bounce, superficies olhando pra baixo). O resultado herda TUDO do LUT: multiscatter,
// crepusculo, transmitancia e o ceu de luar (lua = 2a luz). Lido de volta na CPU (2 frames de
// latencia) e escrito nos slots SkyAmbientColor/GroundAmbientColor do FrameConstants.
Texture2D<float4>          SkyViewLUT : register(t0);
RWStructuredBuffer<float4> OutAmbient : register(u0);

[numthreads(1, 1, 1)]
void main() {
    const int   NEl    = 4;
    const int   NAz    = 8;
    const float dTheta = (0.5f * PI) / (float)NEl;
    const float dPhi   = (2.0f * PI) / (float)NAz;

    // O LUT so depende do azimute RELATIVO ao sol: phi ja e o azimute relativo, de graca.
    [unroll]
    for (int hemi = 0; hemi < 2; ++hemi) {
        float3 sum = float3(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < NEl; ++i) {
            float theta = ((float)i + 0.5f) * dTheta + (hemi == 1 ? 0.5f * PI : 0.0f);
            float ct = cos(theta);
            float st = sin(theta);
            for (int j = 0; j < NAz; ++j) {
                float  phi = ((float)j + 0.5f) * dPhi;
                float2 uv  = SkyViewParamsToUv(ct, cos(phi), kViewHeight);
                float3 L   = SkyViewLUT.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;
                sum += L * abs(ct) * st * dTheta * dPhi;
            }
        }
        OutAmbient[hemi] = float4(sum / PI, 1.0f);
    }
}
