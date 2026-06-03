// Fullscreen triangle for the cloud composite. Renders at the far plane (depth=1,
// LESS_EQUAL) so clouds appear only on background pixels (occluded by geometry).

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
    o.pos = float4(corners[vid], 1.0f, 1.0f);
    return o;
}
