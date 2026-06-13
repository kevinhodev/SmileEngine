// DDGI debug — VS do RÓTULO de nº de raios por probe. Billboard (quad camera-aligned) acima de
// cada probe ativa proxima a camera, mostrando ProbeRayCount[probe] (8/16/32/64) em digitos
// procedurais (ver .ps). Substitui o antigo desenho dos raios (pouco util).
#include "DDGICommon.hlsli"

cbuffer DDGIDebugCB : register(b0) {
    row_major float4x4 ViewProj;
    float4 GridMinSpacing;  // xyz = origem, w = espacamento
    float4 GridCount;       // xyz = counts, w = numProbes
    float4 AtlasParams;
    float4 DistAtlasParams;
    float4 DebugParams;     // y = probeRadius (fracao do spacing)
    float4 CameraPos;       // xyz = camera
    float4 RayParams;       // y = raio (world) ao redor da camera p/ rotular
};

Buffer<float4> ProbeData     : register(t0); // xyz = offset, w = state (<0 inativa)
Buffer<uint>   ProbeRayCount : register(t5); // nº de raios deste probe

struct VSOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;                     // [0,1] no quad
    nointerpolation float2 info : TEXCOORD1;      // x = rayCount, y = numDigits
};

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;
    int3   count    = (int3)GridCount.xyz;
    int3   pc       = DDGI_ProbeCoord((int)iid, count);
    float4 pd       = ProbeData[iid];
    float3 probePos = DDGI_ProbeWorldPos(pc, GridMinSpacing.xyz, GridMinSpacing.w) + pd.xyz;

    // So probes ativas e proximas a camera (senao polui).
    if (pd.w < 0.0f || distance(probePos, CameraPos.xyz) > RayParams.y) {
        o.pos = float4(2, 2, 2, 1); o.uv = 0; o.info = 0; return o; // descartado
    }

    uint  rayCount  = ProbeRayCount[iid];
    float numDigits = (rayCount >= 100u) ? 3.0f : (rayCount >= 10u) ? 2.0f : 1.0f;

    // Quad (2 triangulos) em UV [0,1].
    const float2 quad[6] = { float2(0,0), float2(1,0), float2(1,1),
                             float2(0,0), float2(1,1), float2(0,1) };
    float2 uv = quad[vid];

    // Billboard camera-aligned: projeta o centro e desloca em screen-space (×w cancela a divisao).
    float4 center = mul(float4(probePos, 1.0f), ViewProj);
    float  h = 0.045f;                 // altura do rotulo em NDC
    float  w = h * 0.62f * numDigits;  // largura ~0.62·h por digito
    center.xy += float2((uv.x - 0.5f) * w, (uv.y - 0.5f) * h) * center.w;
    center.y  += (0.05f) * center.w;   // sobe acima da esfera

    o.pos  = center;
    o.uv   = uv;
    o.info = float2((float)rayCount, numDigits);
    return o;
}
