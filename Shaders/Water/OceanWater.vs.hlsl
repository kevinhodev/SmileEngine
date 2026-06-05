#include "WaterCommon.hlsli"

struct VSInput {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VSOutput {
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float  crest    : TEXCOORD3;
};

VSOutput main(VSInput input) {
    VSOutput o;

    float2 localXZ = input.pos.xz * WaterParams1.x;
    float2 baseXZ = float2(GridOrigin.x, GridOrigin.z) + localXZ;

    float4 disp = SampleOceanDisplacement(baseXZ);
    float distToCamera = length(baseXZ - CameraPosition.xz);
    float geomFade = 1.0f - smoothstep(WaterParams1.x * 0.35f, WaterParams1.x * 0.50f, distToCamera);
    float horizontalScale = WaterParams0.y * WaterParams0.w * geomFade;

    float2 displacedXZ = baseXZ + disp.xz * horizontalScale;
    float heightY = WaterParams0.x + disp.y * WaterParams0.y * geomFade;

    float3 worldPos = float3(displacedXZ.x, heightY, displacedXZ.y);

    o.pos = mul(float4(worldPos, 1.0f), ViewProj);
    o.worldPos = worldPos;
    o.normal = ComputeOceanNormal(baseXZ);
    o.uv = input.uv;
    o.crest = disp.w;
    return o;
}
