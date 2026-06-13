// Specular GI — passe de trace das reflexoes (DXR 1.1 inline ray tracing, SM 6.6).
// Esqueleto estilo Lumen Reflections, Fase 1: por pixel da tela, reconstroi o ponto de mundo
// do depth, le a normal+roughness do G-buffer de reflexao (MRT do forward), atira UM raio na
// direcao espelhada (mirror) contra a TLAS e sombreia o hit via HitShading.hlsli (o MESMO
// shading do DDGI: sol+sombra + multibounce do atlas; miss = ceu). Escreve a RADIANCIA INCIDENTE
// crua (o peso BRDF/Fresnel + blend com o DDGI sao aplicados no composite — assim o denoiser das
// fases seguintes ve radiancia, nao o resultado ponderado).
//
// Full-res, sem denoise, mirror-only (Fase 1). Fase 2: VNDF + half-res. Fase 3: denoise.
// So roda sem MSAA (G-buffer single-sample, igual GTAO/TAA).

#include "../GI/DDGICommon.hlsli" // InstanceGeo, DDGIVertex (antes dos globais StructuredBuffer<>)
#include "GGXSample.hlsli"        // VNDF GGX (glossy, Fase 2) + tangent basis

// Prefixo (InvViewProj, CameraPos, ScreenParams, ReflectParams) IDENTICO ao CompositeCB:
// o trace e o composite compartilham o MESMO constant buffer (o composite le so o prefixo).
cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj; // FULL inverse view-proj (reconstroi world do depth)
    float4 CameraPos;       // xyz = camera world, w = -
    float4 ScreenParams;    // x = W, y = H, z = 1/W, w = 1/H
    float4 ReflectParams;   // x = maxRoughnessToTrace, y = roughnessFadeLength, z = realHitShading, w = albedoLOD
    float4 GridMinSpacing;  // xyz = grid origin (mundo), w = spacing  (DDGI)
    float4 GridCount;       // xyz = probe counts, w = -              (DDGI)
    float4 AtlasParams;     // x = tile, y = atlasW, z = atlasH, w = - (DDGI irradiancia)
    float4 SunDirIntensity; // xyz = dir TO sun, w = intensity
    float4 SunColor;        // rgb = sun color, w = -
    float4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = normalBias
    float4 HalfScreenParams;// halfW, halfH, 1/halfW, 1/halfH (trace e half-res, Fase 2b)
};

// Globais lidos por HitShading.hlsli (mesmos nomes que a fatoracao espera).
RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
StructuredBuffer<DDGIVertex>    Vertices   : register(t4);
Buffer<uint>                    Indices    : register(t5);
// Entradas screen-space (G-buffer de reflexao + depth).
Texture2D<float>                Depth      : register(t6); // NDC z [0,1] (Reverse-Z: 0 = far/ceu)
Texture2D<float4>               GBuffer    : register(t7); // RG = octN*0.5+0.5, B = roughness, A = metallic

RWTexture2D<float4>             RWReflection : register(u0); // rgb = radiancia incidente, a = hitDist
RWTexture2D<float4>             RWRayData    : register(u1); // xyz = direcao do raio, w = pdf (resolve)

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli" // ShadeSurfaceHit, ShadeSky, FHitShadeParams, DDGI_OctDecode

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 halfPx = DTid.xy;
    if (halfPx.x >= (uint)HalfScreenParams.x || halfPx.y >= (uint)HalfScreenParams.y) return;

    // Half-res (Fase 2b): cada texel amostra 1 dos 4 pixels full-res do bloco 2x2, jitterado por
    // frame (4-rooks). A reconstrucao/upsample p/ full-res e feita no resolve.
    int2 fullPx = int2(halfPx) * 2 + RefTileJitter(halfPx, (uint)TraceParams.x);
    fullPx = min(fullPx, int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));

    float3 outRadiance = float3(0.0f, 0.0f, 0.0f);
    float  outHitDist  = TraceParams.y;
    float4 outRay      = float4(0.0f, 0.0f, 0.0f, 0.0f); // xyz = dir, w = pdf (0 = invalido p/ o resolve)

    float4 gb        = GBuffer.Load(int3(fullPx, 0));
    float  roughness = gb.b;
    // Combine (Lumen): 1 = so RT, 0 = so DDGI. Pula pixels rugosos (o difuso da GI cobre) — e o
    // ceu (G-buffer limpo p/ roughness=1 -> alpha=0). Mesma formula no composite e no forward.
    float combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));

    float deviceZ = Depth.Load(int3(fullPx, 0)).r;

    if (deviceZ > 0.0f && combineAlpha > 0.0f) {
        float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

        // Reconstroi a posicao de mundo do depth (mesma convencao do fog deferido).
        float2 uv  = (fullPx + 0.5f) * ScreenParams.zw;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
        float3 worldPos = wH.xyz / wH.w;

        float3 V = normalize(CameraPos.xyz - worldPos);

        // Direcao do raio: VNDF GGX (glossy, Fase 2) — amostra o lobo pela roughness. Quase-espelho
        // (roughness < 0.05) usa mirror puro (o resolve pula a reconstrucao). pdf guardado p/ o
        // ratio estimator do resolve; mirror usa pdf=1 (delta, peso forcado no centro).
        float  alpha  = max(roughness * roughness, 1e-3f);
        float3 R;
        float  rayPdf = 1.0f;
        if (roughness < 0.05f) {
            R = reflect(-V, N);
        } else {
            float2 E = GGX_Rand2((uint2)fullPx, (uint)TraceParams.x);
            E.y *= 1.0f - 0.1f; // GGXSamplingBias (anti-ruido: enviesa pro centro do lobo)
            float3x3 basis = GGX_TangentBasis(N);
            float3   Vt    = mul(basis, V);          // world -> tangent
            float4   ggx   = GGX_SampleVNDF(E, alpha, Vt);
            float3   Hw    = mul(ggx.xyz, basis);    // micronormal tangent -> world
            R      = reflect(-V, Hw);
            rayPdf = ggx.w;
            if (dot(R, N) <= 0.0f) { R = reflect(-V, N); rayPdf = 1.0f; } // abaixo do horizonte -> mirror
        }
        outRay = float4(R, rayPdf);

        float3 sunDir = normalize(SunDirIntensity.xyz);

        // Raio de reflexao com normal bias (anti self-intersection / acne — porte do Lumen).
        RayDesc ray;
        ray.Origin    = worldPos + N * max(TraceParams.w, 1e-3f);
        ray.Direction = R;
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;

        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
        q.TraceRayInline(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
        while (q.Proceed()) {}

        FHitShadeParams P;
        P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
        P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
        P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
        P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
        P.SunColor       = SunColor.rgb;        P.NormalBias   = TraceParams.w;
        P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
        P.AlbedoLOD      = ReflectParams.w;
        P.RealHitShading = ReflectParams.z > 0.5f;

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            float sd;
            outHitDist  = q.CommittedRayT();
            outRadiance = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                          q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                          ray.Origin, ray.Direction, outHitDist, P, sd);
        } else {
            outRadiance = ShadeSky(R, sunDir, P.SkyIntensity);
        }
    }

    RWReflection[halfPx] = float4(outRadiance, outHitDist);
    RWRayData[halfPx]    = outRay;
}
