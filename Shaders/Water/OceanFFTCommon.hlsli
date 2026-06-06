#ifndef SMILE_OCEAN_FFT_COMMON_HLSLI
#define SMILE_OCEAN_FFT_COMMON_HLSLI

// Constantes compartilhadas dos passos de compute do oceano (GPU FFT).
// O grid e 256x256 (vs 64x64 do antigo caminho CPU). LOG2 = 8.
static const uint  DISP_MAP_SIZE      = 256u;
static const uint  LOG2_DISP_MAP_SIZE = 8u;
static const float OCEAN_PI    = 3.14159265358979323846f;
static const float OCEAN_TWOPI = 6.28318530717958647692f;

// CB compartilhado por todos os passos (b0 do FVolumetricPipeline). Casa com
// FOceanFFT::OceanCB.
cbuffer OceanFFTCB : register(b0) {
    float Time;          // tempo de sim (= 0.125 * elapsed, quirk da Cry)
    float ChoppyScale;   // lambda do deslocamento horizontal (ChoppyWaveScale)
    float HeightScale;   // escala da altura (MaxWaveSize)
    float NormalUp;      // componente "up" da normal derivada da altura
    float JacobianScale; // sensibilidade do foam (J = det(I + scale*grad D))
    float _Pad0;
    float _Pad1;
    float _Pad2;
};

float2 OceanComplexMul(float2 z, float2 w) {
    return float2(z.x * w.x - z.y * w.y, z.y * w.x + z.x * w.y);
}

#endif // SMILE_OCEAN_FFT_COMMON_HLSLI
