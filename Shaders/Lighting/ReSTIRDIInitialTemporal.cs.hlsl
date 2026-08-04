// ReSTIR DI - Pass A: candidatas iniciais uniformes por pixel + reuso temporal.
// Escreve o reservoir que vira historico. O espacial nao realimenta este buffer, evitando acumular
// o vies do filtro espacial no historico temporal.

#include "../GBuffer.hlsli"
#include "../BRDF.hlsli"
#include "../LightsCommon.hlsli"
#include "../Reflections/GGXSample.hlsli"
#include "../GI/DDGICommon.hlsli"
#include "ReSTIRDICommon.hlsli"

cbuffer ReSTIRDICB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams; // W, H, 1/W, 1/H
    float4 Params;       // x=lightCount, y=frameIndex, z=shadowMask, w=rayEndMargin
    float4 Sampling;     // x=initialCandidates, y=MCap, z=spatialCount, w=spatialRadius
    float4 Reuse;        // x=temporal(0/1), y=posRejectScale, z=normalReject, w=maxAge
    float4 TemporalPolicy; // x=permutation temporal (0/1), z=visibilidade inicial (0/1); yw reservados
    float4 RayEpsA;      // layout compartilhado com o Pass B
    float4 RayEpsB;
    row_major float4x4 View; // layout comum; consumido pelo pack do NRD
};

#include "../RayOffset.hlsli"

Texture2D<float4> GBufferA : register(t0);
Texture2D<float4> GBufferB : register(t1);
Texture2D<float4> GBufferC : register(t2);
Texture2D<float>  Depth    : register(t3);
Texture2D<float2> Velocity : register(t4);
Texture2D<float4> PrevResA : register(t5); // xyz=x1, w=W
Texture2D<float4> PrevResB : register(t6); // x=light, y=M+age, zw=n1 oct
StructuredBuffer<FGPULightFull> Lights : register(t7);
RaytracingAccelerationStructure Scene : register(t8);
StructuredBuffer<InstanceGeo> Instances : register(t9);

SamplerState LinearWrap : register(s1);

RWTexture2D<float4> CurrResA : register(u0);
RWTexture2D<float4> CurrResB : register(u1);

#include "../GI/RTAlphaTest.hlsli"

// Alg. 5, passo 2 do paper (visibility reuse). A target PDF ignora visibilidade de proposito
// (Secao 5), entao uma luz forte porem OCLUIDA tem pHat enorme e domina o WRS. Sem este teste ela
// trava no reservoir, o raio final do Pass B devolve 0 e o pixel emite ZERO — enquanto a luz fraca
// porem VISIVEL que deveria iluminar aquela sombra quase nunca ganha o sorteio. Nao e vazamento de
// luz (o Pass B sempre testa oclusao no destino): e fome de energia na penumbra. Testar aqui, antes
// do reuso temporal, impede que a amostra ocluida contamine o historico.
bool DI_SelectedOccluded(FGPULightFull light, float3 x1, float3 n1, float3 L, float camDist,
                         inout uint rng) {
    const float3 origin = OffsetRayGBuffer(x1, n1, L, camDist);
    // Estocastico igual ao resolve do Pass B: manter o centro aqui descartaria a luz inteira
    // sempre que o CENTRO estivesse ocluido, mesmo com meia esfera visivel — perda de energia
    // justo na penumbra, que e onde este passo mais importa.
    const float3 toLight = DI_SampleLightPoint(light, L, rng) - origin;
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
    CurrResA[px] = 0.0f;
    CurrResB[px] = float4(-1.0f, 0.0f, 0.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint2 upx = dtid.xy;
    if (upx.x >= (uint)ScreenParams.x || upx.y >= (uint)ScreenParams.y) return;
    const int2 px = int2(upx);

    const float deviceZ = Depth.Load(int3(px, 0));
    const uint lightCount = (uint)Params.x;
    if (deviceZ <= 0.0f || lightCount == 0u) { StoreInvalid(px); return; }

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
    const uint candidateCount = max((uint)Sampling.x, 1u);
    [loop]
    for (uint i = 0u; i < candidateCount; ++i) {
        const uint lightIndex = min((uint)(DI_RandNext(proposalRng) * lightCount), lightCount - 1u);
        float3 diff, spec, L; float dist;
        const float target = DI_Evaluate(Lights[lightIndex], g, x1, CameraPos.xyz,
                                         diff, spec, L, dist);
        // Proposta uniforme p=1/N. A target so entra no segundo estagio, como medido no GPU Zen 3.
        DIResUpdate(r, lightIndex, target * (float)lightCount, wrsRng);
    }

    const float camDist = length(CameraPos.xyz - x1);

    // Descarta a AMOSTRA ocluida mas PRESERVA o M: o M conta as candidatas SORTEADAS, e todas
    // foram de fato sorteadas. Zerar o M junto inflaria o brilho no Finalize, que divide por ele.
    // Equivale ao `reservoirs[q].W = 0` do Alg. 5, que tambem preserva o M para o Alg. 4.
    if (TemporalPolicy.z > 0.5f && r.LightIndex < lightCount) {
        const FGPULightFull sel = Lights[r.LightIndex];
        if (DI_IsShadowCaster(sel)) {
            float3 diff, spec, L; float dist;
            DI_Evaluate(sel, g, x1, CameraPos.xyz, diff, spec, L, dist);
            uint visRng = DI_LightPointSeed(upx, (uint)Params.y, r.LightIndex);
            if (DI_SelectedOccluded(sel, x1, n1, L, camDist, visRng)) {
                r.LightIndex = 0xFFFFFFFFu;
                r.WeightSum = 0.0f;
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

            const float4 pa = PrevResA.Load(int3(ppx, 0));
            const float4 pb = PrevResB.Load(int3(ppx, 0));
            ReSTIRDIReservoir prev = DI_LoadReservoir(pa, pb);
            float prevM, prevAge;
            DI_UnpackMAge(pb.y, prevM, prevAge);
            prev.M = min(prevM, Sampling.y);

            const float3 prevN = DDGI_OctDecode(prev.N1Oct);
            const float posReject = Reuse.y * max(camDist, 1.0f);
            bool valid = prev.LightIndex < lightCount && prev.M > 0.0f && prev.W > 0.0f &&
                         dot(prevN, n1) >= Reuse.z &&
                         length(prev.X1 - x1) < posReject &&
                         abs(dot(n1, prev.X1 - x1)) < 0.2f * posReject;

            if (valid && Reuse.w > 0.0f) {
                const float stagger = 0.75f + 0.5f *
                    float(GGX_PCG(upx.x * 7919u + upx.y) & 0xFFu) / 255.0f;
                valid = prevAge < Reuse.w * stagger;
            }

            if (valid) {
                float3 diff, spec, L; float dist;
                const float target = DI_Evaluate(Lights[prev.LightIndex], g, x1, CameraPos.xyz,
                                                 diff, spec, L, dist);
                if (DIResMerge(r, prev, target, wrsRng)) age = prevAge + 1.0f;
            }
        }
    }

    float selectedTarget = 0.0f;
    if (r.LightIndex < lightCount) {
        float3 diff, spec, L; float dist;
        selectedTarget = DI_Evaluate(Lights[r.LightIndex], g, x1, CameraPos.xyz,
                                     diff, spec, L, dist);
    }
    DIResFinalize(r, selectedTarget);

    float4 outA, outB;
    DI_StoreReservoir(r, age, outA, outB);
    CurrResA[px] = outA;
    CurrResB[px] = outB;
}
