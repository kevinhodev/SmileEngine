#include "../GI/DDGICommon.hlsli"
#include "GGXSample.hlsli"
cbuffer ReflectionCB : register(b0) {
    row_major float4x4 InvViewProj; 
    float4 CameraPos;       
    float4 ScreenParams;    
    float4 ReflectParams;   
    float4 GridMinSpacing;  
    float4 GridCount;      
    float4 AtlasParams;     
    float4 SunDirIntensity; 
    float4 SunColor;        
    float4 TraceParams;
    float4 HalfScreenParams;
    // Campos abaixo nao sao usados por este shader; declarados p/ os offsets do CB baterem ate o
    // perfil de epsilons, que vem no fim do ReflectionConstants.
    row_major float4x4 PrevViewProj;
    float4 TemporalParams;
    float4 DebugParams;
    row_major float4x4 View;
    float4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
    float4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
};

#include "../RayOffset.hlsli" // depois do cbuffer: le RayEpsA/RayEpsB

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
// t4/t5 aposentados (VB/IB bindless via InstanceGeo); a tabela CPU mantem o layout com filler.

Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8); // F5: luzes puntuais nos hits

RWTexture2D<float4>             RWReflection : register(u0);
RWTexture2D<float4>             RWRayData    : register(u1); 

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli" 

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 halfPx = DTid.xy;
    if (halfPx.x >= (uint)HalfScreenParams.x || halfPx.y >= (uint)HalfScreenParams.y) return;

    int2 fullPx = int2(halfPx) * 2 + RefTileJitter(halfPx, (uint)TraceParams.x);
    fullPx = min(fullPx, int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));

    float3 outRadiance = float3(0.0f, 0.0f, 0.0f);
    float  outHitDist  = TraceParams.y;
    float4 outRay      = float4(0.0f, 0.0f, 0.0f, 0.0f); 

    float4 gb        = GBuffer.Load(int3(fullPx, 0));
    float  roughness = gb.b;
    float combineAlpha = saturate((ReflectParams.x - roughness) / max(ReflectParams.y, 1e-4f));

    float deviceZ = Depth.Load(int3(fullPx, 0)).r;

    if (deviceZ > 0.0f && combineAlpha > 0.0f) {
        float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

        float2 uv  = (fullPx + 0.5f) * ScreenParams.zw;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
        float3 worldPos = wH.xyz / wH.w;

        float3 V = normalize(CameraPos.xyz - worldPos);

        float  alpha  = max(roughness * roughness, 1e-3f);
        float3 R;
        float  rayPdf = 1.0f;
        if (roughness < 0.05f) {
            R = reflect(-V, N);
        } else {
            float2 E = GGX_Rand2((uint2)fullPx, (uint)TraceParams.x);
            E.y *= 1.0f - 0.1f; 
            float3x3 basis = GGX_TangentBasis(N);
            float3   Vt    = mul(basis, V);          
            float4   ggx   = GGX_SampleVNDF(E, alpha, Vt);
            float3   Hw    = mul(ggx.xyz, basis);   
            R      = reflect(-V, Hw);
            rayPdf = ggx.w;
            if (dot(R, N) <= 0.0f) { R = reflect(-V, N); rayPdf = 1.0f; } 
        }
        outRay = float4(R, rayPdf);

        float3 sunDir = normalize(SunDirIntensity.xyz);

        // Offset robusto (so anti self-hit): o bias 0.2 na origem deslocava o reflexo de
        // contato e inflava o hitT entregue ao NRD/temporal.
        RayDesc ray;
        ray.Origin    = OffsetRayGBuffer(worldPos, N, R, length(CameraPos.xyz - worldPos));
        ray.Direction = R;
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;

        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
        // ALL, e nao GATHER: reflexao PRECISA ver o vidro (a vitrine tem que aparecer no reflexo).
        // E a divisao do Lumen — o gather difuso ignora translucido, o passe de reflexao o inclui.
        q.TraceRayInline(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, SMILE_RT_MASK_ALL, ray);
        SMILE_RT_PROCEED(q)

        FHitShadeParams P;
        P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
        P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
        P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
        P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
        P.SunColor       = SunColor.rgb;        P.ShadowRayBias = TraceParams.w;
        P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
        P.AlbedoLOD      = ReflectParams.w;
        P.RealHitShading = ReflectParams.z > 0.5f;
        P.NumLights      = (int)CameraPos.w; // F5 (w da CameraPos era constante 1.0, livre)
        P.ShadowRayMask  = (uint)SunColor.w;

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
