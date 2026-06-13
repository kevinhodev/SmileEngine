// Specular GI — passe de RESOLVE (reconstrucao espacial + UPSAMPLE half->full, Fase 2b).
// Porte do LumenReflectionResolve: o trace glossy roda em HALF-res (1 raio/lobo, VNDF). Aqui, em
// FULL-res, cada pixel junta os raios dos vizinhos HALF-res e repesa cada um pelo ratio estimator
// `peso = D_GGX(H)/pdf` (H = meio-vetor V↔dir-do-hit, na geometria do pixel central) + peso de
// PLANO (rejeita vizinhos fora da superficie -> upsample sem borrar bordas). Faz o denoise espacial
// E o upsample no mesmo passe. O temporal (estabilidade entre frames) e a F3.

#include "GGXSample.hlsli"        // GGX_D, Hammersley, ConcentricDisk, PCG, RefTileJitter
#include "../GI/DDGICommon.hlsli" // DDGI_OctDecode

cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;     // full-res: W, H, 1/W, 1/H
    float4 ReflectParams;    // x = maxRoughnessToTrace, y = roughnessFadeLength, z = realHit, w = albedoLOD
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;      // x = frameIndex, ...
    float4 HalfScreenParams; // half-res: halfW, halfH, 1/halfW, 1/halfH
};

Texture2D<float4> Radiance : register(t0); // HALF-res: rgb = radiancia, a = hitDist
Texture2D<float4> RayData  : register(t1); // HALF-res: xyz = dir, w = pdf (0 = invalido)
Texture2D<float>  Depth    : register(t2); // FULL-res
Texture2D<float4> GBuffer  : register(t3); // FULL-res

RWTexture2D<float4> RWResolved : register(u0); // FULL-res

#define RESOLVE_SAMPLES 8

float3 ReconstructWorld(int2 px, float deviceZ) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wH.xyz / wH.w;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int2 px = (int2)DTid.xy; // FULL-res
    if (px.x >= (int)ScreenParams.x || px.y >= (int)ScreenParams.y) return;

    float4 gb        = GBuffer.Load(int3(px, 0));
    float  roughness = gb.b;
    float  combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));
    float  deviceZ   = Depth.Load(int3(px, 0)).r;

    float3 resolved   = float3(0.0f, 0.0f, 0.0f);
    float  outHitDist = 0.0f; // exportado p/ a reprojeção parallax do temporal (Fase 3)

    if (deviceZ > 0.0f && combineAlpha > 0.0f) {
        float3 N        = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
        float3 worldPos = ReconstructWorld(px, deviceZ);
        float3 V        = normalize(CameraPos.xyz - worldPos);
        float  alpha    = roughness * roughness;
        float  a2       = alpha * alpha;
        uint   frame    = (uint)TraceParams.x;
        int2   halfDim  = int2((int)HalfScreenParams.x, (int)HalfScreenParams.y);
        int2   centerHalf = px >> 1;
        float  kernel   = max(8.0f * saturate(roughness * 8.0f), 1.0f); // raio (px half-res)
        float  planeThresh = max(length(worldPos - CameraPos.xyz) * 0.05f, 1e-3f); // ~5% da dist de view

        float  centerHitDist = Radiance.Load(int3(centerHalf, 0)).a;
        outHitDist = centerHitDist;
        uint2  rnd = uint2(GGX_PCG(px.x + GGX_PCG(px.y)), GGX_PCG(frame + px.y));

        float3 sum  = float3(0.0f, 0.0f, 0.0f);
        float  wsum = 0.0f;

        // i = 0: texel half-res central; i = 1..N: vizinhos no disco (espaco half-res).
        for (uint i = 0; i <= RESOLVE_SAMPLES; ++i) {
            int2 nbHalf;
            if (i == 0) {
                nbHalf = centerHalf;
            } else {
                float2 E   = GGX_Hammersley(i - 1, RESOLVE_SAMPLES, rnd);
                int2   off = int2(GGX_ConcentricDisk(E) * kernel + 0.5f);
                nbHalf = centerHalf + off;
            }
            if (any(nbHalf < 0) || nbHalf.x >= halfDim.x || nbHalf.y >= halfDim.y) continue;

            float4 nray = RayData.Load(int3(nbHalf, 0));
            if (nray.w <= 0.0f) continue;

            // Pixel full-res que este texel half-res amostrou (mesmo jitter do trace) -> seu depth.
            int2  nbFull = nbHalf * 2 + RefTileJitter((uint2)nbHalf, frame);
            nbFull = min(nbFull, int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));
            float nbDepth = Depth.Load(int3(nbFull, 0)).r;
            if (nbDepth <= 0.0f) continue;

            float3 nbWorld = ReconstructWorld(nbFull, nbDepth);
            // Peso de plano: rejeita vizinhos fora da superficie do centro (bordas nitidas no upsample).
            if (abs(dot(nbWorld - worldPos, N)) > planeThresh) continue;

            float4 nrad     = Radiance.Load(int3(nbHalf, 0));
            float  hitDist  = min(nrad.a, centerHitDist);           // clamp ao centro: preserva contato
            float3 hitPos   = nbWorld + nray.xyz * hitDist;
            float3 dirToHit = normalize(hitPos - worldPos);
            float3 H        = normalize(V + dirToHit);
            float  w        = GGX_D(a2, saturate(dot(N, H))) / max(nray.w, 1e-6f);
            if (i == 0) w = max(w, 1e-3f);                          // centro sempre conta

            sum  += nrad.rgb * w;
            wsum += w;
        }

        resolved = (wsum > 1e-6f) ? (sum / wsum) : Radiance.Load(int3(centerHalf, 0)).rgb;
    }

    RWResolved[DTid.xy] = float4(resolved, outHitDist);
}
