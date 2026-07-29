// DI-lite — iluminacao direta das luzes locais que ficaram SEM slot de shadow map.
//
// O PROBLEMA: o FLocalShadows tem orcamento fixo (8 spots + 4 points ativos). A luz que nao ganha
// slice continua iluminando, so que SEM oclusao nenhuma — vaza parede. Ate agora a unica saida era
// aumentar o orcamento, que custa VRAM e um passe de rasterizacao por luz.
//
// A SAIDA: um raio de sombra por PIXEL, nao por luz. Em vez de sombrear todas as excedentes, o
// passe escolhe UMA por pixel via Weighted Reservoir Sampling e traca um raio so p/ ela; o
// estimador WRS devolve, sem vies, a contribuicao do conjunto inteiro. E o degrau natural para o
// ReSTIR DI: falta so reuso temporal e espacial por cima deste reservoir de uma amostra.
//
// ESCOPO deliberado (ver a discussao que fechou o desenho):
//   - SO luzes locais SEM slot. As com slot seguem no loop do deferred, com shadow map — melhor
//     sinal (deterministico) pelo custo que ja esta pago.
//   - SO a superficie primaria. O sol nao entra: com uma direcional dominante a escolha do RIS ja
//     esta feita, e ele tem caminho dedicado (CSM hoje; RT + SIGMA no futuro). Estratificacao por
//     dominio disjunto — sol e locais nao se sobrepoem, entao nao ha peso de MIS a calcular.
//   - NAO toca o HitShading.hlsli dos bounces indiretos.
//
// O sinal que sai daqui e RUIDOSO (1 raio/pixel de visibilidade). Sob Ray Reconstruction isso e
// exatamente o que a rede espera receber. Sob NRD ele NAO passa por denoiser nenhum hoje — o NRD
// processa GI e reflexao, nao este alvo. O conserto certo e o reuso temporal do proximo milestone
// (ou um denoiser de mascara de sombra, estilo SIGMA, que e o que a CDPR usa p/ sombra estocastica).
// Ate lá, a chave existe e o default e OFF.

#include "../GBuffer.hlsli"
#include "../BRDF.hlsli"
#include "../LightsCommon.hlsli"
#include "../Reflections/GGXSample.hlsli"

// Espelha o FGPULight do Renderer.h — o MESMO buffer que o deferred le em t17. De proposito: o
// campo que diz "esta luz tem slice de sombra" (SpotParams.y) vive nele, e nao no FGPULightGI
// compacto do mundo indireto. Uma fonte de verdade para "quem tem slot".
struct FGPULightFull {
    float4 PosInvRadius;
    float4 ColorSourceRadius;
    float4 DirCosOuter;
    float4 SpotParams;        // y = slice de sombra (-1 = SEM slot -> e nosso)
    row_major float4x4 ShadowMatrix; // nao usado aqui; existe p/ o stride casar com o deferred
};

cbuffer DILiteCB : register(b0) {
    row_major float4x4 InvViewProj; // FULL (jittered) — reconstroi worldPos do depth
    float4 CameraPos;               // xyz = camera em mundo
    float4 ScreenParams;            // x = W, y = H, zw = 1/W, 1/H
    float4 Params;                  // x = nº de luzes, y = frameIndex, z = mascara do shadow ray,
                                    // w = margem final do raio (m)
    float4 RayEpsA;                 // contrato do RayOffset.hlsli
    float4 RayEpsB;
};

#include "../RayOffset.hlsli" // DEPOIS do cbuffer: le RayEpsA/RayEpsB

Texture2D<float4> GBufferA : register(t0);
Texture2D<float4> GBufferB : register(t1);
Texture2D<float4> GBufferC : register(t2);
Texture2D<float>  Depth    : register(t3);
StructuredBuffer<FGPULightFull> Lights : register(t4);
RaytracingAccelerationStructure Scene  : register(t5);

RWTexture2D<float4> OutDirect : register(u0);

// RngNext local: o ReSTIRReservoir.hlsli tem o mesmo, mas puxa a struct de reservoir junto e aqui
// nao ha reservoir de estado — a selecao e por stream, num registro.
float DIRandNext(inout uint s) { s = GGX_PCG(s); return (s & 0x00FFFFFFu) / 16777216.0f; }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint W = (uint)ScreenParams.x, H = (uint)ScreenParams.y;
    if (dtid.x >= W || dtid.y >= H) return;
    const int2 px = int2(dtid.xy);

    const float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) { OutDirect[px] = 0.0f; return; } // ceu

    GBufferData g = DecodeGBuffer(GBufferA.Load(int3(px, 0)),
                                  GBufferB.Load(int3(px, 0)),
                                  GBufferC.Load(int3(px, 0)));

    const float2 uv  = (float2(px) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wH  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    const float3 worldPos = wH.xyz / wH.w;
    const float  camDist  = length(CameraPos.xyz - worldPos);

    const float3 N = g.WorldNormal;
    const float3 V = normalize(CameraPos.xyz - worldPos);

    // Derivacoes IDENTICAS as do deferred (DeferredLighting.ps.hlsl). Replicadas, e nao
    // compartilhadas, porque sao as mesmas 3 linhas que ja aparecem no ForwardBlend e no
    // MaterialPreview — o padrao da casa. Se divergirem, a luz excedente muda de cor.
    const float3 BaseColor    = g.BaseColor;
    const float3 DiffuseColor = BaseColor * (1.0f - g.Metallic);
    const float3 SpecularColor = lerp(float3(0.04f, 0.04f, 0.04f), BaseColor, g.Metallic);
    const float3 TransColor   = BaseColor * g.Subsurface;
    const float  Roughness    = max(g.Roughness, 0.04f);
    const float  a2           = Roughness * Roughness * Roughness * Roughness;

    uint rng = GGX_SeedE((uint2)px, (uint)Params.y, SMILE_RNG_DI_LITE);

    // ---- WRS sobre as luzes SEM slot -----------------------------------------------------------
    // Todas as elegiveis entram no stream (uma candidata cada), em vez de sortear uma proposta
    // uniforme: o reservoir e feito p/ stream, o loop custa o mesmo que o do deferred (que ja
    // percorre a lista inteira) e o resultado domina qualquer proposta. A licao do GPU Zen 3
    // (p. 198) — ponderar a PROPOSTA por potencia/distancia nao supera uniforme — nao se aplica
    // aqui: nao ha proposta a ponderar quando o stream ve todo mundo.
    float  wSum    = 0.0f;
    float  selW    = 0.0f;
    float3 selLit  = 0.0f;
    float3 selL    = 0.0f;
    float  selDist = 0.0f;
    bool   found   = false;

    const uint NumLights = (uint)Params.x;
    [loop]
    for (uint li = 0; li < NumLights; ++li) {
        FGPULightFull Lp = Lights[li];
        if (Lp.SpotParams.y >= 0.0f) continue; // tem slice: o deferred sombreia com shadow map

        FPunctualLight lt;
        lt.PosInvRadius      = Lp.PosInvRadius;
        lt.ColorSourceRadius = Lp.ColorSourceRadius;
        lt.DirCosOuter       = Lp.DirCosOuter;
        lt.SpotParams        = Lp.SpotParams;

        float3 L; float dist;
        const float3 incoming = PunctualLightIncoming(lt, worldPos, L, dist);
        if (all(incoming <= 0.0f)) continue; // janela de raio / cone ja zeraram

        float3 Ls;
        const float SpecEnergy = AreaSphereSpecular(Lp.ColorSourceRadius.w, Roughness,
                                                   L * dist, V, N, Ls);
        const float3 lit = BRDF_DirectArea(N, V, L, Ls, SpecEnergy, incoming,
                                           DiffuseColor, SpecularColor, Roughness, a2, TransColor);

        // pHat = luminancia da contribuicao NAO ocluida. Boa alvo-funcao: ja carrega BRDF,
        // atenuacao e cone; falta so a visibilidade, que e o que o raio vai medir.
        const float w = dot(lit, float3(0.2126f, 0.7152f, 0.0722f));
        if (w <= 0.0f) continue;

        wSum += w;
        if (DIRandNext(rng) * wSum < w) {
            selW = w; selLit = lit; selL = L; selDist = dist; found = true;
        }
    }

    if (!found) { OutDirect[px] = 0.0f; return; }

    // Estimador RIS/WRS: f(x)/pHat(x) * (1/M) * soma(w_i). Com proposta uniforme sobre as M
    // elegiveis, w_i = pHat_i * M, entao (1/M)*soma(w_i) = soma(pHat_i) = wSum, e f/pHat colapsa
    // em selLit/selW. Nao enviesado.
    float3 estimate = selLit * (wSum / max(selW, 1e-6f));

    // ---- Um raio de sombra, p/ o vencedor ------------------------------------------------------
    const float3 origin = OffsetRayGBuffer(worldPos, N, selL, camDist);
    const float  tMax   = selDist - max(Params.w, 0.0f);
    if (tMax > RayEpsB.x) {
        RayDesc r;
        r.Origin    = origin;
        r.Direction = selL;
        r.TMin      = RayEpsB.x;
        r.TMax      = tMax;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, (uint)Params.z, r);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) estimate = 0.0f;
    }
    // tMax <= TMin: luz a poucos centimetros da superficie. O trecho nao testado e menor que os
    // proprios epsilons, entao contar como VISIVEL e o lado certo (mesma politica do HitShading).

    OutDirect[px] = float4(estimate, 1.0f);
}
