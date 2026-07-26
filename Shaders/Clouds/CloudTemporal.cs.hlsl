// Acumulacao temporal das nuvens (estilo UE VolumetricRenderTarget / Cry ReprojectClouds).
// Reprojecao COM parallax: o ponto representativo do pixel na casca media das nuvens
// (aproximacao de camada fina) e reprojetado com a VP do frame anterior em unidades de
// mundo (sem jitter) — cobre rotacao E translacao da camera. Variance clipping 3x3
// segura ghosting quando o vento/TOD evolui a nuvem (mesma receita do TAA F2 da engine).
cbuffer CloudCB : register(b0) {
    row_major float4x4 InvViewProjNoTrans;
    float4 CameraPos;        // xyz = camera no frame do planeta (km)
    float4 SunDir;
    float4 SunColor;
    float4 PlanetRadii;
    float4 CloudParams;
    float4 CloudParams2;
    float4 WindParams;
    float4 MarchParams;
    float4 ScreenParams;
    float4 PhaseParams;
    float4 AtmoLink;
    row_major float4x4 PrevVPWorld;
    float4 TemporalParams;   // x = blend alpha, y = historia valida (0/1)
    float4 SkyAmbientCol;
    float4 GroundAmbientCol;
    row_major float4x4 InvViewProjWorld;
    float4 CamWorld;         // xyz = camera em unidades de mundo, w = km/unidade
    float4 CloudMisc;
};

Texture2D<float4>   RawClouds   : register(t0);
Texture2D<float4>   HistoryPrev : register(t1);
RWTexture2D<float4> HistoryOut  : register(u0);

SamplerState LinearClampSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)ScreenParams.x;
    uint H = (uint)ScreenParams.y;
    if (id.x >= W || id.y >= H) return;

    float4 cur = RawClouds[id.xy];

    float4 m1 = 0.0f, m2 = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    [unroll]
    for (int dx = -1; dx <= 1; ++dx) {
        int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(W - 1, H - 1));
        float4 s = RawClouds[q];
        m1 += s; m2 += s * s;
    }
    float4 mu    = m1 / 9.0f;
    float4 sigma = sqrt(max(m2 / 9.0f - mu * mu, 0.0f));
    float4 lo    = mu - 1.25f * sigma;
    float4 hi    = mu + 1.25f * sigma;

    float2 uv  = (float2(id.xy) + 0.5f) * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 dir = normalize(mul(float4(ndc, 1.0f, 1.0f), InvViewProjNoTrans).xyz);

    // Ponto representativo: intersecao com a esfera media da camada (km, frame do planeta).
    // Aproximacao de camada fina; se ghostar em voo dentro da nuvem, trocar por um buffer
    // de distancia media de espalhamento escrito pelo raymarch.
    float midR = 0.5f * (PlanetRadii.y + PlanetRadii.z);
    float b    = dot(CameraPos.xyz, dir);
    float c    = dot(CameraPos.xyz, CameraPos.xyz) - midR * midR;
    float disc = b * b - c;
    float distKm = 100.0f;
    if (disc >= 0.0f) {
        float s = sqrt(disc);
        float t0 = -b - s, t1 = -b + s;
        distKm = (t0 > 0.0f) ? t0 : ((t1 > 0.0f) ? t1 : 100.0f);
    }
    float3 worldPt  = CamWorld.xyz + dir * (distKm / max(CamWorld.w, 1e-6f));
    float4 prevClip = mul(float4(worldPt, 1.0f), PrevVPWorld);

    float alpha = TemporalParams.x;
    if (TemporalParams.y < 0.5f || prevClip.w <= 1e-4f) alpha = 1.0f;

    float2 prevNdc = prevClip.xy / max(prevClip.w, 1e-4f);
    float2 prevUv  = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f)) alpha = 1.0f;

    float4 hist = HistoryPrev.SampleLevel(LinearClampSampler, saturate(prevUv), 0.0f);
    hist = clamp(hist, lo, hi);

    HistoryOut[id.xy] = lerp(hist, cur, alpha);
}
