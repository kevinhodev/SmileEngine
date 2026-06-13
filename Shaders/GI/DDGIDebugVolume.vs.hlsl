// DDGI debug — VS do wireframe da AABB do volume de probes (cascades/volumes). 12 arestas =
// 24 vertices (line list), geradas do SV_VertexID. Sem VB.
cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing;  // xyz = origem do grid, w = espacamento
    float4 GridCount;       // xyz = counts, w = numProbes
    float4 AtlasParams;
    float4 DistAtlasParams;
    float4 DebugParams;
    float4 CameraPos;
};

struct VSOut { float4 pos : SV_POSITION; };

VSOut main(uint vid : SV_VertexID) {
    float3 mn = GridMinSpacing.xyz;
    float3 mx = mn + (GridCount.xyz - 1.0f) * GridMinSpacing.w;

    // 8 cantos (bit x/y/z). 12 arestas como pares de indices de canto.
    const int edges[24] = {
        0,1, 1,3, 3,2, 2,0,   // face inferior (y=mn)
        4,5, 5,7, 7,6, 6,4,   // face superior (y=mx)
        0,4, 1,5, 2,6, 3,7    // pilares verticais
    };
    int corner = edges[vid];
    float3 p;
    p.x = ((corner & 1) != 0) ? mx.x : mn.x;
    p.y = ((corner & 4) != 0) ? mx.y : mn.y;
    p.z = ((corner & 2) != 0) ? mx.z : mn.z;

    VSOut o;
    o.pos = mul(float4(p, 1.0f), ViewProj);
    return o;
}
