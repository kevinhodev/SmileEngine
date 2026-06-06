#include "OceanFFTCommon.hlsli"

// Combina os campos IFFT (altura + choppy) no mapa de deslocamento (porte de
// createdisplacement.comp do Asylum, adaptado ao layout da Smile).
// Layout Smile: (Dx, Dz, -h, 0). O Jacobiano (foam) entra no passo de gradients.
Texture2D<float2>   HeightF : register(t0); // h(k,t) apos IFFT: .x = altura
Texture2D<float2>   ChoppyF : register(t1); // D(k,t) apos IFFT: .x = Dx, .y = Dz
RWTexture2D<float4> DispOut : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DISP_MAP_SIZE || id.y >= DISP_MAP_SIZE) return;

    int2 loc = int2(id.xy);

    // Correcao de sinal por causa do shift do espectro centrado.
    float sgn = (((loc.x + loc.y) & 1) == 1) ? -1.0f : 1.0f;

    float  h = sgn * HeightF.Load(int3(loc, 0)).x;
    float2 D = sgn * ChoppyF.Load(int3(loc, 0)).xy;

    DispOut[loc] = float4(D.x * ChoppyScale, D.y * ChoppyScale, -h * HeightScale, 0.0f);
}
