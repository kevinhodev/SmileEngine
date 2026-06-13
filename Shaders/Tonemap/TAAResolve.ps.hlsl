// TAAResolve.ps.hlsl — Temporal Anti-Aliasing resolve (Fase 2: variance clip + Catmull-Rom).
//
// Sintese do "melhor de cada" das engines de referencia (D:\Engines):
//   - Estrutura/ClipToAABB    : Flax (TAA.shader; clip de history estilo INSIDE/Pedersen)
//   - Variance clipping YCoCg  : Unreal (TemporalAA.usf) / Salvi — clamp por media +- gamma*sigma
//                                (caixa temporalmente ESTAVEL -> mata a tremedeira de borda que o
//                                 min/max causava: o min/max oscilava com o jitter em cada silhueta)
//   - History em Catmull-Rom   : Unreal/Karis — resample bicubico do history (mata o blur que o
//                                 blend alto + bilinear introduzia)
//   - Blend tonemap-weighted   : Karis "High-Quality Temporal Supersampling" — pesa por 1/(1+luma)
//                                 p/ firefly/probe-noise nao dominar a media (shimmer da DDGI)
//
// Cena Bistro e estatica e sem motion vectors -> reprojecao SO-camera: reconstroi o world-pos do
// depth atual (InvViewProj) e reprojeta no frame anterior (PrevViewProj). Disocclusao por rejeicao
// off-screen + clip de vizinhanca. O jitter (Halton) e removido pela acumulacao temporal.
// Nota: a reconstrucao via InvViewProj ja embute o Reverse-Z; nada de especial aqui.

cbuffer TAAConstants : register(b0) {
    row_major float4x4 InvViewProj;   // atual, FULL (com translacao) — world-pos do depth
    row_major float4x4 PrevViewProj;  // frame anterior, FULL — reprojeta o world-pos
    float4 Params0;                   // xy = 1/screenSize, z = history blend (0..1), w = history valido (0/1)
    float4 Params1;                   // x = variance gamma, y = sharpness, z = debug mode, w = motion blend
    float4 Params2;                   // x = anti-flicker (clamp do atual a caixa), yzw = reservado
};

Texture2D    Input        : register(t0); // cor HDR atual (resolvida, pre-tonemap)
Texture2D    InputHistory : register(t1); // saida do TAA do frame anterior
Texture2D<float> Depth    : register(t2); // depth da cena (R32, Reverse-Z)
SamplerState LinearClamp  : register(s0);

// --- YCoCg <-> RGB (clamp de cromaticidade mais estavel que em RGB puro) ---
float3 RGBToYCoCg(float3 c) {
    return float3(
         0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
         0.5f  * c.r              - 0.5f  * c.b,
        -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}
// Tonemap reversivel (Karis "HDR-TAA"): comprime o brilho p/ que faisca especular nao domine a
// caixa de variancia, o clamp nem o blend. Luma = Y do YCoCg (0.25,0.5,0.25). TODO o resolve roda
// nesse espaco; InvTonemap so no fim. Mata o cintilamento de detalhe sub-pixel brilhante.
float  TaaLuma(float3 c)        { return 0.25f * c.r + 0.5f * c.g + 0.25f * c.b; }
float3 Tonemap(float3 c)        { return c / (1.0f + TaaLuma(c)); }
float3 InvTonemap(float3 c)     { return c / max(1.0f - TaaLuma(c), 1e-4f); }

float3 YCoCgToRGB(float3 c) {
    float t = c.x - c.z; // Y - Cg
    return float3(t + c.y, c.x + c.z, t - c.y);
}

// Clip do history ao AABB [Pedersen 2016, "TAA in INSIDE"]: em vez de clamp duro (corta cor),
// desliza o ponto ate a borda da caixa mantendo a direcao — menos ghosting.
float3 ClipToAABB(float3 color, float3 mn, float3 mx) {
    float3 center  = 0.5f * (mx + mn);
    float3 extents = 0.5f * (mx - mn) + 1e-5f;
    float3 shift   = color - center;
    float3 unit    = abs(shift / extents);
    float  maxUnit = max(unit.x, max(unit.y, unit.z));
    return (maxUnit > 1.0f) ? (center + shift / maxUnit) : color;
}

// Resample bicubico Catmull-Rom de 9 taps (5 fetches bilineares) [MJP/Karis]. Reduz o blur
// que o bilinear introduz quando o history e reamostrado a cada frame num offset sub-pixel.
float3 SampleHistoryCatmullRom(float2 uv, float2 texSize, float2 invTexSize) {
    float2 samplePos = uv * texSize;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    float2 w12     = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 texPos0  = (texPos1 - 1.0f)      * invTexSize;
    float2 texPos3  = (texPos1 + 2.0f)      * invTexSize;
    float2 texPos12 = (texPos1 + offset12)  * invTexSize;

    float3 r = 0.0f;
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos0.x,  texPos0.y),  0).rgb * (w0.x  * w0.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos12.x, texPos0.y),  0).rgb * (w12.x * w0.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos3.x,  texPos0.y),  0).rgb * (w3.x  * w0.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos0.x,  texPos12.y), 0).rgb * (w0.x  * w12.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos12.x, texPos12.y), 0).rgb * (w12.x * w12.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos3.x,  texPos12.y), 0).rgb * (w3.x  * w12.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos0.x,  texPos3.y),  0).rgb * (w0.x  * w3.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos12.x, texPos3.y),  0).rgb * (w12.x * w3.y);
    r += InputHistory.SampleLevel(LinearClamp, float2(texPos3.x,  texPos3.y),  0).rgb * (w3.x  * w3.y);
    return max(r, 0.0f); // o lobo negativo do Catmull-Rom pode dar overshoot; o clip seguinte cuida
}

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOutput input) : SV_TARGET {
    float2 uv  = input.uv;
    float2 inv = Params0.xy;

    // --- Vizinhanca 3x3 do frame atual em YCoCg-TONEMAPPED: media + variancia ---
    // Tudo no espaco tonemapped (brilho comprimido) -> faisca especular de detalhe sub-pixel nao
    // domina a caixa/clamp/blend (= o cintilamento da banca a esquerda).
    float3 m1 = 0.0f; // soma
    float3 m2 = 0.0f; // soma dos quadrados
    float3 centerYCoCg = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float3 c = RGBToYCoCg(Tonemap(Input.SampleLevel(LinearClamp, uv + float2(x, y) * inv, 0).rgb));
            m1 += c;
            m2 += c * c;
            if (x == 0 && y == 0) centerYCoCg = c;
        }
    }

    // Caixa de variancia PURA (Salvi/Karis "An Excursion in Temporal Supersampling"): media +-
    // gamma*sigma, SEM recortar pelo min/max. Critico: numa silhueta (distribuicao bimodal) o sigma
    // e grande -> a caixa fica LARGA e o history (valor medio anti-aliased) cabe dentro todo frame
    // -> a borda para de vibrar. Recortar pelo min/max colapsava a caixa de volta ao min/max na
    // borda (= sem ganho); em regiao chapada o sigma e pequeno -> caixa estreita -> rejeita ghosting.
    float3 mean  = m1 / 9.0f;
    float3 sigma = sqrt(max(m2 / 9.0f - mean * mean, 0.0f));
    float  gamma = Params1.x;
    float3 boxMin = mean - gamma * sigma;
    float3 boxMax = mean + gamma * sigma;

    float3 curRaw = centerYCoCg; // atual sem sharpening (p/ debug)
    // Sharpening (compensa a suavizacao do TAA): empurra o atual p/ longe da media da vizinhanca.
    centerYCoCg += (centerYCoCg - mean) * Params1.y;

    // Anti-cintilamento (firefly espacial): clampa o ATUAL a caixa de variancia da vizinhanca.
    // Um glint especular sub-pixel (1 pixel muito mais brilhante que os vizinhos) fica acima de
    // mean+gamma*sigma -> e puxado p/ a borda da caixa todo frame -> para de piscar na fonte.
    // Em espaco tonemapped, entao so mata o excesso de brilho, nao a cor. Strength 0..1 (slider).
    centerYCoCg = lerp(centerYCoCg, ClipToAABB(centerYCoCg, boxMin, boxMax), Params2.x);

    // --- Reprojecao so-camera: world-pos do depth atual -> UV no frame anterior ---
    float  depth = Depth.SampleLevel(LinearClamp, uv, 0).r;
    float2 ndc   = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wH    = mul(float4(ndc, depth, 1.0f), InvViewProj);
    float3 world = wH.xyz / wH.w;
    float4 pClip = mul(float4(world, 1.0f), PrevViewProj);
    float2 pNdc  = pClip.xy / pClip.w;
    float2 prevUV = float2(pNdc.x * 0.5f + 0.5f, 0.5f - pNdc.y * 0.5f);

    // Validade do history: fora da tela (disocclusao por borda) ou primeiro frame -> descarta.
    float onScreen = (prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
                      prevUV.y >= 0.0f && prevUV.y <= 1.0f) ? 1.0f : 0.0f;

    // Blend adaptativo por velocidade (Flax StationaryBlending/MotionBlending): o offset de
    // reprojecao (sem jitter) = movimento de tela em px/frame. Parado -> blend alto (acumula =
    // AA maximo); em movimento -> blend menor (menos history = menos blur, mais aliasing que o
    // movimento esconde). Por-pixel: o que esta longe (move pouco) continua nitido.
    #define kTAAVelScale 0.15f // ~7 px/frame satura p/ o blend de movimento
    float velPx  = length((prevUV - uv) / inv);
    float motion = saturate(velPx * kTAAVelScale);
    float blend  = lerp(Params0.z, Params1.w, motion); // z = parado, w = movimento
    float histW  = blend * Params0.w * onScreen;

    // History em Catmull-Rom (nitido), TONEMAPPED. Guarda o cru (pre-clamp) p/ os debugs.
    float3 historyRaw     = RGBToYCoCg(Tonemap(SampleHistoryCatmullRom(prevUV, 1.0f / inv, inv)));
    float3 historyClamped = ClipToAABB(historyRaw, boxMin, boxMax);

    // Blend simples no espaco tonemapped: o tonemap ja faz o papel anti-firefly (a faisca esta
    // comprimida), entao o lerp direto basta — e mais estavel que o peso 1/(1+luma) em HDR cru.
    float3 resolvedYCoCg = lerp(centerYCoCg, historyClamped, histW);

    // --- DEBUG VIEWS (Params1.z) — saida crua (passa pelo bloom/tonemap, leitura relativa) ---
    // 1=offset de reprojecao(px)  2=|atual-history|  3=dist do clamp  4=peso do history
    // 5=sigma(largura da caixa)   6=so history       7=so atual(jittered)
    int dbg = (int)(Params1.z + 0.5f);
    if (dbg > 0) {
        float3 d = 0.0f;
        if      (dbg == 1) { float px = length((prevUV - uv) / inv); d = float3(0.0f, saturate(px / 4.0f), saturate(px / 4.0f)); }
        else if (dbg == 2) { d = float3(saturate(abs(curRaw.x - historyRaw.x) * 4.0f), 0.0f, 0.0f); }
        else if (dbg == 3) { d = float3(0.0f, 0.0f, saturate(length(historyClamped - historyRaw) * 4.0f)); }
        else if (dbg == 4) { d = histW.xxx; }
        else if (dbg == 5) { d = saturate(sigma.x * 4.0f).xxx; }
        else if (dbg == 6) { d = max(InvTonemap(YCoCgToRGB(historyRaw)), 0.0f); }
        else if (dbg == 7) { d = max(InvTonemap(YCoCgToRGB(curRaw)), 0.0f); }
        return float4(d, 1.0f);
    }

    // De volta ao HDR linear (InvTonemap) p/ o bloom/tonemap final.
    float3 outColor = max(InvTonemap(YCoCgToRGB(resolvedYCoCg)), 0.0f);
    return float4(outColor, 1.0f);
}
