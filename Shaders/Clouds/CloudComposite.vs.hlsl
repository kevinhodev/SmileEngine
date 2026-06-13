#include "../Common/DepthConfig.hlsli"

struct VSOutput {
    float4 pos : SV_POSITION;
};

VSOutput main(uint vid : SV_VertexID) {
    float2 corners[3] = {
        float2(-1.0f, -3.0f),
        float2(-1.0f,  1.0f),
        float2( 3.0f,  1.0f),
    };
    VSOutput o;
    o.pos = float4(corners[vid], SMILE_NDC_FAR, 1.0f);
    return o;
}
