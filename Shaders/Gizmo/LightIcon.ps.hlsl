// Glifo do icone de luz por SDF (sem textura: nitido em qualquer tamanho/zoom).
// Point = lampada (globo + soquete); Spot = refletor (corpo + feixe trapezoidal).
// Preenche na cor da luz com nucleo quente, contorno escuro p/ ler em fundo claro ou
// escuro, e anel branco quando selecionada.
struct VSOutput {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
    float2 uv    : TEXCOORD0;
    float3 misc  : TEXCOORD1; // y = tipo, z = selecionada
};

float sdCircle(float2 p, float r) { return length(p) - r; }

float sdRoundBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
}

// iq: trapezio simetrico — r1 = meia-largura em y=-he, r2 em y=+he.
float sdTrapezoid(float2 p, float r1, float r2, float he) {
    float2 k1 = float2(r2, he);
    float2 k2 = float2(r2 - r1, 2.0f * he);
    p.x = abs(p.x);
    float2 ca = float2(p.x - min(p.x, (p.y < 0.0f) ? r1 : r2), abs(p.y) - he);
    float2 cb = p - k1 + k2 * clamp(dot(k1 - p, k2) / dot(k2, k2), 0.0f, 1.0f);
    float s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
    return s * sqrt(min(dot(ca, ca), dot(cb, cb)));
}

float4 main(VSOutput i) : SV_TARGET {
    float2 uv = i.uv;
    float  aa = max(fwidth(uv.x), 1e-4f) * 1.2f;
    bool   selected = i.misc.z > 0.5f;

    float d;
    if (i.misc.y < 0.5f) {
        // lampada: globo redondo + soquete arredondado embaixo
        float dGlobe = sdCircle(uv - float2(0.0f, 0.22f), 0.42f);
        float dBase  = sdRoundBox(uv - float2(0.0f, -0.44f), float2(0.16f, 0.20f), 0.07f);
        d = min(dGlobe, dBase);
    } else {
        // refletor: corpo em cima + feixe abrindo pra baixo
        float dBody = sdRoundBox(uv - float2(0.0f, 0.50f), float2(0.24f, 0.15f), 0.08f);
        float dBeam = sdTrapezoid(uv - float2(0.0f, -0.14f), 0.46f, 0.13f, 0.44f);
        d = min(dBody, dBeam);
    }

    float alpha = 1.0f - smoothstep(-aa, aa, d);
    if (alpha <= 0.003f && !selected) discard;

    // nucleo quente no centro do glifo + contorno escuro
    float2 coreP = uv - float2(0.0f, 0.18f);
    float  core  = exp(-3.0f * dot(coreP, coreP));
    float3 fill  = lerp(i.color, min(i.color + 0.65f, 1.0f), core);
    float  edge  = smoothstep(-0.16f, -0.16f + aa * 2.0f, d);
    float3 col   = lerp(fill, float3(0.07f, 0.07f, 0.06f), edge * 0.9f);

    if (selected) {
        float ring  = abs(sdCircle(uv, 0.88f)) - 0.05f;
        float ringA = 1.0f - smoothstep(-aa, aa, ring);
        col   = lerp(col, float3(1.0f, 1.0f, 1.0f), ringA);
        alpha = max(alpha, ringA);
    }

    return float4(col, alpha);
}
