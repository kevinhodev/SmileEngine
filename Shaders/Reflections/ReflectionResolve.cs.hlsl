#include "GGXSample.hlsli"        
#include "../GI/DDGICommon.hlsli" 

cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;    
    float4 ReflectParams;    
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;      
    float4 HalfScreenParams; 
};

Texture2D<float4> Radiance : register(t0); 
Texture2D<float4> RayData  : register(t1); 
Texture2D<float4> RayMotion : register(t2);
Texture2D<float>  Depth    : register(t3);
Texture2D<float4> GBuffer  : register(t4);

RWTexture2D<float4> RWResolved : register(u0); 
RWTexture2D<float4> RWResolvedMotion : register(u1);

#define RESOLVE_SAMPLES 8

float3 ReconstructWorld(int2 px, float deviceZ) {
    float2 uv  = (px + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wH.xyz / wH.w;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int2 px = (int2)DTid.xy; 
    if (px.x >= (int)ScreenParams.x || px.y >= (int)ScreenParams.y) return;

    float4 gb        = GBuffer.Load(int3(px, 0));
    float  roughness = gb.b;
    float  combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));
    float  deviceZ   = Depth.Load(int3(px, 0)).r;

    float3 resolved   = float3(0.0f, 0.0f, 0.0f);
    float  outHitDist = 0.0f; 
    float4 outMotion  = 0.0f;

    if (deviceZ > 0.0f && combineAlpha > 0.0f) {
        float3 N        = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
        float3 worldPos = ReconstructWorld(px, deviceZ);
        float3 V        = normalize(CameraPos.xyz - worldPos);
        float  alpha    = roughness * roughness;
        float  a2       = alpha * alpha;
        uint   frame    = (uint)TraceParams.x;
        int2   halfDim  = int2((int)HalfScreenParams.x, (int)HalfScreenParams.y);
        int2   centerHalf = px >> 1;
        float  kernel   = max(8.0f * saturate(roughness * 8.0f), 1.0f); 
        float  planeThresh = max(length(worldPos - CameraPos.xyz) * 0.05f, 1e-3f);

        uint2  rnd = uint2(GGX_PCG(px.x + GGX_PCG(px.y)), GGX_PCG(frame + px.y));

        int2 sub      = px & 1;
        int2 stepDir  = sub * 2 - 1;                 
        int2 upOff[4] = { int2(0, 0), int2(stepDir.x, 0), int2(0, stepDir.y), stepDir };

        float3 centerRad     = float3(0.0f, 0.0f, 0.0f);
        float  centerW       = 0.0f;
        float  centerHitAcc  = 0.0f; // distancia acumulada com o mesmo peso do centerRad
        float  centerHitDist = 0.0f;
        float  bestW         = 0.0f;
        [unroll] for (uint u = 0; u < 4; ++u) {
            int2 h = centerHalf + upOff[u];
            if (any(h < 0) || h.x >= halfDim.x || h.y >= halfDim.y) continue;
            if (RayData.Load(int3(h, 0)).w <= 0.0f) continue;
            int2 hFull = min(h * 2 + RefTileJitter((uint2)h, frame),
                             int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));
            float hd = Depth.Load(int3(hFull, 0)).r;
            if (hd <= 0.0f) continue;
            float3 hWorld = ReconstructWorld(hFull, hd);
            if (abs(dot(hWorld - worldPos, N)) > planeThresh) continue;   
            float2 dd = abs(float2(hFull - px));
            float  wb = max(0.0f, 2.0f - dd.x) * max(0.0f, 2.0f - dd.y);  
            if (wb <= 0.0f) continue;
            float4 hrad = Radiance.Load(int3(h, 0));
            centerRad += hrad.rgb * wb;
            // A DISTANCIA acumula com o mesmo peso da radiancia. Antes ela era copiada da UNICA
            // amostra de maior peso, enquanto o rgb virava media — o hitDist saia daqui com todo
            // o ruido de 1 spp de meia-res. Isso importa porque ele nao e so metadado: e o
            // kBufferTypeSpecularHitDistance do DLSS-RR, de onde ele deriva os motion vectors
            // ESPECULARES. Ruido por pixel ali faz o RR reprojetar cada vizinho para um lugar
            // diferente, e o resultado e o campo de firefly que so aparece no RR (medido
            // 2026-08-07: o heatmap do guide bate forma, posicao e tamanho com o artefato).
            centerHitAcc += hrad.a * wb;
            centerW   += wb;
            if (wb > bestW) {
                bestW = wb;
                // Motion continua vindo da MELHOR amostra, nao da media: media de motion vector
                // entre superficies diferentes nao descreve movimento nenhum.
                outMotion = RayMotion.Load(int3(h, 0));
            }
        }
        if (centerW > 1e-6f) {
            centerRad /= centerW;
            centerHitDist = centerHitAcc / centerW;
        } else {
            float4 c = Radiance.Load(int3(centerHalf, 0));
            centerRad = c.rgb; centerHitDist = c.a;
            outMotion = RayMotion.Load(int3(centerHalf, 0));
        }
        outHitDist = centerHitDist;

        float3 dir0 = reflect(-V, N);
        float  w0   = max(GGX_D(a2, saturate(dot(N, normalize(V + dir0)))), 1e-3f);
        float3 sum  = centerRad * w0;
        float  wsum = w0;
        // O laco largo tambem passou a somar distancia. E onde mais importa: o kernel cresce com
        // a roughness, entao a superficie que mais espalha raio (e mais ruido gera no hitDist) e
        // justamente a que ganha mais vizinhos para mediar.
        float  hitSum = centerHitDist * w0;

        [loop] for (uint i = 1; i <= RESOLVE_SAMPLES; ++i) {
            float2 E   = GGX_Hammersley(i - 1, RESOLVE_SAMPLES, rnd);
            int2   off = int2(GGX_ConcentricDisk(E) * kernel + 0.5f);
            int2   nbHalf = centerHalf + off;
            if (any(nbHalf < 0) || nbHalf.x >= halfDim.x || nbHalf.y >= halfDim.y) continue;

            float4 nray = RayData.Load(int3(nbHalf, 0));
            if (nray.w <= 0.0f) continue;

            int2 nbFull = min(nbHalf * 2 + RefTileJitter((uint2)nbHalf, frame),
                              int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));
            float nbDepth = Depth.Load(int3(nbFull, 0)).r;
            if (nbDepth <= 0.0f) continue;

            float3 nbWorld = ReconstructWorld(nbFull, nbDepth);
            if (abs(dot(nbWorld - worldPos, N)) > planeThresh) continue;

            float4 nrad     = Radiance.Load(int3(nbHalf, 0));
            float  hitDist  = min(nrad.a, centerHitDist);
            float3 hitPos   = nbWorld + nray.xyz * hitDist;
            float3 dirToHit = normalize(hitPos - worldPos);
            float3 H        = normalize(V + dirToHit);
            float  w        = GGX_D(a2, saturate(dot(N, H))) / max(nray.w, 1e-6f);
            // Clamp de peso na FONTE do firefly: com pdf pequeno, GGX_D/pdf explode e uma unica amostra RT
            // domina o sum => sparkle/mancha colorida. Limita o vizinho a 8x o peso do centro (mirror),
            // preservando a media sem deixar um outlier sequestrar o pixel.
            w = min(w, w0 * 8.0f);

            sum  += nrad.rgb * w;
            wsum += w;
            hitSum += nrad.a * w;
        }

        resolved = sum / max(wsum, 1e-6f);
        // Fade por combineAlpha: onde a reflexao esta desaparecendo por roughness, o guide vai a
        // ZERO junto com ela. Zero e o valor SEGURO do kBufferTypeSpecularHitDistance — o RR cai
        // no motion vector da superficie —, e sem isto a borda do corte de roughness entregaria
        // distancia cheia num pixel que quase nao tem especular, que e o pior dos dois mundos.
        outHitDist = (hitSum / max(wsum, 1e-6f)) * combineAlpha;
    }

    RWResolved[DTid.xy] = float4(resolved, outHitDist);
    RWResolvedMotion[DTid.xy] = outMotion;
}
