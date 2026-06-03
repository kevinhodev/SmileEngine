// Fullscreen triangle for the atmosphere sky pass. The PS reconstructs the world
// view direction from clip-space position + InvViewProjNoTrans (AtmosphereCB).

struct VSOutput {
    float4 pos    : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

VSOutput main(uint vid : SV_VertexID) {
    float2 corners[3] = {
        float2(-1.0f, -3.0f),
        float2(-1.0f,  1.0f),
        float2( 3.0f,  1.0f),
    };
    float2 p = corners[vid];

    VSOutput o;
    o.pos    = float4(p, 1.0f, 1.0f); // far plane (depth = 1)
    o.clipXY = p;
    return o;
}
