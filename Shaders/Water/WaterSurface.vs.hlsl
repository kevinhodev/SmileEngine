#include "WaterCommon.hlsli"

// Local tile vertex: xy in [0,1], z = Chebyshev edge value.
struct VSInput {
    float3 GridPos  : POSITION;
    float4 TileData : TEXCOORD0; // x=originX y=originZ z=size w=subset range (internal LOD + pattern)
    float4 MorphData : TEXCOORD1; // x=geomorph y=coverage z=internal LOD debug w=unused
};

// World-space quadtree tile. This replaces the old projected grid/ray-plane
// intersection path, so the horizon is no longer a stretched screen-space ring.
VSOutput main(VSInput IN) {
    VSOutput o;

    const float waterLevel = OceanParams1.w;
    const float3 camPos = CameraPos.xyz;

    float2 localUV = saturate(IN.GridPos.xy);
    float subsetRange = IN.TileData.w;
    float internalLod = floor(subsetRange / 81.0);
    float subsetPattern = subsetRange - internalLod * 81.0;
    float geomorph = saturate(IN.MorphData.x);
    // Geomorph em borda compartilhada abre crack: vizinhos podem estar em LOD/morph
    // diferentes. Pinamos a borda e deixamos o morph atuar no interior do tile.
    geomorph *= saturate((1.0 - IN.GridPos.z) * 16.0);
    float nextLodStep = min(exp2(internalLod + 1.0) / 32.0, 1.0);
    float2 coarseUV = saturate(floor(localUV / nextLodStep + 0.5) * nextLodStep);
    float2 sampleUV = lerp(localUV, coarseUV, geomorph);
    float2 worldXZ = IN.TileData.xy + localUV * IN.TileData.z;
    float2 sampleWorldXZ = IN.TileData.xy + sampleUV * IN.TileData.z;

    float3 worldPos = float3(worldXZ.x, waterLevel, worldXZ.y);
    float3 normal = float3(0.0, 1.0, 0.0);

    float camDist = length(worldPos - camPos);
    float farBlend = WaterFarBlend(camDist);
    float nearFFT = 1.0 - farBlend;

    // Small far lift, kept from the Cry path, but driven by world distance instead of
    // projected-grid edge vertices. It helps the last ocean tiles blend into haze/sky.
    float horizonT = SmoothWaterStep(saturate((camDist - ProjGrid.x * 0.72) / max(ProjGrid.x * 0.28, 1.0)));
    worldPos.y += horizonT * ProjGrid.z;

    // --- FFT displacement near/mid, Asylum-style procedural swell in the distance. ---
    if (OceanFFT.x > 0.5) {
        // No tile-edge damping here: damping every quadtree border would reveal the grid.
        float edgeMask = 1.0;

        float atten = saturate(camDist * 0.5);
        atten *= atten;

        float farFade = saturate(1.0 - (camDist - OceanFade.x) / max(OceanFade.y, 1.0));
        farFade = farFade * farFade * (3.0 - 2.0 * farFade);

        float normalFadeStart = max(OceanFade.x * 2.5, 1000.0);
        float normalFadeRange = max(OceanFade.y * 3.0, 4500.0);
        float farNormalFade = saturate(1.0 - (camDist - normalFadeStart) / normalFadeRange);
        farNormalFade = farNormalFade * farNormalFade * (3.0 - 2.0 * farNormalFade);

        float s = atten * edgeMask * farFade * nearFFT * 0.06 * OceanParams1.x * OceanFFT.y;

        float2 tcFFT = sampleWorldXZ * 0.0125 * OceanParams0.w * 1.25;
        float4 disp = FFTDisplacement.SampleLevel(LinearWrap, tcFFT, 0.0);
        float farSwell = WaterFarSwellHeight(sampleWorldXZ) * atten * edgeMask * farBlend;

        worldPos.x += disp.x * s * OceanFFT.z;
        worldPos.z += disp.y * s * OceanFFT.z;
        worldPos.y += disp.z * s + farSwell;

        float hC = disp.z;
        float hX = FFTDisplacement.SampleLevel(LinearWrap, tcFFT + float2(1.0 / 64.0, 0.0), 0.0).z;
        float hZ = FFTDisplacement.SampleLevel(LinearWrap, tcFFT + float2(0.0, 1.0 / 64.0), 0.0).z;
        float3 fftNormal = normalize(float3(hC - hX, OceanFFT.w, hC - hZ));
        float3 farNormal = WaterFarSwellNormal(sampleWorldXZ);
        normal = normalize(lerp(fftNormal, farNormal, farBlend));
        normal = normalize(lerp(float3(0.0, 1.0, 0.0), normal, farNormalFade));
    }

    float2 flowDir = OceanParams1.yz;
    float2 trans = Misc.x * OceanParams0.y * 0.0025 * flowDir;
    float2 vTex = worldPos.xz * 0.005;
    o.baseTC.xy = vTex * BumpParams.x + trans;
    o.baseTC.zw = vTex * (2.0 * BumpParams.x * BumpParams.y) + trans * 2.0;
    o.debugData = float4(IN.TileData.z, subsetPattern, internalLod, geomorph);

    o.worldPos = worldPos;
    o.vView = camPos - worldPos;
    o.pos = mul(float4(worldPos, 1.0), ViewProj);

    float normalScreenFadeStart = max(OceanFade.x * 2.5, 1000.0);
    float normalScreenFadeRange = max(OceanFade.y * 3.0, 4500.0);
    float normalScreenFade = saturate(1.0 - (max(o.pos.w, 0.0) - normalScreenFadeStart) / normalScreenFadeRange);
    normalScreenFade = normalScreenFade * normalScreenFade * (3.0 - 2.0 * normalScreenFade);
    o.normal = normalize(lerp(float3(0.0, 1.0, 0.0), normal, normalScreenFade));

    o.screenProj.xy = o.pos.xy * float2(0.5, -0.5) + 0.5 * o.pos.w;
    o.screenProj.w = o.pos.w;
    o.screenProj.z = saturate(1.0 - 0.15 * sqrt(saturate(o.pos.w)));

    return o;
}
