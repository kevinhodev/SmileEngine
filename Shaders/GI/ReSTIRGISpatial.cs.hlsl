// ReSTIR GI Pass B: reuso espacial, correção MIS e resolve.
// Não realimenta o reservoir temporal. Referência: Ouyang et al. 2021.
// Debug 1 pinta em vermelho conexões mortas pela visibilidade.
#define RESTIR_DEBUG_VIS_KILLS 0

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
    float4 TraceParams;             // x=frameIndex, y=maxRayDist, z=skyIntensity, w=shadowRayBias
                                    // (nao usado aqui; visibility usa OffsetRayGBuffer)
    float4 ShadeParams;
    float4 ReuseParams;             // x=MCap, y=posRejectScale, z=visibility(0/1), w=temporal(0/1)
    float4 SpatialParams;           // x=radius(px), y=count, z=spatial(0/1), w=normalReject
    float4 JitterParams;            // xy = prevJitterUv - currJitterUv (so o Pass A usa; layout comum)
    row_major float4x4 View;        // nao usado aqui; declarado p/ os offsets do CB baterem
    float4 RayEpsA;                 // x=originFloorMin, y=originFloorPerMeter, z=angularMax,
                                    // w=shadowRayBiasMin  (perfil de epsilons — knobs do editor)
    float4 RayEpsB;                 // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin,
                                    // w=angularMinRatio
    float4 PolicyParams;            // x/y/z so o Pass A usa (backface, boiling, vies temporal);
                                    // w = kill de backface no Jacobiano, usado pelos DOIS
    // Placeholders preservam o layout compartilhado do cbuffer.
    float4 GIDistParams;
    float4 GIBiasParams;
    float4 ReGIRGridMinSlots;
    float4 ReGIRInvCellEnabled;
    float4 ReGIRGridCountSamples;
    float4 ReGIRResources;
    float4 SkyParams;
    float4 DebugParams;
    float4 HistoryParams;
    float4 RadianceCacheCamCell;
    float4 RadianceCacheLodCapFlags;
    float4 RadianceCacheResources;
    float4 GICascadeParams;
    float4 GICascadeGridMinSpacing[4];
    float4 GICascadeScrollOffset[4];
    float4 TraceScreenParams;
    float4 ResolutionParams;
};

#include "../RayOffset.hlsli" // depois do cbuffer: le RayEpsA/RayEpsB

// Garante TMax > TMin nos raios de visibilidade.
float VisRayMinLength() {
    const float kGuard = 0.01f;
    return RayEpsB.y + RayEpsB.z + kGuard;
}

RaytracingAccelerationStructure Scene  : register(t0);
// t3/t4 são fillers do layout de descriptors.
Texture2D<float4>               Res0   : register(t1); // x2.xyz, W
Texture2D<uint4>                Res1   : register(t2); // n2 | Lo | M+idade | n1
Texture2D<float4>               GBuffer : register(t5);
Texture2D<float>                Depth   : register(t6);
StructuredBuffer<InstanceGeo>   Instances : register(t7);

RWTexture2D<float4>             GIOut   : register(u0); // rgb=gi, a=hitDist (preservado p/ o NRD)

SamplerState LinearClamp : register(s0);
SamplerState LinearWrap  : register(s1);

#include "RTAlphaTest.hlsli"
#include "ReSTIRReservoir.hlsli"

// Reconstrói x1 do depth em vez de armazená-lo no reservoir.
float3 WorldFromDepth(int2 p, float deviceZ) {
    const float2 uv  = (float2(p) + 0.5f) * ScreenParams.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 wh  = mul(float4(ndc, deviceZ, 1.0f), InvViewProj);
    return wh.xyz / wh.w;
}

uint2 FullPixelFromTrace(uint2 tracePx) {
    const uint scale = max((uint)ResolutionParams.x, 1u);
    return min(tracePx * scale + uint2(ResolutionParams.yz), uint2(ScreenParams.xy) - 1u);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 px = dtid.xy;
    if (px.x >= (uint)TraceScreenParams.x || px.y >= (uint)TraceScreenParams.y)
        return;

    const uint2 surfacePx = FullPixelFromTrace(px);

    float deviceZ = Depth.Load(int3(surfacePx, 0)).r;
    if (deviceZ <= 0.0f) { GIOut[px] = float4(0.0f, 0.0f, 0.0f, 0.0f); return; }

    float hitDist = GIOut[px].a; // hitDist do Pass A — so fallback qdo o WRS espacial fica vazio

    float4 gb = GBuffer.Load(int3(surfacePx, 0));
    float3 n1 = DDGI_OctDecode(gb.rg * 2.0f - 1.0f);

    float4 s0 = Res0.Load(int3(px, 0));
    uint4  s1 = Res1.Load(int3(px, 0));
    float3 x1 = WorldFromDepth(surfacePx, deviceZ);

    uint rng = RngSeed(surfacePx, (uint)TraceParams.x, SMILE_RNG_SPATIAL_WRS);

    // Domínios participantes da correção MIS.
    const int kMaxCand = 9; // 1 self + K (default 4; folga p/ slider futuro)
    float3 candX1[kMaxCand];
    float3 candN1[kMaxCand];
    float  candM [kMaxCand];
    int    candCount = 0;
    int    selCand   = -1;

    // Começa pela amostra do próprio pixel.
    Reservoir rs; ResInit(rs); rs.x1 = x1;
    {
        Reservoir self;
        float selfM, selfAge; // Res1.z = M + idade empacotados (so o temporal usa a idade)
        ResUnpackMAge(s1.z, selfM, selfAge);
        self.x1 = x1;                    self.x2 = s0.xyz;
        self.n2 = ResUnpackNormal(s1.x); self.Lo = ResUnpackRadiance(s1.y);
        self.M = selfM; self.W = s0.w; self.wSum = 0.0f;
        float pHatSelf = TargetPHat(x1, n1, self.x2, self.Lo);
        if (ResMerge(rs, self, pHatSelf, 1.0f, rng)) selCand = 0;
        candX1[0] = x1; candN1[0] = n1; candM[0] = self.M; candCount = 1;
    }

    float camDist   = length(CameraPos.xyz - x1);
    float posReject = ReuseParams.y * max(camDist, 1.0f);
    float radius    = SpatialParams.x / max(ResolutionParams.x, 1.0f);
    int   K         = min((int)SpatialParams.y, kMaxCand - 1);
    float normalRej = SpatialParams.w;

    for (int i = 0; i < K; ++i) {
        float2 E   = GGX_Rand2E(px, (uint)TraceParams.x, SMILE_RNG_SPATIAL_TAP + (uint)i);
        float2 off = GGX_ConcentricDisk(E) * radius;
        int2   qpx = int2(px) + int2(round(off));
        if (qpx.x < 0 || qpx.y < 0 || qpx.x >= (int)TraceScreenParams.x ||
            qpx.y >= (int)TraceScreenParams.y)
            continue;
        if (all(qpx == int2(px))) continue;

        const uint2 qSurfacePx = FullPixelFromTrace(uint2(qpx));
        float qz = Depth.Load(int3(qSurfacePx, 0)).r;
        if (qz <= 0.0f) continue;
        float4 qgb = GBuffer.Load(int3(qSurfacePx, 0));
        float3 qn1 = DDGI_OctDecode(qgb.rg * 2.0f - 1.0f);
        if (dot(qn1, n1) < normalRej) continue; // rejeicao por normal

        uint4 q1 = Res1.Load(int3(qpx, 0));
        float qM, qAge;
        ResUnpackMAge(q1.z, qM, qAge);
        if (qM <= 0.0f) continue;
        float3 qx1 = WorldFromDepth(qSurfacePx, qz);
        if (length(qx1 - x1) > posReject) continue; // rejeicao radial por posicao

        float4 q0 = Res0.Load(int3(qpx, 0));

        Reservoir nb;
        nb.x1 = qx1;                     nb.x2 = q0.xyz;
        nb.n2 = ResUnpackNormal(q1.x);   nb.Lo = ResUnpackRadiance(q1.y);
        nb.M = qM; nb.W = q0.w; nb.wSum = 0.0f;

        float J = ReconnectionJacobian(x1, nb.x1, nb.x2, nb.n2, PolicyParams.w > 0.5f);
        // Clamp manteria a amostra com peso incorreto.
        if (J < 0.1f || J > 10.0f) continue;
        float pHat = TargetPHat(x1, n1, nb.x2, nb.Lo);
        if (ResMerge(rs, nb, pHat, J, rng)) selCand = candCount;
        candX1[candCount] = nb.x1; candN1[candCount] = qn1; candM[candCount] = nb.M;
        ++candCount;
    }

    // Balance heuristic: W = wSum*pi/(pHatSel*sum(ps_c*M_c)).
    {
        float pHatSel = TargetPHat(x1, n1, rs.x2, rs.Lo);
        float pi = 0.0f, piSum = 0.0f;
        for (int cd = 0; cd < candCount; ++cd) {
            float ps = TargetPHat(candX1[cd], candN1[cd], rs.x2, rs.Lo);
            // O domínio próprio é testado pela shading visibility abaixo.
            if (ps > 0.0f && cd > 0 && ReuseParams.z > 0.5f) {
                float3 mDir = SafeRayDir(rs.x2 - candX1[cd], candN1[cd]); // p/ o termo angular
                float3 morg = OffsetRayGBuffer(candX1[cd], candN1[cd], mDir,
                                               length(CameraPos.xyz - candX1[cd]));
                float3 mToS = rs.x2 - morg;
                float  mLen = length(mToS);
                if (mLen > VisRayMinLength()) { // mesmas folgas do shading visibility
                    RayDesc mray;
                    mray.Origin    = morg;
                    mray.Direction = mToS / mLen;
                    mray.TMin      = RayEpsB.y;
                    mray.TMax      = mLen - RayEpsB.z;
                    // Teste de oclusão sem culling.
                    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> mq;
                    mq.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                                      SMILE_RT_MASK_GATHER, mray);
                    SMILE_RT_PROCEED(mq)
                    if (mq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ps = 0.0f;
                }
            }
            piSum += ps * candM[cd];
            if (cd == selCand) pi = ps;
        }
        ResFinalizeMIS(rs, pHatSel, pi, piSum);
    }

    // Visibilidade opcional da conexão selecionada.
    if (ReuseParams.z > 0.5f && rs.W > 0.0f) {
        float3 sDir = SafeRayDir(rs.x2 - x1, n1); // aprox. p/ o termo angular do offset
        float3 org  = OffsetRayGBuffer(x1, n1, sDir, camDist);
        float3 toS  = rs.x2 - org;
        float  len  = length(toS);
        if (len > VisRayMinLength()) {
            RayDesc vray;
            vray.Origin    = org;
            vray.Direction = toS / len;
            vray.TMin      = RayEpsB.y;
            vray.TMax      = len - RayEpsB.z; // > TMin garantido; para antes do x2
            const uint VisFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> vq;
            vq.TraceRayInline(Scene, VisFlags, SMILE_RT_MASK_GATHER, vray);
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
    // O NRD recebe a distância da amostra vencedora.
    float selDist = (rs.wSum > 0.0f) ? length(rs.x2 - x1) : hitDist;
    GIOut[px] = float4(gi, selDist);
}
