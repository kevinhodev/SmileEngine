// Configuracao central de profundidade (Reverse-Z) — lado HLSL.
// Espelha Engine/Include/Smile/Graphics/DepthConfig.h: manter SMILE_REVERSE_Z em
// sincronia com kReverseZ (flip os dois + rebuild p/ comparar A/B).
//
// Reverse-Z: near->1, far->0. So a camera principal (perspectiva). Detalhes no header C++.
#ifndef SMILE_DEPTH_CONFIG
#define SMILE_DEPTH_CONFIG

#define SMILE_REVERSE_Z 1

#if SMILE_REVERSE_Z
    #define SMILE_NDC_FAR 0.0f   // far plane em NDC (fundo/ceu)
#else
    #define SMILE_NDC_FAR 1.0f
#endif

// true quando o pixel e fundo/ceu (depth no far plane do depth buffer limpo).
bool SmileIsSky(float d) {
#if SMILE_REVERSE_Z
    return d <= 0.0f;
#else
    return d >= 1.0f;
#endif
}

#endif // SMILE_DEPTH_CONFIG
