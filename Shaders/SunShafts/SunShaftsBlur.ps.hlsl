#include "SunShaftsCommon.hlsli"

// Blur radial em direcao ao sol (LightShaftShader.usf): 12 taps por passe, distancia
// cresce exponencialmente a cada passe (4.8^i) — 3 passes cobrem a tela inteira com
// 36 taps efetivos. Trabalha em UV aspect-corrigido p/ raio isotropico em pixels.

Texture2D<float4> Source : register(t0);

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    float2 uv     = svpos.xy * ScreenParams.zw;
    float2 aspect = float2(1.0f, SunPosParams.w);

    float2 auv  = uv * aspect;
    float2 asun = SunPosParams.xy * aspect;

    // nunca le alem da posicao do sol (min com 1)
    float2 ablur = (asun - auv) * min(ShaftParams.y * ShaftParams.z, 1.0f);

    float3 acc = 0.0f;
    [unroll] for (int i = 0; i < SUNSHAFTS_NUM_SAMPLES; ++i) {
        float2 suv = (auv + ablur * (float(i) / float(SUNSHAFTS_NUM_SAMPLES))) / aspect;
        acc += Source.SampleLevel(LinearClamp, saturate(suv), 0.0f).rgb;
    }

    return float4(acc / float(SUNSHAFTS_NUM_SAMPLES), 1.0f);
}
