// ReSTIR GI — Pass B: reuso ESPACIAL + resolve (BIASED). Le o reservoir do Pass A (pos-temporal),
// funde k vizinhos com o Jacobiano de reconexao (Ouyang 2021), rejeicao por normal/posicao, e
// resolve a irradiancia -> GITexture. NAO realimenta o reservoir temporal (evita acumulo de bias).
// Sem MIS/correcao de bias (vem no M6). Visibility ray opcional (x1->x2).

#include "DDGICommon.hlsli"
#include "../Reflections/GGXSample.hlsli"

cbuffer ReSTIRCB : register(b0) {
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 ScreenParams;            // x=W, y=H, z=1/W, w=1/H
    float4 GridMinSpacing;
    float4 GridCount;
    float4 AtlasParams;
    float4 SunDirIntensity;
    float4 SunColor;
    float4 TraceParams;             // x=frameIndex, y=maxRayDist, z=skyIntensity, w=normalBias
    float4 ShadeParams;
    float4 ReuseParams;             // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    float4 SpatialParams;           // x=radius(px), y=count, z=spatial(0/1), w=normalReject
};

RaytracingAccelerationStructure Scene  : register(t0);
Texture2D<float4>               ResA   : register(t1); // x1.xyz, M
Texture2D<float4>               ResB   : register(t2); // x2.xyz, W
Texture2D<float4>               ResC   : register(t3); // Lo.rgb
Texture2D<float4>               ResD   : register(t4); // n2.xyz
Texture2D<float4>               GBuffer : register(t5);
Texture2D<float>                Depth   : register(t6);

RWTexture2D<float4>             GIOut   : register(u0); // rgb=gi, a=hitDist (preservado p/ o NRD)

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "ReSTIRReservoir.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)ScreenParams.x || px.y >= (uint)ScreenParams.y)
        return;

    float deviceZ = Depth.Load(int3(px, 0)).r;
    if (deviceZ <= 0.0f) { GIOut[px] = float4(0.0f, 0.0f, 0.0f, 0.0f); return; }

    float hitDist = GIOut[px].a; // preserva o hitDist escrito pelo Pass A (p/ o NRD na Fase C)

    float4 gb = GBuffer.Load(int3(px, 0));
    float3 n1 = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float4 a = ResA.Load(int3(px, 0));
    float4 b = ResB.Load(int3(px, 0));
    float4 c = ResC.Load(int3(px, 0));
    float4 dd = ResD.Load(int3(px, 0));
    float3 x1 = a.xyz;

    uint rng = RngSeed(px, (uint)TraceParams.x ^ 0x9E3779B9u);

    // Reservoir espacial: comeca com a propria amostra do pixel.
    Reservoir rs; ResInit(rs); rs.x1 = x1;
    {
        Reservoir self;
        self.x1 = a.xyz; self.x2 = b.xyz; self.n2 = dd.xyz; self.Lo = c.rgb;
        self.M = a.w; self.W = b.w; self.wSum = 0.0f;
        float pHatSelf = TargetPHat(x1, n1, self.x2, self.Lo);
        ResMerge(rs, self, pHatSelf, 1.0f, rng);
    }

    float camDist   = length(CameraPos.xyz - x1);
    float posReject = ReuseParams.y * max(camDist, 1.0f);
    float radius    = SpatialParams.x;
    int   K         = (int)SpatialParams.y;
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
        if (qa.w <= 0.0f) continue;
        if (length(qa.xyz - x1) > posReject) continue; // rejeicao por posicao

        float4 qb = ResB.Load(int3(qpx, 0));
        float4 qc = ResC.Load(int3(qpx, 0));
        float4 qdd = ResD.Load(int3(qpx, 0));

        Reservoir nb;
        nb.x1 = qa.xyz; nb.x2 = qb.xyz; nb.n2 = qdd.xyz; nb.Lo = qc.rgb;
        nb.M = qa.w; nb.W = qb.w; nb.wSum = 0.0f;

        float J = ReconnectionJacobian(x1, nb.x1, nb.x2, nb.n2); // dst=atual, src=vizinho
        // Rejeita (nao clampa) Jacobiano extremo: clampar mantem o sample com peso errado
        // (firefly/escurecimento em quinas). RTXDI/kajiya descartam o vizinho nesse caso.
        if (J < 0.1f || J > 10.0f) continue;
        float pHat = TargetPHat(x1, n1, nb.x2, nb.Lo);
        ResMerge(rs, nb, pHat, J, rng);
    }

    ResFinalizeW(rs, x1, n1);

    // Shading visibility (opcional): testa a conexao x1->x2 da amostra SELECIONADA. Se ocluida, o
    // indireto deste pixel cai -> o NRD borra esse 0/1 estocastico num gradiente = sombra de contato
    // SUAVE (estilo AO do bounce). Igual a shading visibility do RTXDI (DI). Sem reuso do resultado.
    if (ReuseParams.z > 0.5f && rs.W > 0.0f) {
        float3 toS = rs.x2 - x1;
        float  len = length(toS);
        // Pula conexoes curtas: garante TMax > TMin (senao = UB no DXR) e elas sao triviais.
        if (len > 0.15f) {
            RayDesc vray;
            vray.Origin    = x1 + n1 * max(TraceParams.w, 1e-3f);
            vray.Direction = toS / len;
            vray.TMin      = 0.02f;
            vray.TMax      = len - 0.05f; // > TMin garantido (len > 0.15)
            // MESMAS flags do trace inicial (CULL_BACK) — senao backfaces viram oclusores fantasmas
            // que a amostra original nunca viu, causando escurecimento espurio.
            const uint VisFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> vq;
            vq.TraceRayInline(Scene, VisFlags, 0xFF, vray);
            vq.Proceed();
            if (vq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) rs.W = 0.0f;
        }
    }

    float3 gi = ResResolve(rs, x1, n1, ShadeParams.z);
    GIOut[px] = float4(gi, hitDist);
}
