#include "WaterCommon.hlsli"

struct VSInput {
    float3 GridPos       : POSITION;
    uint   InstanceData0 : TEXCOORD0; 
    uint   InstanceData1 : TEXCOORD1; 
    uint   InstanceData2 : TEXCOORD2; 
};

VSOutput main(VSInput IN) {
    VSOutput o;

    const float waterLevel = OceanParams1.w;
    const float3 camPos = CameraPos.xyz;

    float2 localUV = saturate(IN.GridPos.xy);
    uint nodeX = IN.InstanceData0 & 0x7FFu;
    uint nodeZ = (IN.InstanceData0 >> 11u) & 0x7FFu;
    uint nodeScaleLOD = (IN.InstanceData0 >> 22u) & 0x1Fu;
    uint subsetRange = IN.InstanceData1;
    uint internalLodU = min(subsetRange / 81u, 2u);
    float internalLod = (float)internalLodU;
    float subsetPattern = (float)(subsetRange - internalLodU * 81u);
    float geomorph = (float)(IN.InstanceData2 & 0xFFu) / 255.0f;
    float tileSize = QuadTreeParams.z * exp2((float)nodeScaleLOD);
    float2 tileOrigin = QuadTreeParams.xy + float2(nodeX, nodeZ) * tileSize;

    geomorph *= saturate((1.0 - IN.GridPos.z) * 16.0);
    float nextLodStep = min(exp2(internalLod + 1.0) / 32.0, 1.0);
    float2 coarseUV = saturate(floor(localUV / nextLodStep + 0.5) * nextLodStep);
    float2 sampleUV = lerp(localUV, coarseUV, geomorph);
    float2 worldXZ = tileOrigin + localUV * tileSize;
    float2 sampleWorldXZ = tileOrigin + sampleUV * tileSize;

    float3 worldPos = float3(worldXZ.x, waterLevel, worldXZ.y);
    float3 normal = float3(0.0, 1.0, 0.0);

    float camDist = length(worldPos - camPos);

    if (OceanFFT.x > 0.5) {
        float atten = saturate(camDist * 0.5);
        atten *= atten;

        float s = atten * 0.06 * OceanParams1.x * OceanFFT.y;

        // Multi-cascata: soma das 3 escalas. Bandas de espectro disjuntas (config no
        // C++) — a soma não duplica energia. Boost por cascata (OceanFade.zw, ~razão
        // dos tiles) mantém steepness auto-similar: onda maior = mais alta, mesma
        // inclinação. Fades: cascata maior persiste mais longe (×4/×16).
        const float boosts[3] = { 1.0, OceanFade.z, OceanFade.w };
        uint  numC = (uint)CascadeParams.w;
        float3 dispSum  = float3(0.0, 0.0, 0.0);
        float2 slopeAcc = float2(0.0, 0.0);
        float  fadeMul  = 1.0;
        [unroll] for (uint c = 0; c < 3; ++c) {
            if (c < numC) {
                float farFade = WaterDistanceFade(camDist, OceanFade.x * fadeMul,
                                                  OceanFade.y * fadeMul);
                float nFade = WaterDistanceFade(camDist,
                                                max(OceanFade.x * 2.5, 1000.0) * fadeMul,
                                                max(OceanFade.y * 3.0, 4500.0) * fadeMul);
                if (farFade > 0.0 || nFade > 0.0) {
                    float2 tc = WaterCascadeUV(c, sampleWorldXZ);
                    float4 d  = WaterSampleFFTCascadeUv(c, tc);
                    float  w  = boosts[c] * farFade;
                    dispSum.x += d.x * w * OceanFFT.z;
                    dispSum.z += d.y * w * OceanFFT.z;
                    dispSum.y += d.z * w;
                    float hX = WaterSampleFFTCascadeUv(c, tc + float2(1.0 / 256.0, 0.0)).z;
                    float hZ = WaterSampleFFTCascadeUv(c, tc + float2(0.0, 1.0 / 256.0)).z;
                    // boost × (1/texelWorld) cancelam → peso igual por cascata
                    slopeAcc += float2(d.z - hX, d.z - hZ) * nFade;
                }
            }
            fadeMul *= 4.0;
        }
        worldPos += dispSum * s;
        normal = normalize(float3(slopeAcc.x, OceanFFT.w, slopeAcc.y));
    }

    // UVs físicas das cascatas (o PS usa as normais reais de cada escala; a cascata 2
    // é recomputada no PS a partir do worldPos).
    o.baseTC.xy = WaterCascadeUV(1, worldPos.xz);
    o.baseTC.zw = WaterCascadeUV(0, worldPos.xz);
    o.debugData = float4(tileSize, subsetPattern, internalLod, geomorph);
    o.tileUV = localUV;

    o.worldPos = worldPos;
    o.vView = camPos - worldPos;
    o.pos = mul(float4(worldPos, 1.0), ViewProj);
    // Velocity so de camera: mesma posicao deslocada projetada pelas VPs sem jitter
    // (fase da onda entre frames fica de fora — residuo pequeno pro TAA/FSR2).
    o.curClip  = mul(float4(worldPos, 1.0), ViewProjNoJitter);
    o.prevClip = mul(float4(worldPos, 1.0), PrevViewProj);

    // Fade da normal já aplicado por cascata (nFade) — o swell mantém normal no horizonte.
    o.normal = normal;

    o.screenProj.xy = o.pos.xy * float2(0.5, -0.5) + 0.5 * o.pos.w;
    o.screenProj.w = o.pos.w;
    o.screenProj.z = saturate(1.0 - 0.15 * sqrt(saturate(o.pos.w)));

    return o;
}
