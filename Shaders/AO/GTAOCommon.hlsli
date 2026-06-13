#ifndef GTAO_COMMON_HLSLI
#define GTAO_COMMON_HLSLI

// Constantes compartilhadas pelos passes de GTAO (b0). Bate campo-a-campo com
// GTAOConstants (AmbientOcclusion.h).
cbuffer GTAOCB : register(b0) {
    float4 ProjA;        // x = M00, y = M11, z = M22, w = M32 (termos da projecao LH)
    float4 ScreenParams; // x = W, y = H, z = 1/W, w = 1/H
    float4 Params;       // x = radius (mundo), y = intensity, z = power, w = falloffEnd (mundo)
    float4 Params2;      // x = numDirections, y = numSteps, z = frameIndex, w = blurDir (0=H,1=V)
    float4 Params3;      // x = fadeStart (Z de view), y = fadeEnd, zw = reservado
    row_major float4x4 ViewMatrix; // world->view: transforma a normal do G-buffer p/ view (GTAO main)
};

static const float GTAO_PI     = 3.14159265f;
static const float GTAO_HALFPI = 1.57079633f;

// Profundidade nao-linear (NDC z em [0,1]) -> Z de view (positivo p/ frente).
// Zv = M32 / (d - M22), com M22 = Far/(Far-Near), M32 = -Near*Far/(Far-Near).
float GTAO_LinearizeDepth(float d) {
    return ProjA.w / (d - ProjA.z);
}

// Posicao em espaco de view a partir do pixel (px = coord inteira) + profundidade crua.
float3 GTAO_ViewPos(float2 px, float rawDepth) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float  zv  = GTAO_LinearizeDepth(rawDepth);
    float  ndcX = uv.x * 2.0f - 1.0f;
    float  ndcY = 1.0f - uv.y * 2.0f;
    float  xv  = ndcX * zv / ProjA.x;
    float  yv  = ndcY * zv / ProjA.y;
    return float3(xv, yv, zv);
}

// Interleaved gradient noise (Jimenez). ESTATICO (sem shift por frame): sem acumulacao
// temporal, rotacionar o ruido por frame causava cintilacao; o padrao fixo e escondido pelo
// blur bilateral. O arg frame e mantido p/ um futuro caminho temporal.
float GTAO_IGN(float2 px, float frame) {
    return frac(52.9829189f * frac(0.06711056f * px.x + 0.00583715f * px.y));
}

#endif
