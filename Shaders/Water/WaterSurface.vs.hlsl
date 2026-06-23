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

        float farFade = WaterDistanceFade(camDist, OceanFade.x, OceanFade.y);
        float farNormalFade = WaterNormalFade(camDist);

        float s = atten * farFade * 0.06 * OceanParams1.x * OceanFFT.y;

        float2 tcFFT = WaterFFTSampleUV(sampleWorldXZ);
        float4 disp = WaterSampleFFTUv(tcFFT);

        worldPos.x += disp.x * s * OceanFFT.z;
        worldPos.z += disp.y * s * OceanFFT.z;
        worldPos.y += disp.z * s;

        float hC = disp.z;
        float hX = WaterSampleFFTUv(tcFFT + float2(1.0 / 256.0, 0.0)).z;
        float hZ = WaterSampleFFTUv(tcFFT + float2(0.0, 1.0 / 256.0)).z;
        normal = normalize(float3(hC - hX, OceanFFT.w, hC - hZ));
        normal = normalize(lerp(float3(0.0, 1.0, 0.0), normal, farNormalFade));
    }

    float2 vTex = worldPos.xz * 0.005;
    o.baseTC.xy = vTex * BumpParams.x;
    o.baseTC.zw = vTex * (2.0 * BumpParams.x * BumpParams.y);
    o.debugData = float4(tileSize, subsetPattern, internalLod, geomorph);
    o.tileUV = localUV;

    o.worldPos = worldPos;
    o.vView = camPos - worldPos;
    o.pos = mul(float4(worldPos, 1.0), ViewProj);

    float normalScreenFade = WaterNormalFade(max(o.pos.w, 0.0));
    o.normal = normalize(lerp(float3(0.0, 1.0, 0.0), normal, normalScreenFade));

    o.screenProj.xy = o.pos.xy * float2(0.5, -0.5) + 0.5 * o.pos.w;
    o.screenProj.w = o.pos.w;
    o.screenProj.z = saturate(1.0 - 0.15 * sqrt(saturate(o.pos.w)));

    return o;
}
