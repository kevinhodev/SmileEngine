// ReSTIR DI - Pass A: candidatas iniciais uniformes por pixel + reuso temporal.
// Escreve o reservoir que vira historico. O espacial nao realimenta este buffer, evitando acumular
// o vies do filtro espacial no historico temporal.

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
    float4 ScreenParams; // W, H, 1/W, 1/H
    float4 Params;       // x=lightCount, y=frameIndex, z=shadowMask, w=rayEndMargin
    float4 Sampling;     // x=initialCandidates, y=MCap, z=spatialCount, w=spatialRadius
    float4 Reuse;        // x=temporal(0/1), y=posRejectScale, z=normalReject, w=maxAge
    float4 TemporalPolicy; // x=permutation, z=visibilidade inicial, w=contagem de mesh lights; y reserv.
    float4 RayEpsA;      // layout compartilhado com o Pass B
    float4 RayEpsB;
    row_major float4x4 View; // layout comum; consumido pelo pack do NRD
    // Nao lido aqui. Declarado porque o cbuffer e posicional: sem ele o MeshSampling cairia no
    // offset do PrevViewProj e o orcamento de mesh viria de uma linha de matriz.
    row_major float4x4 PrevViewProj;
    float4 MeshSampling; // x=candidatas mesh; y=BRDF-ratio (consumido pelo Pass B); zw livres
};

#include "../RayOffset.hlsli"

Texture2D<float4> GBufferA : register(t0);
Texture2D<float4> GBufferB : register(t1);
Texture2D<float4> GBufferC : register(t2);
Texture2D<float>  Depth    : register(t3);
Texture2D<float2> Velocity : register(t4);
Texture2D<float>  PrevResW : register(t5); // W (o x1 saiu — ver ReSTIRDICommon.hlsli)
Texture2D<uint4>  PrevResB : register(t6); // x=light, y=uv(reserv), z=M+idade, w=n1 oct
StructuredBuffer<FGPULightFull> Lights : register(t7);
RaytracingAccelerationStructure Scene : register(t8);
StructuredBuffer<InstanceGeo> Instances : register(t9);
StructuredBuffer<FTriangleLightGPU> TriLights : register(t10);
StructuredBuffer<FMeshLightAlias>   MeshAlias : register(t11);
// Historico de superficie do frame ANTERIOR: float4(worldPos, instanceId+1). De onde sai o X1 do
// reservoir temporal. Valido so quando TemporalPolicy.y (motion history) esta ligado.
Texture2D<float4> PrevSurface : register(t12);

SamplerState LinearWrap : register(s1);

RWTexture2D<float>  CurrResW : register(u0);
RWTexture2D<uint4>  CurrResB : register(u1);

#include "../GI/RTAlphaTest.hlsli"

// Alg. 5, passo 2 do paper (visibility reuse). A target PDF ignora visibilidade de proposito
// (Secao 5), entao uma luz forte porem OCLUIDA tem pHat enorme e domina o WRS. Sem este teste ela
// trava no reservoir, o raio final do Pass B devolve 0 e o pixel emite ZERO — enquanto a luz fraca
// porem VISIVEL que deveria iluminar aquela sombra quase nunca ganha o sorteio. Nao e vazamento de
// luz (o Pass B sempre testa oclusao no destino): e fome de energia na penumbra. Testar aqui, antes
// do reuso temporal, impede que a amostra ocluida contamine o historico.
// O ponto na luz agora vem da amostra (uv guardada no reservoir), entao nao ha mais sorteio aqui:
// o Pass A e o Pass B miram o MESMO ponto por construcao, sem precisar do seed determinístico que
// existia antes. Isso tambem elimina a dupla aplicacao de visibilidade que aquele truque evitava.
bool DI_SelectedOccluded(float3 lightPoint, float3 x1, float3 n1, float3 L, float camDist) {
    const float3 origin = OffsetRayGBuffer(x1, n1, L, camDist);
    const float3 toLight = lightPoint - origin;
    const float len = length(toLight);
    const float tMax = len - max(Params.w, 0.0f);
    if (len <= 1e-6f || tMax <= RayEpsB.x) return false;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = toLight / len;
    ray.TMin = RayEpsB.x;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, (uint)Params.z, ray);
    SMILE_RT_PROCEED(query)
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

void StoreInvalid(int2 px) {
    CurrResW[px] = 0.0f;
    CurrResB[px] = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 upx = dtid.xy;
    if (upx.x >= (uint)ScreenParams.x || upx.y >= (uint)ScreenParams.y) return;
    const int2 px = int2(upx);

    const float deviceZ = Depth.Load(int3(px, 0));
    const uint lightCount = (uint)Params.x;
    // Sem luz analitica E sem triangulo emissivo nao ha o que amostrar.
    if (deviceZ <= 0.0f || (lightCount + (uint)TemporalPolicy.w) == 0u) { StoreInvalid(px); return; }

    const GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                        GBufferB.Load(int3(px, 0)),
                                        GBufferC.Load(int3(px, 0)));
    const float2 uv = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 x1 = wh.xyz / wh.w;
    const float3 n1 = g.WorldNormal;

    ReSTIRDIReservoir r;
    DIResInit(r);
    r.X1 = x1;
    r.N1Oct = DDGI_OctEncode(n1);
    float age = 0.0f;

    uint proposalRng = GGX_SeedE(upx, (uint)Params.y, SMILE_RNG_DI_INITIAL);
    uint wrsRng = GGX_SeedE(upx, (uint)Params.y, SMILE_RNG_DI_TEMPORAL);
    const uint analyticBudget = max((uint)Sampling.x, 1u);
    const uint triCount   = (uint)TemporalPolicy.w;
    const uint totalCount = lightCount + triCount;

    // ORCAMENTO POR POOL, e nao um sorteio uniforme no pool combinado. Com 2 analiticas e 26.498
    // triangulos, uniforme dava ~0,06% de chance de uma das 8 candidatas cair numa analitica: o
    // poste da rua sumia. Cada pool tem o SEU orcamento, que e a estrutura do RTXDI
    // (numLocalLightSamples / numInfiniteLightSamples / numEnvironmentSamples separados).
    //
    // Os dois vinham do mesmo Sampling.x ate a Fase 0 do plano de mesh lights. Separa-los nao muda
    // a matematica abaixo — os pesos ja eram escritos em termos de analyticCands/meshCands/
    // totalCands, entao a mistura continua correta com orcamentos diferentes — e e o que permite
    // varrer 8/4/2/1 candidatas de mesh sem arrastar as analiticas junto.
    //
    // Zero em meshCands e uma configuracao PEDIDA, e por isso nao ha max(1) aqui: o operador pode
    // querer o pool no reservoir (o temporal ainda carrega um triangulo escolhido antes) sem gastar
    // proposta nova. O analitico mantem o piso de 1 porque zero la so acontece por ausencia de luz,
    // e esse caso ja e coberto pelo gate de lightCount.
    const uint analyticCands = (lightCount > 0u) ? analyticBudget : 0u;
    const uint meshCands     = (triCount   > 0u) ? (uint)MeshSampling.x : 0u;
    const uint totalCands    = max(analyticCands + meshCands, 1u);

    // O fator M/M_pool NAO e opcional. Amostrar de duas fontes e amostrar de uma MISTURA: a pdf
    // efetiva de uma amostra do pool A e (M_a/M)*p_A, porque os pools sao disjuntos. Usar so p_A
    // deixaria o estimador enviesado por um fator M_a/M em cada pool.
    [loop]
    for (uint i = 0u; i < analyticCands; ++i) {
        const uint idx = min((uint)(DI_RandNext(proposalRng) * lightCount), lightCount - 1u);
        const float2 uv = float2(DI_RandNext(proposalRng), DI_RandNext(proposalRng));
        const DILightSample ls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                   idx, uv, x1);
        float3 diff, spec, L; float dist;
        const float target = DI_TargetFromSample(ls, g, x1, CameraPos.xyz, diff, spec, L, dist);
        const float w = (ls.SolidAnglePdf > 0.0f)
                      ? (target * (float)lightCount * (float)totalCands
                         / ((float)analyticCands * ls.SolidAnglePdf)) : 0.0f;
        DIResUpdate(r, idx, uv, w, wrsRng);
    }

    // Dentro do pool de triangulos a escolha e por POTENCIA, via alias table (O(1): um sorteio de
    // entrada e uma decisao contra o alias dela). Uniforme daria 1/26498 por triangulo, e a
    // disparidade de area x radiancia entre eles e de ordens de grandeza — e o caso em que o
    // achado do CP2077 sobre potencia nao ajudar NAO se aplica, porque la eram luzes analiticas
    // de potencia parecida.
    [loop]
    for (uint j = 0u; j < meshCands; ++j) {
        float triProb;
        const uint tri = MeshLight_SampleAlias(MeshAlias, triCount,
                                               DI_RandNext(proposalRng), DI_RandNext(proposalRng),
                                               triProb);
        const uint idx = lightCount + tri;
        const float2 uv = float2(DI_RandNext(proposalRng), DI_RandNext(proposalRng));
        const DILightSample ls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                   idx, uv, x1);
        float3 diff, spec, L; float dist;
        const float target = DI_TargetFromSample(ls, g, x1, CameraPos.xyz, diff, spec, L, dist);
        // p = (M_t/M) * p_alias(tri) * pdf. O p_alias ja vem normalizado da tabela.
        const float w = (ls.SolidAnglePdf > 0.0f && triProb > 0.0f)
                      ? (target * (float)totalCands
                         / ((float)meshCands * triProb * ls.SolidAnglePdf)) : 0.0f;
        DIResUpdate(r, idx, uv, w, wrsRng);
    }

    const float camDist = length(CameraPos.xyz - x1);

    // Descarta a AMOSTRA ocluida mas PRESERVA o M: o M conta as candidatas SORTEADAS, e todas
    // foram de fato sorteadas. Zerar o M junto inflaria o brilho no Finalize, que divide por ele.
    // Equivale ao `reservoirs[q].W = 0` do Alg. 5, que tambem preserva o M para o Alg. 4.
    if (TemporalPolicy.z > 0.5f && r.LightIndex < totalCount) {
        // Triangulo emissivo sempre projeta sombra: nao ha flag de artista nele, e a geometria que
        // emite e a mesma que oclui. A flag CastShadows so faz sentido para a luz analitica.
        const bool isTri = DI_IsTriangleIndex(r.LightIndex, lightCount);
        if (isTri || DI_IsShadowCaster(Lights[r.LightIndex])) {
            const DILightSample ls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                       r.LightIndex, r.UV, x1);
            if (ls.Valid) {
                const float3 L = normalize(ls.Position - x1);
                if (DI_SelectedOccluded(ls.Position, x1, n1, L, camDist)) {
                    r.LightIndex = 0xFFFFFFFFu;
                    r.WeightSum = 0.0f;
                }
            }
        }
    }

    if (Reuse.x > 0.5f) {
        const float2 prevUv = uv - Velocity.Load(int3(px, 0));
        if (all(prevUv > 0.0f) && all(prevUv < 1.0f)) {
            int2 ppx = int2(prevUv * ScreenParams.xy);

            // A bijecao 4x4 quebra a correlacao espacial que o DLSS Ray Reconstruction revela.
            // No NRD/Nenhum ela fica desligada: o teste em movimento nao mostrou ganho, e a
            // referencia do RTXDI alerta que a permutacao pode piorar geometria fina. Se a
            // permutacao sair da tela, caia na reprojecao original em vez de perder o historico.
            if (TemporalPolicy.x > 0.5f) {
                uint rnd = GGX_PCG((uint)Params.y);
                int2 offset = int2(rnd & 3u, (rnd >> 2u) & 3u);
                int2 perm = ppx + offset;
                perm.x ^= 3; perm.y ^= 3;
                perm -= offset;
                if (all(perm >= int2(0, 0)) && all(perm < int2(ScreenParams.xy)))
                    ppx = perm;
            }

            const uint4 pb = PrevResB.Load(int3(ppx, 0));
            ReSTIRDIReservoir prev = DI_LoadReservoir(PrevResW.Load(int3(ppx, 0)), pb);
            float prevM, prevAge;
            DI_UnpackMAge(pb.z, prevM, prevAge);
            prev.M = min(prevM, Sampling.y);

            // O X1 anterior vem do historico de superficie, nao do reservoir (ver
            // ReSTIRDICommon.hlsli). So e lido depois dos testes que nao dependem dele: um
            // reservoir sem amostra ou sem M nunca toca a textura de superficie, que pode estar
            // velha de um frame em que o passe de motion confiavel nao rodou.
            const float3 prevN = DDGI_OctDecode(prev.N1Oct);
            const float posReject = Reuse.y * max(camDist, 1.0f);
            bool valid = prev.LightIndex < totalCount && prev.M > 0.0f && prev.W > 0.0f &&
                         dot(prevN, n1) >= Reuse.z;
            if (valid) {
                prev.X1 = PrevSurface.Load(int3(ppx, 0)).xyz;
                valid = length(prev.X1 - x1) < posReject &&
                        abs(dot(n1, prev.X1 - x1)) < 0.2f * posReject;
            }

            if (valid && Reuse.w > 0.0f) {
                const float stagger = 0.75f + 0.5f *
                    float(GGX_PCG(upx.x * 7919u + upx.y) & 0xFFu) / 255.0f;
                valid = prevAge < Reuse.w * stagger;
            }

            if (valid) {
                // Reamostra com o MESMO uv, mas no dominio DESTE pixel: para triangulo isso
                // reconstroi o ponto exato; para esfera, o equivalente no cone local.
                const DILightSample pls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                            prev.LightIndex, prev.UV, x1);
                float3 diff, spec, L; float dist;
                const float target = DI_TargetFromSample(pls, g, x1, CameraPos.xyz,
                                                         diff, spec, L, dist);
                if (DIResMerge(r, prev, target, wrsRng)) age = prevAge + 1.0f;
            }
        }
    }

    float selectedTarget = 0.0f;
    if (r.LightIndex < totalCount) {
        const DILightSample sls = DI_SampleAnyLight(Lights, lightCount, TriLights, triCount,
                                                    r.LightIndex, r.UV, x1);
        float3 diff, spec, L; float dist;
        selectedTarget = DI_TargetFromSample(sls, g, x1, CameraPos.xyz, diff, spec, L, dist);
    }
    DIResFinalize(r, selectedTarget);

    float outW; uint4 outB;
    DI_StoreReservoir(r, age, outW, outB);
    CurrResW[px] = outW;
    CurrResB[px] = outB;
}
