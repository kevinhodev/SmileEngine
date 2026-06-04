// Fullscreen triangle vertex shader for post-processing passes.
// Generates a full screen quad from a single triangle with UV coordinates.

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(uint vid : SV_VertexID) {
    float2 corners[3] = {
        float2(-1.0f, -3.0f),
        float2(-1.0f,  1.0f),
        float2( 3.0f,  1.0f),
    };
    VSOutput o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv  = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}
