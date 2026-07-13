#ifndef SMILE_SUNSHAFTS_COMMON
#define SMILE_SUNSHAFTS_COMMON

// Sun shafts screen-space (radial blur) — hibrido Cry/UE: mascara em meia-res
// (luminancia acima do limiar x mascara de distancia) + 3 passes de blur radial
// de 12 taps com distancia exponencial (LightShaftShader.usf) + composicao
// aditiva no HDR antes do TAA/FSR2 (o acumulador limpa o ruido do downsample).

cbuffer SunShaftsCB : register(b0) {
    float4 SunPosParams;  // xy = UV do sol na tela, z = fade global, w = aspect (H/W)
    float4 ShaftParams;   // x = limiar de luminancia, y = dist do 1o passe (fracao ate o sol),
                          // z = escala do passe (4.8^i), w = intensidade da composicao
    float4 ShaftTint;     // rgb = tint (key light), w = 1/occlusionDepthRange (mundo)
    float4 ScreenParams;  // xy = dims do RT alvo deste passe, zw = 1/dims
    row_major float4x4 InvViewProj; // inversa full (reconstroi dist de mundo do depth)
    float4 CameraWorldPos;          // xyz = camera em mundo, w unused
};

SamplerState LinearClamp : register(s0);
SamplerState PointClamp  : register(s1);

#define SUNSHAFTS_NUM_SAMPLES 12

#endif
