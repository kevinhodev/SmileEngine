#ifndef SMILE_OCEAN_FFT_COMMON_HLSLI
#define SMILE_OCEAN_FFT_COMMON_HLSLI

static const uint  DISP_MAP_SIZE      = 256u;
static const uint  LOG2_DISP_MAP_SIZE = 8u;
static const float OCEAN_PI    = 3.14159265358979323846f;
static const float OCEAN_TWOPI = 6.28318530717958647692f;

cbuffer OceanFFTCB : register(b0) {
    float Time;
    float ChoppyScale;
    float HeightScale;
    float InvTwoTexelWorld;
    float DeltaTime;    // dt real do frame (s) — acumulacao temporal da espuma
    float FoamRecovery; // taxa de recuperacao do J acumulado rumo ao J do frame (1/s)
    float FoamReset;    // 1 = sem historico valido (primeiro frame): usa J direto
    float OceanPadding;
};

float2 OceanComplexMul(float2 z, float2 w) {
    return float2(z.x * w.x - z.y * w.y, z.y * w.x + z.x * w.y);
}

#endif 
