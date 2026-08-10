#include "../GI/DDGICommon.hlsli"
#include "../GBuffer.hlsli"
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
    row_major float4x4 PrevViewProj;
    float4 PrevCameraPos;
    float4 TemporalParams;
    float4 DebugParams;
    row_major float4x4 View;
    float4 RayEpsA;
    float4 RayEpsB;
    float4 PolicyParams;
    float4 GIDistParams;
    float4 GIBiasParams;
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    row_major float4x4 ViewProj;
    float4 WaterEnvironmentParams; // x=atmosphere, y=intensity, z=env max mip, w=scene max mip
    float4 SkyParams;              // x = view height (km), y = raio do planeta (km) — ShadeSky
    // Sem bit de update (radiancia direcional) — ver ReflectionTrace.cs.hlsl.
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
};

uint ReflectionCullFlags() {
    return (PolicyParams.x > 0.5f) ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE;
}

#include "../RayOffset.hlsli"

RaytracingAccelerationStructure Scene      : register(t0);
Texture2D<float4>               SkyViewLUT : register(t1);
StructuredBuffer<InstanceGeo>   Instances  : register(t2);
Texture2D<float4>               IrradAtlas : register(t3);
Texture2D<float4>               GIDistAtlas : register(t4);
Buffer<float4>                  GIProbeData : register(t5);
Texture2D<float>                Depth       : register(t6);
Texture2D<float4>               GBuffer     : register(t7);

#include "../LightsCommon.hlsli"
StructuredBuffer<FPunctualLight> SceneLights : register(t8);
Texture2D<float2>                Velocity    : register(t9);
Texture2D<float4>                GBufferC    : register(t10);
Texture2D<float4>                SceneColorNoWater : register(t11);
Texture2D<float>                 SceneDepthNoWater : register(t12);
TextureCube<float4>              AtmosphereSpecular : register(t13);
TextureCube<float4>              HDRSpecular        : register(t14);

RWTexture2D<float4> RWReflection : register(u0);
RWTexture2D<float4> RWReceiverMotion : register(u1);

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "../GI/HitShading.hlsli"

bool WaterProjectToScreen(float3 worldPos, out float2 uv) {
    const float4 clip = mul(float4(worldPos, 1.0f), ViewProj);
    if (clip.w <= 1.0e-4f) {
        uv = 0.0f;
        return false;
    }
    const float2 ndc = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    return all(uv > 0.0f) && all(uv < 1.0f);
}

float3 WaterWorldFromDepth(float2 uv, float deviceZ) {
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldH = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return worldH.xyz / worldH.w;
}

float3 WaterOffSpecularPeak(float3 N, float3 reflectionDirection, float roughness) {
    // Mesmo ajuste usado pelo composite de Single Layer Water da Unreal: quando o lobo alarga,
    // seu pico migra da reflexao perfeita em direcao a normal macroscopica.
    const float a = roughness * roughness;
    const float peak = (1.0f - a) * (sqrt(saturate(1.0f - a)) + a);
    return normalize(lerp(N, reflectionDirection, peak));
}

float3 WaterSamplePrefilteredEnvironment(float3 N, float3 reflectionDirection,
                                         float roughness) {
    const float3 lookup = WaterOffSpecularPeak(N, reflectionDirection, roughness);
    const float lod = saturate(roughness) * max(WaterEnvironmentParams.z, 0.0f);
    const float3 atmosphere = AtmosphereSpecular.SampleLevel(LinearClamp, lookup, lod).rgb;
    const float3 hdri = HDRSpecular.SampleLevel(LinearClamp, lookup, lod).rgb;
    return max(lerp(hdri, atmosphere, saturate(WaterEnvironmentParams.x)), 0.0f) *
           max(WaterEnvironmentParams.y, 0.0f);
}

// Primeiro nivel da hierarquia de reflexao: a cena ja iluminada e SEM agua contem exatamente
// os objetos on-screen que precisam de contato (barco, esfera, costa). A marcha segue o molde do
// water SSR do Cry: amostras redistribuidas quadraticamente e teste em view-Z, independente da
// convencao reverse-Z do depth buffer. DXR so entra quando este caminho nao cobre o raio.
bool WaterTraceContactSSR(uint2 px, float3 origin, float3 direction, float roughness,
                          out float3 reflectedRadiance, out float reflectionHitDist,
                          out float confidence) {
    reflectedRadiance = 0.0f;
    reflectionHitDist = 0.0f;
    confidence = 0.0f;

    const float receiverDistance = length(CameraPos.xyz - origin);
    const float rayStart = max(0.08f, receiverDistance * 2.0e-4f);
    const float rayEnd = min(TraceParams.y, max(32.0f, receiverDistance * 1.5f));
    if (rayEnd <= rayStart) return false;

    static const uint kSteps = 40u;
    const float stableJitter = GGX_Rand2E(px, 0u, SMILE_RNG_REFL_GLOSSY + 733u).x;
    float previousT = rayStart;
    float previousDelta = -1.0e20f;
    bool enteredScreen = false;

    [loop] for (uint i = 0u; i < kSteps; ++i) {
        const float u = saturate((float(i) + 0.35f + 0.3f * stableJitter) / float(kSteps));
        const float t = lerp(rayStart, rayEnd, u * u);
        const float3 rayPos = origin + direction * t;
        float2 sampleUv;
        if (!WaterProjectToScreen(rayPos, sampleUv)) {
            if (enteredScreen) break;
            previousT = t;
            continue;
        }
        enteredScreen = true;

        const int2 samplePx = clamp(int2(sampleUv * ScreenParams.xy), int2(0, 0),
                                    int2(ScreenParams.xy) - 1);
        const float sceneDeviceZ = SceneDepthNoWater.Load(int3(samplePx, 0));
        if (sceneDeviceZ <= 0.0f) {
            // Ceu nao e uma superficie. Mantemos o raio do lado "da frente" para que a primeira
            // silhueta opaca encontrada ainda possa formar uma travessia valida.
            previousDelta = -1.0e20f;
            previousT = t;
            continue;
        }

        const float3 scenePos = WaterWorldFromDepth(sampleUv, sceneDeviceZ);
        const float rayViewZ = abs(mul(float4(rayPos, 1.0f), View).z);
        const float sceneViewZ = abs(mul(float4(scenePos, 1.0f), View).z);
        const float delta = rayViewZ - sceneViewZ;
        const float thickness = max(0.10f, sceneViewZ * 0.008f + roughness * 0.20f);
        const bool crossed = delta >= -thickness && previousDelta < -thickness;
        const bool closeEnough = abs(delta) <= thickness;

        if (crossed || closeEnough) {
            // Refina o intervalo em view-Z. Em uma borda de silhueta, depth pode alternar entre
            // ceu e objeto; nesse caso o lado sem depth permanece como "antes do hit".
            float lo = previousT;
            float hi = t;
            float2 hitUv = sampleUv;
            [unroll] for (uint refine = 0u; refine < 5u; ++refine) {
                const float mid = 0.5f * (lo + hi);
                const float3 midPos = origin + direction * mid;
                float2 midUv;
                if (!WaterProjectToScreen(midPos, midUv)) {
                    lo = mid;
                    continue;
                }
                const int2 midPx = clamp(int2(midUv * ScreenParams.xy), int2(0, 0),
                                         int2(ScreenParams.xy) - 1);
                const float midDeviceZ = SceneDepthNoWater.Load(int3(midPx, 0));
                if (midDeviceZ <= 0.0f) {
                    lo = mid;
                    continue;
                }
                const float3 midScenePos = WaterWorldFromDepth(midUv, midDeviceZ);
                const float midRayZ = abs(mul(float4(midPos, 1.0f), View).z);
                const float midSceneZ = abs(mul(float4(midScenePos, 1.0f), View).z);
                if (midRayZ >= midSceneZ) {
                    hi = mid;
                    hitUv = midUv;
                } else {
                    lo = mid;
                }
            }

            const float2 border = min(hitUv, 1.0f - hitUv);
            const float edgeConfidence = saturate(min(border.x, border.y) / 0.035f);
            // Confianca nao e atenuacao por distancia. A forma antiga (1 - hit/rayEnd)
            // misturava TODO hit distante com o ceu e fazia o reflexo mudar quando a camera
            // recuava. So precisamos de fade no ultimo 15% da marcha, onde o truncamento pode
            // cortar o intervalo de interseccao.
            const float distanceConfidence = saturate((rayEnd - hi) /
                                                       max(rayEnd * 0.15f, 1.0e-3f));
            confidence = edgeConfidence * distanceConfidence;
            if (confidence <= 0.01f) return false;

            // Integra a radiancia no footprint aproximado do lobo GGX. O depth continua em
            // resolucao cheia para localizar o hit; somente a radiancia escolhe o mip. A norma
            // da coluna Y remove a rotacao da view e recupera cot(fovY/2) do ViewProj.
            const int2 hitPx = clamp(int2(hitUv * ScreenParams.xy), int2(0, 0),
                                     int2(ScreenParams.xy) - 1);
            const float hitDeviceZ = SceneDepthNoWater.Load(int3(hitPx, 0));
            const float3 hitScenePos = WaterWorldFromDepth(hitUv, hitDeviceZ);
            const float hitViewZ = max(abs(mul(float4(hitScenePos, 1.0f), View).z), 0.25f);
            const float projectionY = max(length(float3(ViewProj[0][1], ViewProj[1][1],
                                                        ViewProj[2][1])), 1.0e-3f);
            const float pixelsPerMeter = 0.5f * ScreenParams.y * projectionY / hitViewZ;
            const float alpha = roughness * roughness;
            const float coneRadiusPixels = max(1.0f, 0.35f * hi * alpha * pixelsPerMeter);
            const float radianceMip = clamp(log2(coneRadiusPixels), 0.0f,
                                            max(WaterEnvironmentParams.w, 0.0f));
            reflectedRadiance = max(
                SceneColorNoWater.SampleLevel(LinearClamp, hitUv, radianceMip).rgb, 0.0f);
            reflectionHitDist = hi;
            return true;
        }

        previousDelta = delta;
        previousT = t;
    }
    return false;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const uint2 px = DTid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y) return;

    float3 radiance = 0.0f;
    float hitDist = 0.0f; // miss/ceu = zero no contrato do RR
    float4 receiverMotion = 0.0f;

    const float4 gb = GBuffer.Load(int3(px, 0));
    const uint shadingModel = GBuffer_DecodeShadingModel(gb.a);
    const float deviceZ = Depth.Load(int3(px, 0)).r;
    const float4 waterData = GBufferC.Load(int3(px, 0));
    const float reflectionCoverage = waterData.g;
    if (shadingModel == SMILE_SHADINGMODEL_WATER && deviceZ > 0.0f &&
        reflectionCoverage > (0.5f / 255.0f)) {
        const float roughness = max(gb.b, 0.04f);
        const float3 N = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);
        const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float4 wH = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
        const float3 worldPos = wH.xyz / wH.w;
        const float3 V = normalize(CameraPos.xyz - worldPos);

        // A busca de geometria local precisa ser coerente entre pixels/frames. Jitterar o proprio
        // raio de interseccao pelo VNDF fazia objetos pequenos (barco, esfera, costa) virarem hits
        // raros que o neighborhood clamp temporal classificava como outliers. O Cry segue a mesma
        // separacao no water SSR: busca com a superficie resolvida e aplica a largura do lobo depois.
        //
        // O ambiente e prefiltrado offline/compute por roughness; nao precisa jitterar o raio de
        // geometria nem depender do historico para reconstruir um lobo de ceu estavel.
        const float3 RLocal = reflect(-V, N);
        const float3 environmentRadiance =
            WaterSamplePrefilteredEnvironment(N, RLocal, roughness);

        const float3 sunDir = normalize(SunDirIntensity.xyz);
        FHitShadeParams P;
        P.GridMin = GridMinSpacing.xyz; P.Spacing = GridMinSpacing.w;
        P.Count = (int3)GridCount.xyz; P.AtlasTile = (int)AtlasParams.x;
        P.AtlasInvSize = float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z);
        P.SunDir = sunDir; P.SunIntensity = SunDirIntensity.w;
        P.SunColor = SunColor.rgb; P.ShadowRayBias = TraceParams.w;
        P.SkyIntensity = TraceParams.z; P.MaxRayDist = TraceParams.y;
        P.AlbedoLOD = ReflectParams.w;
        P.NumLights = (int)CameraPos.w; P.ShadowRayMask = (uint)SunColor.w;
        P.ReGIRGridMin = ReGIRGridMinSlots.xyz;
        P.ReGIRSlotsPerCell = (uint)ReGIRGridMinSlots.w;
        P.ReGIRInvCellSize = ReGIRInvCellEnabled.xyz;
        P.ReGIREnabled = ReGIRInvCellEnabled.w > 0.5f;
        P.ReGIRGridCount = (int3)ReGIRGridCountSamples.xyz;
        P.ReGIRSampleCount = (int)ReGIRGridCountSamples.w;
        P.ReGIRSlotsSRV = (uint)ReGIRResources.x;
        P.ReGIRAverageSRV = (uint)ReGIRResources.y;
        P.FrameIndex = (uint)TraceParams.x; P.ReGIRPad = 0u;
        P.SkyViewHeightKm = SkyParams.x;    P.SkyBottomRKm = SkyParams.y;
        // ZERO: hit direcional e consumido uma vez (ver ReflectionTrace.cs.hlsl).
        P.RoughnessMin = 0.0f;              P.CacheRayRoughness = roughness;
        RC_UNPACK_PARAMS(P);

        float ssrConfidence;
        float3 ssrRadiance;
        float ssrHitDist;
        const bool ssrHit = WaterTraceContactSSR(px, worldPos, RLocal, roughness,
                                                  ssrRadiance, ssrHitDist, ssrConfidence);

        // A hierarquia e SSR -> DXR -> ambiente inclusive na faixa de confianca parcial.
        // Antes um SSR parcial pulava direto para o ceu, enquanto um miss total chamava DXR;
        // ao mover a camera isso alternava entre duas solucoes e produzia pop/fade espacial.
        float3 fallbackRadiance = environmentRadiance;
        float fallbackHitDist = 0.0f;
        const float missingSSR = ssrHit ? (1.0f - ssrConfidence) : 1.0f;
        if (missingSSR > 0.01f) {
            RayDesc ray;
            ray.Origin = OffsetRayGBuffer(worldPos, N, RLocal,
                                          length(CameraPos.xyz - worldPos));
            ray.Direction = RLocal;
            ray.TMin = 0.0f;
            ray.TMax = TraceParams.y;

            RayQuery<RAY_FLAG_NONE> q;
            q.TraceRayInline(Scene, ReflectionCullFlags(), SMILE_RT_MASK_ALL, ray);
            SMILE_RT_PROCEED(q)

            if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                float secondaryDepth;
                fallbackHitDist = q.CommittedRayT();
                fallbackRadiance = ShadeSurfaceHit(
                    q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                    q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                    ray.Origin, ray.Direction, fallbackHitDist, P, secondaryDepth);
            }
        }

        if (ssrHit) {
            // Equivalente a SSR premultiplicado + fallback * (1-alpha), mas mantendo a radiancia
            // do SSR nao-premultiplicada para o filtro temporal.
            radiance = lerp(fallbackRadiance, ssrRadiance, ssrConfidence);
            hitDist = (ssrConfidence >= 0.5f) ? ssrHitDist : fallbackHitDist;
        } else {
            radiance = fallbackRadiance;
            hitDist = fallbackHitDist;
        }

        // A velocity foi escrita pelo VS da agua com a fase FFT anterior, portanto captura o
        // movimento que faltava ao caminho antigo. Movimento do hit secundario fica para a
        // extensao de specular motion; confianca conservadora limita o comprimento do historico.
        const float2 motion = Velocity.Load(int3(px, 0));
        receiverMotion = float4(motion, saturate(0.85f - roughness * 0.35f), 1.0f);
    }

    RWReflection[px] = float4(radiance, hitDist);
    RWReceiverMotion[px] = receiverMotion;
}
