#include "VolumetricFogCommon.hlsli"
#include "../Common/DepthConfig.hlsli"

// Conservative depth (UE GenerateConservativeDepthBuffer): p/ cada tile XY do grid
// froxel, o depth NDC do pixel MAIS DISTANTE (reverse-Z: MIN de device-z) dentro do
// tile em resolucao de render. O scattering pula froxels cuja face frontal esta mais
// longe que isso — tile 100% coberto por parede nao computa luz atras dela. Ceu tem
// device-z 0 (reverse-Z), entao tile com qualquer pixel de ceu nunca pula (correto:
// o fog na frente do ceu existe).

Texture2D<float>   SceneDepth   : register(t0);
RWTexture2D<float> ConsDepthOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)kVolFogW || id.y >= (uint)kVolFogH) return;

    const uint W = (uint)ConsDepthParams.y, H = (uint)ConsDepthParams.z;
    // Faixa de pixels do tile — mesma particao do sample do fog apply (uv * grid).
    uint x0 = (id.x * W) / (uint)kVolFogW, x1 = ((id.x + 1u) * W) / (uint)kVolFogW;
    uint y0 = (id.y * H) / (uint)kVolFogH, y1 = ((id.y + 1u) * H) / (uint)kVolFogH;
    x1 = max(x1, x0 + 1u); y1 = max(y1, y0 + 1u);

    float minZ = 1.0f;
    for (uint y = y0; y < y1; ++y)
        for (uint x = x0; x < x1; ++x)
            minZ = min(minZ, SceneDepth.Load(int3(x, y, 0)));

    ConsDepthOut[id.xy] = minZ;
}
