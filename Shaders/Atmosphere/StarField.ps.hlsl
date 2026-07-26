#include "AtmosphereCommon.hlsli"

Texture2D<float4> TransmittanceLUT : register(t1);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float3 col : TEXCOORD1;
    float3 dir : TEXCOORD2;
};

// Escala global do brilho: mag 0 (Vega) -> pico ~0.2 de luminancia HDR antes do slider;
// Sirius (~3.9x) chega perto de 0.8. Tunavel pelo slider de estrelas (MoonParams.y).
static const float kStarExposure = 0.2f;

float4 main(VSOut input) : SV_TARGET {
    float r2 = dot(input.uv, input.uv);
    // Gaussiana com corte suave na borda do quad (evita quadrado duro em quads pequenos).
    float footprint = exp(-r2 * 3.5f) * saturate(1.2f - r2 * 1.2f);

    float3 dir = normalize(input.dir);

    // Extincao atmosferica pela transmitancia na direcao de vista + fade no horizonte.
    float3 viewTrans    = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, dir.y);
    float  aboveHorizon = smoothstep(-0.02f, 0.06f, dir.y);

    // Estrela atras do disco da lua some (o disco e desenhado no sky pass).
    float moonMask = (dot(dir, normalize(MoonDir.xyz)) > MoonDir.w) ? 0.0f : 1.0f;

    float night = MoonParams.z;
    float3 c = input.col * (kStarExposure * MoonParams.y * footprint)
             * viewTrans * (night * aboveHorizon * moonMask);
    return float4(c, 1.0f);
}
