// FogFullscreen.vs.hlsl — fullscreen triangle (no vertex buffer).
struct VSOut { float4 pos : SV_POSITION; };

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2); // (0,0) (2,0) (0,2)
    o.pos = float4(t * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
