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
    float4 PrevCameraPos;
    float4 TemporalParams;
    float4 DebugParams;     // w = slot bindless do alvo de timer (< 0 = captura off)
    row_major float4x4 View;
    float4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
    float4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
    float4 PolicyParams;            // x = politica deste passe (backface/culling)
    // Gather do 2o bounce (contrato do HitShading.hlsli).
    float4 GIDistParams;            // x=distTile, y=distW, z=distH, w=skipMode
    float4 GIBiasParams;            // x=escala do bias, y=teto em metros, zw=-
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    // Cauda da agua: nao usada aqui, declarada p/ alcancar o offset do SkyParams (mesma
    // convencao dos campos de preenchimento acima).
    row_major float4x4 ViewProj;
    float4 WaterEnvironmentParams;
    float4 SkyParams;       // x = view height (km), y = raio do planeta (km) — ver ShadeSky
    // O Renderer publica estas linhas SEM o bit de update para as reflexoes: a radiancia daqui e
    // direcional e o cache nao guarda direcao. Consultar continua valido. Ver ShaderParams().
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    // Cascatas do DDGI, consumidas pelo gather do 2o bounce no HitShading (contrato por NOME).
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    // 6.2b-ii: scroll toroidal, em CELULAS, por cascata (xyz). Espelha o ScrollOffset do
    // FDDGICascadeConstants — o bloco e copiado campo-a-campo, entao a ORDEM e o contrato.
    float4 GICascadeScrollOffset[4];
};

// Politica de culling DESTE passe. O Lumen culla na reflexao e nao culla no gather do ReSTIR; a
// ray flag e por raio, entao os dois regimes convivem na mesma TLAS (que marca
// TRIANGLE_CULL_DISABLE so no que e mesmo two-sided).
uint ReflectionCullFlags() {
    return (PolicyParams.x > 0.5f) ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE;
}

#include "../RayOffset.hlsli" // depois do cbuffer: le RayEpsA/RayEpsB
#include "../Debug/ShaderTimer.hlsli"

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
// t4/t5: atlas de distancia e ProbeData do DDGI — o 2o bounce usa o gather COMPLETO
// (Chebyshev + bias + skip), igual ao deferred. Antes eram filler do VB/IB bindless.
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t5);

Texture2D<float>                Depth      : register(t6);
Texture2D<float4>               GBuffer    : register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8); // F5: luzes puntuais nos hits
#include "../Temporal/TemporalMotionCommon.hlsli"
StructuredBuffer<FTemporalInstanceTransform> TemporalTransforms : register(t9);
Texture2D<float4> TemporalSurface : register(t10);
Texture2D<float4> PrevTemporalSurface : register(t11);

RWTexture2D<float4>             RWReflection : register(u0);
RWTexture2D<float4>             RWRayData    : register(u1); 
RWTexture2D<float4>             RWGlossyMotion : register(u2);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli" 
#include "ReflectionMotion.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 halfPx = DTid.xy;
    if (halfPx.x >= (uint)HalfScreenParams.x || halfPx.y >= (uint)HalfScreenParams.y) return;

    // O alvo de timer deste passe e HALF-RES (o dominio do dispatch), entao a medida vai em
    // halfPx. O visualizador ja preserva o aspecto de alvo com resolucao propria.
    SMILE_TIMER_BEGIN(timerStart)

    int2 fullPx = int2(halfPx) * 2 + RefTileJitter(halfPx, (uint)TraceParams.x);
    fullPx = min(fullPx, int2((int)ScreenParams.x - 1, (int)ScreenParams.y - 1));

    float3 outRadiance = float3(0.0f, 0.0f, 0.0f);
    float  outHitDist  = TraceParams.y;
    float4 outRay      = float4(0.0f, 0.0f, 0.0f, 0.0f); 
    float4 outMotion   = float4(0.0f, 0.0f, 0.0f, 0.0f);

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
            float2 E = GGX_Rand2E((uint2)fullPx, (uint)TraceParams.x, SMILE_RNG_REFL_GLOSSY);
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

        RayQuery<RAY_FLAG_NONE> q;
        // ALL, e nao GATHER: reflexao PRECISA ver o vidro (a vitrine tem que aparecer no reflexo).
        // E a divisao do Lumen — o gather difuso ignora translucido, o passe de reflexao o inclui.
        //
        // Culling DINAMICO (PolicyParams.x), no molde do Context.CullingMode do Lumen: a flag do
        // template fica em NONE e a decisao vai na chamada, porque as duas sao combinadas pela
        // spec do DXR e uma flag estatica de culling nao teria como ser desligada.
        q.TraceRayInline(Scene, ReflectionCullFlags(), SMILE_RT_MASK_ALL, ray);
        SMILE_RT_PROCEED(q)

        FHitShadeParams P;
        P.GridMin        = GridMinSpacing.xyz;  P.Spacing      = GridMinSpacing.w;
        P.Count          = (int3)GridCount.xyz; P.AtlasTile    = (int)AtlasParams.x;
        P.AtlasInvSize   = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
        P.SunDir         = sunDir;              P.SunIntensity = SunDirIntensity.w;
        P.SunColor       = SunColor.rgb;        P.ShadowRayBias = TraceParams.w;
        P.SkyIntensity   = TraceParams.z;       P.MaxRayDist   = TraceParams.y;
        P.AlbedoLOD      = ReflectParams.w;
        P.NumLights      = (int)CameraPos.w; // F5 (w da CameraPos era constante 1.0, livre)
        P.ShadowRayMask  = (uint)SunColor.w;
        P.ReGIRGridMin       = ReGIRGridMinSlots.xyz;
        P.ReGIRSlotsPerCell  = (uint)ReGIRGridMinSlots.w;
        P.ReGIRInvCellSize   = ReGIRInvCellEnabled.xyz;
        P.ReGIREnabled       = ReGIRInvCellEnabled.w > 0.5f;
        P.ReGIRGridCount     = (int3)ReGIRGridCountSamples.xyz;
        P.ReGIRSampleCount   = (int)ReGIRGridCountSamples.w;
        P.ReGIRSlotsSRV      = (uint)ReGIRResources.x;
        P.ReGIRAverageSRV    = (uint)ReGIRResources.y;
        P.FrameIndex         = (uint)TraceParams.x;
        P.ReGIRPad           = 0u;
        P.SkyViewHeightKm    = SkyParams.x;
        P.SkyBottomRKm       = SkyParams.y;
        // ZERO de proposito: a reflexao sombreia o hit p/ uma visada conhecida e consome uma vez
        // so, sem reuso — o piso que o DDGI/ReSTIR precisam borraria espelho-no-espelho aqui.
        P.RoughnessMin       = 0.0f;
        P.CacheRayRoughness  = roughness;
        RC_UNPACK_PARAMS(P);

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            float sd;
            outHitDist  = q.CommittedRayT();
            outRadiance = ShadeSurfaceHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                          q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                          ray.Origin, ray.Direction, outHitDist, P, sd);
            const float3 secondaryHit = ray.Origin + ray.Direction * outHitDist;
            outMotion = ComputeGlossyMotion((uint2)fullPx, worldPos, N, secondaryHit,
                                            q.CommittedInstanceID(), roughness,
                                            SMILE_RNG_REFL_GLOSSY + 137u);
        } else {
            outRadiance = ShadeSky(R, sunDir, P.SkyIntensity, P);
        }
    }

    RWReflection[halfPx] = float4(outRadiance, outHitDist);
    RWRayData[halfPx]    = outRay;
    RWGlossyMotion[halfPx] = outMotion;

    SMILE_TIMER_END(timerStart, halfPx, DebugParams.w)
}
