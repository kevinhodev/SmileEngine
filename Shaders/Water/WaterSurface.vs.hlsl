#include "WaterCommon.hlsli"

struct VSInput {
    float3 GridPos       : POSITION;
    uint   InstanceData0 : TEXCOORD0; 
    uint   InstanceData1 : TEXCOORD1; 
    uint   InstanceData2 : TEXCOORD2; 
};

float2 WaterClipToPixel(float4 Clip) {
    const float InvW = rcp(max(abs(Clip.w), 1.0e-4f));
    return Clip.xy * InvW * float2(0.5f * ScreenParams.x, -0.5f * ScreenParams.y);
}

float WaterSpectralAAWeight(uint Cascade, float FootprintWorld) {
    // Todas as cascatas atuais comecam em dois ciclos/tile. Logo tileWorld/2
    // e a maior onda exclusiva da banda e fornece um limite fisico continuo,
    // sem thresholds em distancia ou dependentes da resolucao da FFT.
    const float TileWorld = rcp(max(CascadeParams[Cascade], 1.0e-6f));
    const float LongestBandWave = TileWorld * 0.5f;
    const float FadeStart = LongestBandWave * 0.25f;
    const float FadeEnd = LongestBandWave * 0.5f;
    return 1.0f - smoothstep(FadeStart, FadeEnd, FootprintWorld);
}

VSOutput main(VSInput IN) {
    VSOutput o;

    const float waterLevel = OceanParams1.w;
    const float3 camPos = CameraPos.xyz;

    float2 localUV = saturate(IN.GridPos.xy);
    uint nodeX = IN.InstanceData0 & 0xFFFu;
    uint nodeZ = (IN.InstanceData0 >> 12u) & 0xFFFu;
    uint nodeScaleLOD = (IN.InstanceData0 >> 24u) & 0x1Fu;
    uint subsetRange = IN.InstanceData1;
    uint internalLodU = min(subsetRange / 81u, 2u);
    float internalLod = (float)internalLodU;
    float subsetPattern = (float)(subsetRange - internalLodU * 81u);
    const float tileGeomorph = (float)(IN.InstanceData2 & 0xFFu) / 255.0f;
    float tileSize = QuadTreeParams.z * exp2((float)nodeScaleLOD);
    float2 tileOrigin = QuadTreeParams.xy + float2(nodeX, nodeZ) * tileSize;

    const float currentLodStep = min(exp2(internalLod) / 32.0f, 1.0f);
    const float currentGridStepWorld = tileSize * currentLodStep;
    const float2 fineWorldXZ = tileOrigin + localUV * tileSize;
    const float3 finePlanarPos = float3(fineWorldXZ.x, waterLevel, fineWorldXZ.y);
    const float4 fineClip = mul(float4(finePlanarPos, 1.0f), ViewProjNoJitter);
    const float2 finePixel = WaterClipToPixel(fineClip);
    const float cellPixelsX = length(WaterClipToPixel(
        fineClip + ViewProjNoJitter[0] * currentGridStepWorld) - finePixel);
    const float cellPixelsZ = length(WaterClipToPixel(
        fineClip + ViewProjNoJitter[2] * currentGridStepWorld) - finePixel);

    // Quando uma celula se torna subpixel, converge para a grade seguinte em
    // vez de cintilar. Somente o interior sofre morph: todos os vertices de
    // borda continuam bit-identicos entre tiles e com os padroes de costura.
    const float footprintGeomorph = 1.0f - smoothstep(
        0.75f, 2.0f, min(cellPixelsX, cellPixelsZ));
    // GridPos.z is Chebyshev distance from the tile centre: 1 at the border.
    // The old one-cell guard jumped from an unmorphed edge to a fully morphed
    // interior and showed up as a rectangular crease. Spread the transition
    // across four cells while keeping the shared border bit-identical.
    const float edgeDistanceCells = (1.0f - IN.GridPos.z) * 16.0f;
    const float morphInteriorWeight = smoothstep(0.0f, 4.0f, edgeDistanceCells);
    const float geomorph = max(tileGeomorph, footprintGeomorph) * morphInteriorWeight;

    // A densidade interna muda a cada dois niveis. Derivar o alvo do nivel
    // espacial seguinte corrige as transicoes 1->2 e 3->4 (razao 4, nao 2).
    const uint nextInternalLodU = min((nodeScaleLOD + 1u) >> 1u, 2u);
    const float nextLodStep = min(exp2((float)nextInternalLodU + 1.0f) / 32.0f, 1.0f);
    float2 coarseUV = saturate(floor(localUV / nextLodStep + 0.5) * nextLodStep);
    float2 sampleUV = lerp(localUV, coarseUV, geomorph);
    float2 worldXZ = tileOrigin + sampleUV * tileSize;
    float2 sampleWorldXZ = worldXZ;

    // Footprint sem jitter em metros/pixel. O probe e global e, portanto, da
    // exatamente o mesmo resultado nos dois lados de uma borda compartilhada.
    // No interior incorporamos metade do passo geometrico (Nyquist) para filtrar
    // somente o que a malha nao representa. A transicao ocupa oito celulas; antes
    // ela acontecia em uma e desenhava um anel de filtro em cada tile.
    const float probeWorld = max(QuadTreeParams.z / 32.0f, 0.25f);
    const float3 planarPos = float3(worldXZ.x, waterLevel, worldXZ.y);
    const float4 planarClip = mul(float4(planarPos, 1.0f), ViewProjNoJitter);
    const float2 planarPixel = WaterClipToPixel(planarClip);
    const float probePixelsX = length(WaterClipToPixel(
        planarClip + ViewProjNoJitter[0] * probeWorld) - planarPixel);
    const float probePixelsZ = length(WaterClipToPixel(
        planarClip + ViewProjNoJitter[2] * probeWorld) - planarPixel);
    const float screenFootprintWorld = max(
        probeWorld / max(probePixelsX, 0.25f),
        probeWorld / max(probePixelsZ, 0.25f));
    const float filterInteriorWeight = smoothstep(0.0f, 8.0f, edgeDistanceCells);
    const float spectralFootprintWorld = lerp(
        screenFootprintWorld,
        max(screenFootprintWorld, currentGridStepWorld * 0.5f),
        filterInteriorWeight);

    float3 worldPos = float3(worldXZ.x, waterLevel, worldXZ.y);
    float3 previousWorldPos = worldPos;
    float3 normal = float3(0.0, 1.0, 0.0);

    float camDist = length(worldPos - camPos);

    if (OceanFFT.x > 0.5) {
        // The physical bake already supplies relative band energy. Geometry and
        // derivatives use the same fade/AA weight; no per-cascade amplitude boost.
        uint  numC = (uint)CascadeParams.w;
        float3 dispSum  = float3(0.0, 0.0, 0.0);
        float3 previousDispSum = float3(0.0, 0.0, 0.0);
        float3 derivativeX = float3(0.0, 0.0, 0.0);
        float3 derivativeZ = float3(0.0, 0.0, 0.0);
        float  fadeMul  = 1.0;
        [unroll] for (uint c = 0; c < 3; ++c) {
            if (c < numC) {
                float farFade = WaterDistanceFade(camDist, OceanFade.x * fadeMul,
                                                  OceanFade.y * fadeMul);
                const float spectralAA = WaterSpectralAAWeight(c, spectralFootprintWorld);
                farFade *= spectralAA;
                if (farFade > 0.0) {
                    float2 tc = WaterCascadeUV(c, sampleWorldXZ);
                    const float tileWorld = rcp(max(CascadeParams[c], 1.0e-6f));
                    const float texelWorld = tileWorld / 256.0f;
                    const float displacementLod = clamp(log2(
                        max(spectralFootprintWorld, texelWorld) / texelWorld), 0.0f, 8.0f);
                    float4 d  = WaterSampleFFTCascadeUvLod(c, tc, displacementLod);
                    float4 previousD = WaterSamplePreviousFFTCascadeUvLod(
                        c, tc, displacementLod);
                    dispSum += float3(d.x, d.z, d.y) * farFade;
                    previousDispSum += float3(previousD.x, previousD.z, previousD.y) * farFade;

                    const float2 texelX = float2(1.0 / 256.0, 0.0);
                    const float2 texelZ = float2(0.0, 1.0 / 256.0);
                    const float4 dL = WaterSampleFFTCascadeUvLod(c, tc - texelX, displacementLod);
                    const float4 dR = WaterSampleFFTCascadeUvLod(c, tc + texelX, displacementLod);
                    const float4 dD = WaterSampleFFTCascadeUvLod(c, tc - texelZ, displacementLod);
                    const float4 dU = WaterSampleFFTCascadeUvLod(c, tc + texelZ, displacementLod);
                    const float invTwoTexelWorld = 128.0f * CascadeParams[c];
                    derivativeX += (dR.xyz - dL.xyz) * (invTwoTexelWorld * farFade);
                    derivativeZ += (dU.xyz - dD.xyz) * (invTwoTexelWorld * farFade);
                }
            }
            fadeMul *= 4.0;
        }
        worldPos += dispSum;
        previousWorldPos += previousDispSum;

        const float3 Tx = float3(1.0f + derivativeX.x, derivativeX.z, derivativeX.y);
        const float3 Tz = float3(derivativeZ.x, derivativeZ.z, 1.0f + derivativeZ.y);
        normal = cross(Tz, Tx);
        if (normal.y < 0.0f) normal = -normal;
        normal.xz /= max(OceanFFT.w, 0.05f);
        normal = normalize(normal);
    }

    // Material coordinates remain parametric; shading must not re-evaluate the
    // spectrum at the horizontally displaced position x'.
    o.baseTC.xy = WaterCascadeUV(1, sampleWorldXZ);
    o.baseTC.zw = WaterCascadeUV(0, sampleWorldXZ);
    o.debugData = float4(tileSize, subsetPattern, internalLod, geomorph);
    o.tileUV = localUV;

    o.worldPos = worldPos;
    o.parametricXZ = sampleWorldXZ;
    o.vView = camPos - worldPos;
    o.pos = mul(float4(worldPos, 1.0), ViewProj);
    o.curClip  = mul(float4(worldPos, 1.0), ViewProjNoJitter);
    o.prevClip = mul(float4(previousWorldPos, 1.0), PrevViewProj);

    o.normal = normal;

    o.screenProj.xy = o.pos.xy * float2(0.5, -0.5) + 0.5 * o.pos.w;
    o.screenProj.w = o.pos.w;
    o.screenProj.z = saturate(1.0 - 0.15 * sqrt(saturate(o.pos.w)));

    return o;
}
