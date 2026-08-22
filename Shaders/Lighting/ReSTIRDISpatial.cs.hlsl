// ReSTIR DI - Pass B: reuso espacial enviesado + resolve de visibilidade (no maximo 1 raio/pixel).
// O reservoir final NAO e gravado de volta no historico: este marco prefere nao realimentar o
// vies espacial. A/B posterior pode habilitar o caminho do Alg. 5 do paper quando houver medicao.

#include "../GBuffer.hlsli"
#include "../BRDF.hlsli"
#include "../LightsCommon.hlsli"
#include "../Reflections/GGXSample.hlsli"
#include "../GI/DDGICommon.hlsli"
#include "ReSTIRDICommon.hlsli"
#include "MeshLightCommon.hlsli"
#include "DILightSampling.hlsli"

cbuffer ReSTIRDICB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;
    float4 Params;       // x=lightCount, y=frameIndex, z=shadowMask, w=rayEndMargin
    float4 Sampling;     // x=initialCandidates, y=MCap, z=spatialCount, w=spatialRadius
    float4 Reuse;        // x=temporal, y=posRejectScale, z=normalReject, w=maxAge
    float4 TemporalPolicy; // layout identico ao Pass A; y=shadow motion, w=contagem de mesh lights
    float4 RayEpsA;
    float4 RayEpsB;
    row_major float4x4 View; // layout comum; consumido pelo pack do NRD
    row_major float4x4 PrevViewProj;
    float4 MeshSampling; // x=candidatas mesh; y=BRDF-ratio demodulation
};

#include "../RayOffset.hlsli"

Texture2D<float4> GBufferA : register(t0);
Texture2D<float4> GBufferB : register(t1);
Texture2D<float4> GBufferC : register(t2);
Texture2D<float>  Depth    : register(t3);
RaytracingAccelerationStructure Scene : register(t4);
StructuredBuffer<InstanceGeo> Instances : register(t5);
Texture2D<float>  ResW : register(t6); // W (o x1 saiu — ver ReSTIRDICommon.hlsli)
Texture2D<uint4>  ResB : register(t7);
StructuredBuffer<FGPULightFull> Lights : register(t8);
#include "../Temporal/TemporalMotionCommon.hlsli"
StructuredBuffer<FTemporalInstanceTransform> TemporalTransforms : register(t9);
Texture2D<float4> CurrentSurface : register(t10);
Texture2D<float4> PreviousSurface : register(t11);
StructuredBuffer<FTriangleLightGPU> TriLights : register(t12);

SamplerState LinearWrap : register(s1);
RWTexture2D<float4> OutDirect : register(u0);
RWTexture2D<float4> OutDiffuse : register(u1);
RWTexture2D<float4> OutSpecular : register(u2);
RWTexture2D<float4> OutShadowMotion : register(u3); // xy=curUV-prevUV, z=confianca, w=valido
RWTexture2D<float2> OutBrdfRatio : register(u4); // x=difuso, y=especular; aplicado pos-RELAX

#include "../GI/RTAlphaTest.hlsli"

// O ponto visivel deixou de morar no reservoir: vem do depth, que este passe ja carrega pro teste
// de ceu de cada tap. A correcao de vies la embaixo ja fazia exatamente esta conta inline — agora
// e uma so, usada tambem pela rejeicao geometrica dos vizinhos.
float3 WorldFromDepth(int2 p, float deviceZ) {
    const float2 uv  = (float2(p) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wh.xyz / wh.w;
}

// O RELAX acumula melhor a radiancia sem detalhes de material, mas esse historico tambem tende a
// apagar variacao subpixel da normal mapeada. O fator abaixo preserva somente esse residual local:
// avalia a MESMA BRDF/material/direcao de luz com a normal central e com as quatro normais
// vizinhas. Radiancia, visibilidade, pdf e peso do reservoir ficam fora da razao por construcao.
float2 BrdfLuminanceForNormal(float3 normal, float3 V, float3 L, GBufferData centerMaterial) {
    const float3 diffuseColor = centerMaterial.BaseColor * (1.0f - centerMaterial.Metallic);
    const float3 specularColor = lerp(0.04f.xxx, centerMaterial.BaseColor,
                                      centerMaterial.Metallic);
    const float3 transColor = centerMaterial.BaseColor * centerMaterial.Subsurface;
    const float roughness = max(centerMaterial.Roughness, 0.04f);
    const float a2 = roughness * roughness * roughness * roughness;
    float3 diffuse, specular;
    BRDF_DirectAreaSplit(normal, V, L, L, 1.0f, 1.0f.xxx,
                         diffuseColor, specularColor, roughness, a2, transColor,
                         diffuse, specular);
    // Pisos e teto sao os mesmos da implementacao descrita no capitulo: evitam 0/0 e impedem
    // que um highlight numericamente extremo domine a razao antes do clamp final.
    return clamp(float2(DI_Luminance(diffuse), DI_Luminance(specular)),
                 0.001f.xx, 1000.0f.xx);
}

float2 ComputeBrdfRatio(int2 px, float3 centerPos, float3 L,
                        GBufferData centerMaterial) {
    if (MeshSampling.y < 0.5f) return 1.0f.xx;

    const float3 V = normalize(CameraPos.xyz - centerPos);
    const float2 centerBrdf = BrdfLuminanceForNormal(
        centerMaterial.WorldNormal, V, L, centerMaterial);
    const float centerViewZ = max(abs(mul(float4(centerPos, 1.0f), View).z), 1.0e-4f);
    const int2 offsets[4] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
    };

    float2 neighborSum = 0.0f;
    uint validCount = 0u;
    [unroll]
    for (uint i = 0u; i < 4u; ++i) {
        const int2 qpx = px + offsets[i];
        if (qpx.x < 0 || qpx.y < 0 || qpx.x >= (int)ScreenParams.x ||
            qpx.y >= (int)ScreenParams.y) continue;

        const float qz = Depth.Load(int3(qpx, 0));
        if (qz <= 0.0f) continue;
        const float3 qpos = WorldFromDepth(qpx, qz);
        const float qViewZ = abs(mul(float4(qpos, 1.0f), View).z);
        // O depth relativo impede que a cruz atravesse silhuetas. Nao rejeitamos por normal:
        // justamente a variacao de normal mapeada e o detalhe que queremos recolocar.
        if (abs(qViewZ - centerViewZ) > centerViewZ * 0.02f) continue;

        const GBufferData neighbor = DecodeGBuffer(GBufferA.Load(int3(qpx, 0)),
                                                   GBufferB.Load(int3(qpx, 0)),
                                                   GBufferC.Load(int3(qpx, 0)));
        neighborSum += BrdfLuminanceForNormal(neighbor.WorldNormal, V, L, centerMaterial);
        ++validCount;
    }

    if (validCount == 0u) return 1.0f.xx;
    const float2 ratio = centerBrdf / (neighborSum / (float)validCount);
    // Expoente < 1 comprime contraste; 15 evita amplificar fireflies/reservoirs ruins sem limite.
    return clamp(pow(max(ratio, 0.0f.xx), 0.7f), 0.0f.xx, 15.0f.xx);
}

float4 ComputeShadowMotion(int2 px, float3 receiverPos, float3 receiverNormal,
                           float3 blockerPos, uint blockerId, FGPULightFull light) {
    if (TemporalPolicy.y < 0.5f) return 0.0f;
    const uint instanceCount = (uint)CameraPos.w;
    uint receiverId;
    if (!TemporalDecodeInstance(CurrentSurface.Load(int3(px, 0)).w,
                                instanceCount, receiverId) || blockerId >= instanceCount)
        return 0.0f;

    const float3 prevReceiver = TemporalTransformPoint(
        receiverPos, TemporalTransforms[receiverId].CurrentToPrevious);
    const float3 prevNormal = TemporalTransformDirection(
        receiverNormal, TemporalTransforms[receiverId].CurrentToPrevious);
    const float3 prevBlocker = TemporalTransformPoint(
        blockerPos, TemporalTransforms[blockerId].CurrentToPrevious);
    const float3 prevLight = light.PrevPosInvRadius.xyz;

    const float3 blockerLine = prevBlocker - prevLight;
    const float denom = dot(prevNormal, blockerLine);
    if (abs(denom) <= 1.0e-5f) return 0.0f;
    const float t = dot(prevNormal, prevReceiver - prevLight) / denom;
    if (t <= 0.0f) return 0.0f;
    const float3 intersection = prevLight + blockerLine * t;

    float2 prevUv;
    if (!TemporalProjectUv(intersection, PrevViewProj, prevUv)) return 0.0f;

    const int2 prevPx = clamp(int2(prevUv * ScreenParams.xy), int2(0, 0),
                              int2(ScreenParams.xy) - 1);
    const float4 previous = PreviousSurface.Load(int3(prevPx, 0));
    uint previousId;
    if (!TemporalDecodeInstance(previous.w, instanceCount, previousId)) return 0.0f;

    // Eq. 25.4: confianca maxima quando o ponto real anterior se desloca no plano virtual
    // (theta ~= pi/2). A forma gaussiana reduz o peso em receptores curvos/descontinuos.
    const float3 delta = previous.xyz - prevReceiver;
    const float deltaLen = length(delta);
    float confidence = 1.0f;
    if (deltaLen > 1.0e-5f) {
        const float theta = acos(clamp(dot(delta / deltaLen, prevNormal), -1.0f, 1.0f));
        const float angleError = theta - 1.57079632679f;
        confidence = exp(-0.5f * angleError * angleError / (0.1f * 0.1f));
    }
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    return float4(uv - prevUv, confidence, 1.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 upx = dtid.xy;
    if (upx.x >= (uint)ScreenParams.x || upx.y >= (uint)ScreenParams.y) return;
    const int2 px = int2(upx);

    const float deviceZ = Depth.Load(int3(px, 0));
    const uint lightCount = (uint)Params.x;
    const uint triCount   = (uint)TemporalPolicy.w;
    const uint totalCount = lightCount + triCount;
    if (deviceZ <= 0.0f || totalCount == 0u) {
        OutDirect[px] = OutDiffuse[px] = OutSpecular[px] = 0.0f;
        OutShadowMotion[px] = 0.0f;
        OutBrdfRatio[px] = 1.0f.xx;
        return;
    }

    const GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                        GBufferB.Load(int3(px, 0)),
                                        GBufferC.Load(int3(px, 0)));
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float3 x1 = WorldFromDepth(px, deviceZ);
    const float3 n1 = g.WorldNormal;

    uint rng = GGX_SeedE(upx, (uint)Params.y, SMILE_RNG_DI_SPATIAL);
    ReSTIRDIReservoir r;
    DIResInit(r);
    r.X1 = x1;
    r.N1Oct = DDGI_OctEncode(n1);

    // Dominios combinados, p/ a correcao de vies abaixo. [0] e sempre o proprio pixel.
    int2  candPx[9];
    float candM[9];
    int   candCount = 0;
    int   selCand = 0; // dominio que gerou a amostra vencedora

    ReSTIRDIReservoir self = DI_LoadReservoir(ResW.Load(int3(px, 0)), ResB.Load(int3(px, 0)));
    self.X1 = x1; // mesmo pixel, mesmo depth: e o x1 ja reconstruido acima
    self.M = min(self.M, Sampling.y);
    // Basta ter M: um reservoir SEM amostra valida (todas as candidatas deram target 0, ou o
    // Alg. 5 passo 2 descartou a ocluida) ainda sorteou M candidatas e precisa entrar na soma.
    // Pular o merge inteiro descartaria esse M e INFLARIA o brilho, porque o resolve divide por
    // ele. Com target 0 o peso e 0, a amostra nunca e adotada, e so o M entra.
    if (self.M > 0.0f) {
        float target = 0.0f;
        if (self.LightIndex < totalCount && self.W > 0.0f) {
            const DILightSample ls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                       self.LightIndex, self.UV, x1);
            float3 diff, spec, L; float dist;
            target = DI_TargetFromSample(ls, g, x1, CameraPos.xyz, diff, spec, L, dist);
        }
        if (DIResMerge(r, self, target, rng)) selCand = candCount;
        candPx[candCount] = px; candM[candCount] = self.M; ++candCount;
    }

    const int K = min((int)Sampling.z, 8);
    const float posReject = Reuse.y * max(length(CameraPos.xyz - x1), 1.0f);
    [loop]
    for (int i = 0; i < K; ++i) {
        const float2 xi = GGX_Rand2E(upx, (uint)Params.y, SMILE_RNG_DI_SPATIAL_TAP + (uint)i);
        const int2 qpx = px + int2(round(GGX_ConcentricDisk(xi) * Sampling.w));
        if (qpx.x < 0 || qpx.y < 0 || qpx.x >= (int)ScreenParams.x || qpx.y >= (int)ScreenParams.y ||
            all(qpx == px)) continue;

        const float qz = Depth.Load(int3(qpx, 0));
        if (qz <= 0.0f) continue;
        const GBufferData qg = DecodeGBuffer(GBufferA.Load(int3(qpx, 0)),
                                             GBufferB.Load(int3(qpx, 0)),
                                             GBufferC.Load(int3(qpx, 0)));
        if (dot(qg.WorldNormal, n1) < Reuse.z) continue;

        ReSTIRDIReservoir nb = DI_LoadReservoir(ResW.Load(int3(qpx, 0)), ResB.Load(int3(qpx, 0)));
        // So M > 0 — mesma razao do bloco do self. Reservoir invalido (fundo/ceu) tem M = 0 e cai
        // aqui. O qz > 0 acima ja garante que o X1 reconstruido abaixo e de geometria real, entao
        // os testes geometricos continuam validos.
        if (nb.M <= 0.0f) continue;
        nb.X1 = WorldFromDepth(qpx, qz);
        if (length(nb.X1 - x1) >= posReject ||
            abs(dot(n1, nb.X1 - x1)) >= 0.2f * posReject) continue;
        nb.M = min(nb.M, Sampling.y);

        float target = 0.0f;
        if (nb.LightIndex < totalCount && nb.W > 0.0f) {
            // Reamostra com o uv do VIZINHO no dominio DESTE pixel — e o que torna o reuso valido.
            const DILightSample ls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                       nb.LightIndex, nb.UV, x1);
            float3 diff, spec, L; float dist;
            target = DI_TargetFromSample(ls, g, x1, CameraPos.xyz, diff, spec, L, dist);
        }
        if (DIResMerge(r, nb, target, rng)) selCand = candCount;
        candPx[candCount] = qpx; candM[candCount] = nb.M; ++candCount;
    }

    if (r.LightIndex >= totalCount) {
        OutDirect[px] = OutDiffuse[px] = OutSpecular[px] = 0.0f;
        OutShadowMotion[px] = 0.0f;
        OutBrdfRatio[px] = 1.0f.xx;
        return;
    }

    const DILightSample selSample = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                      r.LightIndex, r.UV, x1);
    float3 diffuse, specular, L; float dist;
    const float selectedTarget = DI_TargetFromSample(selSample, g, x1, CameraPos.xyz,
                                                     diffuse, specular, L, dist);
    OutBrdfRatio[px] = selSample.Valid
        ? ComputeBrdfRatio(px, x1, L, g) : 1.0f.xx;

    // Correcao de vies (Alg. 6 / Secao 4.3), mesma convencao do ReSTIRGISpatial.cs.hlsl:
    // balance heuristic continua no lugar do 1/M. ps_c = pHat da luz VENCEDORA avaliada no
    // dominio de cada participante; W = wSum * pi / (pHatSel * Σ ps_c·M_c), com pi = ps do
    // dominio que gerou o vencedor.
    //
    // O 1/M puro (Alg. 4) supoe que todo vizinho poderia ter gerado a amostra, e escurece onde
    // isso e falso: banda do terminador de cada luz (N·L ~ 0 no vizinho), borda do cone do spot
    // e limite do raio de atenuacao, onde PunctualLightIncoming zera EXATAMENTE. Aqui o vizinho
    // que mal poderia gerar a amostra vota pouco no denominador — menos variancia que o teste
    // binario de Z, e degenera em 1/M quando todos os dominios concordam.
    //
    // Versao SEM raios extras: a Secao 4.4 pede tambem testar visibilidade de cada vizinho ate a
    // luz (K raios/pixel). Fica para depois de medir; o resolve de visibilidade abaixo ja cobre
    // o dominio do proprio pixel.
    {
        float pi = 0.0f, piSum = 0.0f;
        [loop]
        for (int cd = 0; cd < candCount; ++cd) {
            // cd 0 e o proprio pixel: ps == selectedTarget por construcao, nao recarrega nada.
            float ps = selectedTarget;
            if (cd > 0) {
                ps = 0.0f;
                const int2 cp = candPx[cd];
                const float cz = Depth.Load(int3(cp, 0));
                if (cz > 0.0f) {
                    const GBufferData cg = DecodeGBuffer(GBufferA.Load(int3(cp, 0)),
                                                         GBufferB.Load(int3(cp, 0)),
                                                         GBufferC.Load(int3(cp, 0)));
                    const float3 cx1 = WorldFromDepth(cp, cz);
                    // A amostra vencedora vista do dominio do vizinho: reamostra com o MESMO uv
                    // no ponto DELE, senao a pdf comparada seria a de outra amostra.
                    const DILightSample cls = DI_SampleAnyLight(Lights, lightCount, TriLights,
                                                                triCount, r.LightIndex, r.UV, cx1);
                    float3 cdf, csp, cl; float cdst;
                    ps = DI_TargetFromSample(cls, cg, cx1, CameraPos.xyz, cdf, csp, cl, cdst);
                }
            }
            piSum += ps * candM[cd];
            if (cd == selCand) pi = ps;
        }
        r.W = (selectedTarget > 0.0f && pi > 0.0f && piSum > 0.0f)
            ? (r.WeightSum * pi / (piSum * selectedTarget)) : 0.0f;
    }
    float visibility = 1.0f;
    float4 shadowMotion = 0.0f;

    // Triangulo emissivo sempre projeta sombra; a flag do artista so vale p/ a luz analitica.
    const bool selIsTri = DI_IsTriangleIndex(r.LightIndex, lightCount);
    if (r.W > 0.0f && selSample.Valid &&
        (selIsTri || DI_IsShadowCaster(Lights[r.LightIndex]))) {
        const float camDist = length(CameraPos.xyz - x1);
        const float3 origin = OffsetRayGBuffer(x1, n1, L, camDist);
        // O ponto vem da AMOSTRA guardada no reservoir, entao o Pass A e o Pass B miram o mesmo
        // lugar por construcao. O seed determinístico que fazia esse papel saiu junto.
        const float3 toLight = selSample.Position - origin;
        const float len = length(toLight);
        const float tMax = len - max(Params.w, 0.0f);
        if (len > 1e-6f && tMax > RayEpsB.x) {
            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = toLight / len;
            ray.TMin = RayEpsB.x;
            ray.TMax = tMax;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
            query.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                                 (uint)Params.z, ray);
            SMILE_RT_PROCEED(query)
            if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                visibility = 0.0f;
                const float3 blockerPos = ray.Origin + ray.Direction * query.CommittedRayT();
                // So p/ luz analitica: o plano virtual precisa da posicao ANTERIOR da luz, e
                // triangulo emissivo nao tem esse par (a malha e estatica, e quando deixar de ser
                // a correspondencia certa e a transform da instancia, nao um PrevPos).
                if (!selIsTri)
                    shadowMotion = ComputeShadowMotion(px, x1, n1, blockerPos,
                                                       query.CommittedInstanceID(),
                                                       Lights[r.LightIndex]);
            }
        }
    }

    const float3 diffuseEstimate = diffuse * (r.W * visibility);
    const float3 specularEstimate = specular * (r.W * visibility);
    // Igual ao RTXDI: hit distance zero quando a amostra nao entrega radiancia valida. Isso evita
    // ensinar ao RELAX uma superficie virtual atraves de um bloqueador ou de um reservoir vazio.
    const float hitDist = (visibility > 0.0f && r.W > 0.0f &&
                           any(diffuseEstimate + specularEstimate > 0.0f)) ? dist : 0.0f;
    OutDirect[px]  = float4(diffuseEstimate + specularEstimate, hitDist);
    OutDiffuse[px] = float4(diffuseEstimate, hitDist);
    OutSpecular[px] = float4(specularEstimate, hitDist);
    OutShadowMotion[px] = shadowMotion;
}
