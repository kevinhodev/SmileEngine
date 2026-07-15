// ReSTIR GI — Pass B: reuso ESPACIAL + resolve. Le o reservoir do Pass A (pos-temporal), funde k
// vizinhos com o Jacobiano de reconexao (Ouyang 2021), rejeicao por normal/posicao, e resolve a
// irradiancia -> GITexture. NAO realimenta o reservoir temporal (evita acumulo de bias).
// Correcao de bias (M6 completo, estilo RTXDI): balance heuristic pi/piSum com os valores
// CONTINUOS do pHat do vencedor no dominio de cada participante (nao mais o Z binario de
// contagem); com visibility ON, vizinho ocluido do vencedor sai do denominador (a pdf de origem
// dele e 0 — a amostragem inicial so gera pontos mutuamente visiveis).
// Visibility ray opcional (x1->x2) com alpha-test (Instances/Vertices/Indices bindados).

// Debug: pinta de VERMELHO (10,0,0) os pixels cuja conexao selecionada foi morta pelo visibility
// ray, em vez de zerar W. Ligar = 1 + rebuild do target Shaders + reabrir o editor; testar com
// "ReSTIR visibility" ON e NRD REBLUR OFF (o denoiser borraria os pontos). Esperado: pontos
// esparsos re-sorteados por frame em volta de oclusores finos (postes/cadeiras/quinas).
#define RESTIR_DEBUG_VIS_KILLS 0

#include "DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"
#include "../RayOffset.hlsli"

cbuffer ReSTIRCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;            // x=W, y=H, z=1/W, w=1/H
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;             // x=frameIndex, y=maxRayDist, z=skyIntensity, w=shadowRayBias
                                    // (nao usado aqui; visibility usa OffsetRayGBuffer)
    float4 ShadeParams;
    float4 ReuseParams;             // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    float4 SpatialParams;           // x=radius(px), y=count, z=spatial(0/1), w=normalReject
    float4 JitterParams;            // xy = prevJitterUv - currJitterUv (so o Pass A usa; layout comum)
};

RaytracingAccelerationStructure Scene  : register(t0);
Texture2D<float4>               ResA   : register(t1); // x1.xyz, M
Texture2D<float4>               ResB   : register(t2); // x2.xyz, W
Texture2D<float4>               ResC   : register(t3); // Lo.rgb  (a = n1.oct.x, so o temporal usa)
Texture2D<float4>               ResD   : register(t4); // n2.xyz  (a = n1.oct.y, so o temporal usa)
Texture2D<float4>               GBuffer : register(t5);
Texture2D<float>                Depth   : register(t6);
// Alpha-test dos visibility rays (M6): sem eles o pass usava CULL_NON_OPAQUE (folhagem nao
// ocluia e paredes atras de candidato masked eram perdidas). t8/t9 aposentados (VB/IB
// bindless via InstanceGeo); a tabela CPU mantem o layout com filler.
StructuredBuffer<InstanceGeo>   Instances : register(t7);

RWTexture2D<float4>             GIOut   : register(u0); // rgb=gi, a=hitDist (preservado p/ o NRD)

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "RTAlphaTest.hlsli"
#include "ReSTIRReservoir.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) { GIOut[px] = float4(0.0f, 0.0f, 0.0f, 0.0f); return; }

    float hitDist = GIOut[px].a; // hitDist do Pass A — so fallback qdo o WRS espacial fica vazio

    float4 gb = GBuffer.Load(int3(px, 0));
    float3 n1 = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float4 a = ResA.Load(int3(px, 0));
    float4 b = ResB.Load(int3(px, 0));
    float4 c = ResC.Load(int3(px, 0));
    float4 dd = ResD.Load(int3(px, 0));
    float3 x1 = a.xyz;

    uint rng = RngSeed(px, (uint)TraceParams.x ^ 0x9E3779B9u);

    // Participantes do WRS (self + ate K vizinhos aceitos), guardados p/ a correcao de bias:
    // precisamos re-avaliar o sample VENCEDOR no dominio de cada um depois da selecao. selCand
    // rastreia o dominio que GEROU o vencedor (numerador pi do peso MIS).
    const int kMaxCand = 9; // 1 self + K (default 4; folga p/ slider futuro)
    float3 candX1[kMaxCand];
    float3 candN1[kMaxCand];
    float  candM [kMaxCand];
    int    candCount = 0;
    int    selCand   = -1;

    // Reservoir espacial: comeca com a propria amostra do pixel.
    Reservoir rs; ResInit(rs); rs.x1 = x1;
    {
        Reservoir self;
        float selfM, selfAge; // ResA.w = M + idade empacotados (so o temporal usa a idade)
        ResUnpackMAge(a.w, selfM, selfAge);
        self.x1 = a.xyz; self.x2 = b.xyz; self.n2 = dd.xyz; self.Lo = c.rgb;
        self.M = selfM; self.W = b.w; self.wSum = 0.0f;
        float pHatSelf = TargetPHat(x1, n1, self.x2, self.Lo);
        if (ResMerge(rs, self, pHatSelf, 1.0f, rng)) selCand = 0;
        candX1[0] = x1; candN1[0] = n1; candM[0] = self.M; candCount = 1;
    }

    float camDist   = length(CameraPos.xyz - x1);
    float posReject = ReuseParams.y * max(camDist, 1.0f);
    float radius    = SpatialParams.x;
    int   K         = min((int)SpatialParams.y, kMaxCand - 1);
    float normalRej = SpatialParams.w;

    for (int i = 0; i < K; ++i) {
        float2 E   = GGX_Rand2(px, ((uint)TraceParams.x * 7u + (uint)i * 131u));
        float2 off = GGX_ConcentricDisk(E) * radius;
        int2   qpx = int2(px) + int2(round(off));
        if (qpx.x < 0 || qpx.y < 0 || qpx.x >= (int)ScreenParams.x || qpx.y >= (int)ScreenParams.y)
            continue;
        if (all(qpx == int2(px))) continue;

        float qz = Depth.Load(int3(qpx, 0)).r;
        if (qz <= 0.0f) continue;
        float4 qgb = GBuffer.Load(int3(qpx, 0));
        float3 qn1 = DDGI_OctDecode(qgb.rg * 2.0f - 1.0f);
        if (dot(qn1, n1) < normalRej) continue; // rejeicao por normal

        float4 qa = ResA.Load(int3(qpx, 0));
        float qM, qAge;
        ResUnpackMAge(qa.w, qM, qAge);
        if (qM <= 0.0f) continue;
        // So rejeicao RADIAL (config estavel do bisect 2026-07-12; a rejeicao de plano do fix 6
        // saiu junto com o resto do pacote — re-introduzir so com A/B dedicado).
        if (length(qa.xyz - x1) > posReject) continue; // rejeicao radial por posicao

        float4 qb = ResB.Load(int3(qpx, 0));
        float4 qc = ResC.Load(int3(qpx, 0));
        float4 qdd = ResD.Load(int3(qpx, 0));

        Reservoir nb;
        nb.x1 = qa.xyz; nb.x2 = qb.xyz; nb.n2 = qdd.xyz; nb.Lo = qc.rgb;
        nb.M = qM; nb.W = qb.w; nb.wSum = 0.0f;

        float J = ReconnectionJacobian(x1, nb.x1, nb.x2, nb.n2); // dst=atual, src=vizinho
        // Rejeita (nao clampa) Jacobiano extremo: clampar mantem o sample com peso errado
        // (firefly/escurecimento em quinas). RTXDI/kajiya descartam o vizinho nesse caso.
        if (J < 0.1f || J > 10.0f) continue;
        float pHat = TargetPHat(x1, n1, nb.x2, nb.Lo);
        if (ResMerge(rs, nb, pHat, J, rng)) selCand = candCount;
        candX1[candCount] = nb.x1; candN1[candCount] = qn1; candM[candCount] = nb.M;
        ++candCount;
    }

    // Correcao de bias (M6 completo, estilo RTXDI): balance heuristic no lugar do Z binario.
    // ps_c = pHat do sample VENCEDOR avaliado no dominio de cada participante (valor continuo);
    // W = wSum * pi / (pHatSel * Σ ps_c·M_c), com pi = ps do dominio que gerou o vencedor.
    // Vizinho que "mal" poderia gerar o sample (ps pequeno) vota pouco no denominador — menos
    // variancia que o teste binario de hemisferio. Com visibility ON, vizinho OCLUIDO do
    // vencedor tem ps zerado por ray: a pdf de origem real dele e 0 (a amostragem inicial so
    // gera pontos mutuamente visiveis) — remove o escurecimento residual em bordas de oclusao.
    {
        float pHatSel = TargetPHat(x1, n1, rs.x2, rs.Lo);
        float pi = 0.0f, piSum = 0.0f;
        for (int cd = 0; cd < candCount; ++cd) {
            float ps = TargetPHat(candX1[cd], candN1[cd], rs.x2, rs.Lo);
            // Raio so p/ vizinhos (cd>0): o dominio do proprio pixel e coberto pela shading
            // visibility abaixo (mesmo segmento; nao paga o raio 2x).
            if (ps > 0.0f && cd > 0 && ReuseParams.z > 0.5f) {
                float3 morg = OffsetRayGBuffer(candX1[cd], candN1[cd],
                                               length(CameraPos.xyz - candX1[cd]));
                float3 mToS = rs.x2 - morg;
                float  mLen = length(mToS);
                if (mLen > 0.15f) { // mesmas folgas do shading visibility
                    RayDesc mray;
                    mray.Origin    = morg;
                    mray.Direction = mToS / mLen;
                    mray.TMin      = 0.02f;
                    mray.TMax      = mLen - 0.05f;
                    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                             RAY_FLAG_CULL_BACK_FACING_TRIANGLES> mq;
                    mq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                                      RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, mray);
                    SMILE_RT_PROCEED(mq)
                    if (mq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ps = 0.0f;
                }
            }
            piSum += ps * candM[cd];
            if (cd == selCand) pi = ps;
        }
        rs.W = (pHatSel > 0.0f && pi > 0.0f && piSum > 0.0f)
             ? (rs.wSum * pi / (piSum * pHatSel)) : 0.0f;
    }

    // Shading visibility (opcional): testa a conexao x1->x2 da amostra SELECIONADA. Se ocluida, o
    // indireto deste pixel cai -> o NRD borra esse 0/1 estocastico num gradiente = sombra de contato
    // SUAVE (estilo AO do bounce). Igual a shading visibility do RTXDI (DI). Sem reuso do resultado.
    if (ReuseParams.z > 0.5f && rs.W > 0.0f) {
        // Direcao e comprimento medidos da origem efetiva (offset robusto anti self-hit). O
        // x2 fica protegido pela folga do TMax (para 0.05 antes da superficie dele); a origem
        // pelo TMin + offset.
        float3 org = OffsetRayGBuffer(x1, n1, camDist);
        float3 toS = rs.x2 - org;
        float  len = length(toS);
        // Pula conexoes curtas: garante TMax > TMin (senao = UB no DXR) e elas sao triviais.
        if (len > 0.15f) {
            RayDesc vray;
            vray.Origin    = org;
            vray.Direction = toS / len;
            vray.TMin      = 0.02f;
            vray.TMax      = len - 0.05f; // > TMin garantido (len > 0.15); para antes do x2
            // MESMAS flags do trace inicial (CULL_BACK) — senao backfaces viram oclusores
            // fantasmas que a amostra original nunca viu. Alpha-test via SMILE_RT_PROCEED
            // (Instances/Vertices/Indices bindados no M6): folhagem masked oclui correto e
            // paredes atras de candidato masked nao sao mais perdidas (o CULL_NON_OPAQUE
            // antigo ignorava os dois casos).
            const uint VisFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                                  RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                     RAY_FLAG_CULL_BACK_FACING_TRIANGLES> vq;
            vq.TraceRayInline(Scene, VisFlags, 0xFF, vray);
            SMILE_RT_PROCEED(vq)
            if (vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
#if RESTIR_DEBUG_VIS_KILLS
                GIOut[px] = float4(10.0f, 0.0f, 0.0f, hitDist); // kill -> vermelho puro
                return;
#else
                rs.W = 0.0f;
#endif
            }
        }
    }

    float3 gi = ResResolve(rs, x1, n1, ShadeParams.z);
    // hitDist do NRD segue a amostra VENCEDORA (um vizinho pode ter trocado x2); ver Pass A.
    float selDist = (rs.wSum > 0.0f) ? length(rs.x2 - x1) : hitDist;
    GIOut[px] = float4(gi, selDist);
}
