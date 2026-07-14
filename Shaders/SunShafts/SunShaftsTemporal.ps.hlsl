// Acumulação temporal dos sun shafts volumétricos (mesma receita do CloudTemporal,
// validada na engine): o raymarch jittera com IGN por frame e ESTE passe integra o
// ruído ao longo dos frames — history weight ~0.9 equivale a ~10x mais passos de
// graça (UE froxel fog usa o mesmo 0.9). Reprojeção pela distância média ponderada
// que o raymarch escreve no alpha (mídia presa ao mundo: fog + estrutura de sombra
// são estáticos, cobre rotação E translação). Variance clipping 3x3 segura ghosting
// quando o sol/TOD move a estrutura do feixe.

cbuffer SunShaftsTemporalCB : register(b0) {
    float4 ScreenParams;   // xy = dims do RT meia-res, zw = 1/dims
    float4 TemporalParams; // x = peso do frame atual, y = história válida (0/1), zw unused
    row_major float4x4 InvViewProj;   // atual (full), reconstrói a direção do raio
    row_major float4x4 PrevViewProj;  // frame anterior SEM jitter
    float4 CameraWorldPos;
};

Texture2D<float4> RawShafts   : register(t0);
Texture2D<float4> HistoryPrev : register(t1);
SamplerState      LinearClamp : register(s0);

float4 main(float4 svpos : SV_POSITION) : SV_TARGET {
    int2 px   = int2(svpos.xy);
    int2 dims = int2(ScreenParams.xy);

    float4 cur = RawShafts.Load(int3(px, 0));

    // variance clipping 3x3 no rgb (alpha carrega distância, não entra no clamp)
    float3 m1 = 0.0f, m2 = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    [unroll]
    for (int dx = -1; dx <= 1; ++dx) {
        int2 q = clamp(px + int2(dx, dy), int2(0, 0), dims - int2(1, 1));
        float3 s = RawShafts.Load(int3(q, 0)).rgb;
        m1 += s; m2 += s * s;
    }
    float3 mu    = m1 / 9.0f;
    float3 sigma = sqrt(max(m2 / 9.0f - mu * mu, 0.0f));
    float3 lo    = mu - 1.25f * sigma;
    float3 hi    = mu + 1.25f * sigma;

    // ponto representativo do pixel = câmera + dir * distância média do raymarch
    float2 uv  = svpos.xy * ScreenParams.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 pH  = mul(float4(ndc, 0.5f, 1.0f), InvViewProj);
    float3 dir = normalize(pH.xyz / pH.w - CameraWorldPos.xyz);
    float3 wp  = CameraWorldPos.xyz + dir * cur.a;

    float4 prevClip = mul(float4(wp, 1.0f), PrevViewProj);

    float alpha = TemporalParams.x;
    if (TemporalParams.y < 0.5f || prevClip.w <= 1e-4f) alpha = 1.0f;

    float2 prevNdc = prevClip.xy / max(prevClip.w, 1e-4f);
    float2 prevUv  = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f)) alpha = 1.0f;

    // história descartada = nem amostra (RT recém-criado pode conter NaN, e
    // lerp(NaN, cur, 1) propaga o NaN)
    if (alpha >= 1.0f) return cur;

    float3 hist = HistoryPrev.SampleLevel(LinearClamp, saturate(prevUv), 0.0f).rgb;
    hist = clamp(hist, lo, hi);

    return float4(lerp(hist, cur.rgb, alpha), cur.a);
}
