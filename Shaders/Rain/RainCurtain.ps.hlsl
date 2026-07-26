#include "RainCommon.hlsli"

// Chuva F3 — cortina de gotas (porte conceitual do SceneRain da CryEngine). A Cry desenha
// 1-3 cones de geometria presos na camera com texturas de rainfall em 2 velocidades; aqui o
// mesmo efeito sai de um fullscreen pass: 3 CILINDROS conceituais de raios diferentes ao
// redor da camera, streaks procedurais em UV cilindrico (sem asset), parallax real entre as
// camadas. Soft-depth manual (streak atras de parede some), oclusao pelo mapa da F2
// (marquise corta a cortina) e tint da key light com forward scattering (chuva brilha
// contra a luz). Blend premultiplicado por cima do HDR ja iluminado/fogado, antes do TAA —
// o acumulador temporal integra os streaks como um motion blur leve, que e o look desejado.

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// Um layer de streaks: colunas verticais de ~0.14 m no arco do cilindro, cada coluna com
// presenca/fase/offset proprios (hash). "presence" controla quantas colunas tem gota ativa.
float StreakPattern(float u, float v, float presence) {
    float  col = floor(u);
    float  fu  = u - col;
    float2 rnd = Hash22(float2(col, 3.7f));
    if (rnd.x >= presence) return 0.0f;

    // corpo do streak: dash de ~1/3 do periodo com pontas suaves, fase propria por coluna
    float dash = frac(v + rnd.y * 23.7f);
    float body = smoothstep(0.00f, 0.10f, dash) * smoothstep(0.42f, 0.25f, dash);

    // perfil transversal fino num centro aleatorio da coluna
    float cx   = 0.25f + 0.5f * Hash21(float2(col, 9.1f));
    float prof = 1.0f - saturate(abs(fu - cx) * 5.0f);
    return body * prof * prof;
}

float4 main(VSOutput input) : SV_Target {
    // F4: CurtainParams.x ja vem pre-multiplicado pelo RainAmount INSTANTANEO no CPU —
    // RainParams0.x agora e o molhado acumulado (Wetness) e nao serve pra cortina.
    const float rain = CurtainParams.x;
    if (rain <= 0.001f) discard;

    int2 px = int2(input.pos.xy);

    // raio de visao (unproject em profundidade arbitraria — direcao independe dela)
    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float4 pH  = mul(float4(ndc, 0.5f, 1.0f), InvViewProj);
    float3 dir = normalize(pH.xyz / pH.w - CameraWorldPos.xyz);

    // distancia ate a cena (reverse-Z: 0 = ceu)
    float rawDepth  = SceneDepth.Load(int3(px, 0));
    float sceneDist = 1e5f;
    if (rawDepth > 0.0f) {
        float4 wH = mul(float4(ndc, rawDepth, 1.0f), InvViewProj);
        sceneDist = length(wH.xyz / wH.w - CameraWorldPos.xyz);
    }

    const float time  = CameraWorldPos.w;
    const float speed = CurtainParams.y;

    // forward scattering: gota e uma lente — brilha olhando CONTRA a luz (sol/lua baixos
    // atras da chuva = cortina prateada; de costas pra luz sobra o ambient do ceu).
    // O tint da key light e DESSATURADO: a gota refrata o ceu inteiro, nao so o disco do
    // sol — com a cor crua, crepusculo virava streak ambar tipo faisca (bug do A/B).
    const float  fwd    = pow(saturate(dot(dir, KeyLightDir.xyz) * 0.5f + 0.5f), 4.0f);
    const float3 keyRaw = KeyLightColor.rgb;
    const float  keyLum = dot(keyRaw, float3(0.2126f, 0.7152f, 0.0722f));
    const float3 keyCol = lerp(keyLum.xxx, keyRaw, 0.35f);
    const float3 tint   = keyCol * lerp(0.03f, 0.11f, fwd)
                        + SkyAmbientRain.rgb * 0.10f;

    // 4 cilindros presos na camera: perto = streak grande e rapido, longe = cortina densa.
    // O de 28 m e a chuva DISTANTE (rua do outro lado de um arco/tunel curto — A/B da F3).
    const float radii [4] = { 2.2f, 5.5f, 12.0f, 28.0f };
    const float scaleV[4] = { 0.45f, 0.30f, 0.22f, 0.15f }; // 1/altura da celula vertical (m)
    const float alphaL[4] = { 0.35f, 0.30f, 0.25f, 0.20f };
    const float colsM [4] = { 7.0f, 7.0f, 7.0f, 3.5f };     // colunas/m (longe = mais grosso,
                                                            // senao vira shimmer subpixel)
    float3 acc  = 0.0f;
    float  accA = 0.0f;

    // singularidade do zenite: olhando reto p/ cima o UV cilindrico degenera num starburst
    // radial (bug do A/B) — fade suave; horiz > 0.2 (ate ~78 graus de elevacao) intacto
    const float horizRaw = length(dir.xz);
    const float upFade   = smoothstep(0.08f, 0.20f, horizRaw);
    if (upFade <= 0.001f) discard;

    // Exposicao ao longo do raio (raymarch de chuva distante estilo Cry, 8 taps entre 12 m
    // e min(cena, 55 m)): fracao do caminho EXPOSTA a chuva. Alimenta o veu e — o ponto do
    // A/B do tunel — serve de fallback do cilindro de 28 m: de dentro de um tunel comprido
    // a parede do cilindro ainda esta coberta, mas o raio atravessa chuva depois da boca.
    float farExpo = 0.0f;
    const float veilStart = 12.0f;
    const float veilEnd   = min(sceneDist, 55.0f);
    if (veilEnd > veilStart + 1.0f) {
        [unroll] for (int k = 0; k < 8; ++k) {
            float tk = lerp(veilStart, veilEnd, (k + 0.5f) / 8.0f);
            farExpo += SkyVisibility(CameraWorldPos.xyz + dir * tk);
        }
        farExpo *= 0.125f;
    }

    [unroll] for (int i = 0; i < 4; ++i) {
        // F5: com particulas ON o near-field e delas — a cortina mantem so os cilindros
        // de 12/28 m (medio/longe), senao gota-quad e streak se sobrepoem em dobro
        if (CurtainParams.z > 0.5f && i < 2) continue;
        const float r     = radii[i];
        const float horiz = max(horizRaw, 0.12f);   // clamp p/ olhar reto cima/baixo
        const float t     = r / horiz;               // distancia 3D REAL ate o streak

        // soft-depth contra a distancia REAL do streak (t), nao o raio do cilindro:
        // olhando p/ cima t >> r e o streak fica ATRAS do toldo/arco — comparar com r
        // deixava chover "dentro" de area coberta (bug do A/B)
        const float gfade = saturate((sceneDist - t) * 0.8f);
        if (gfade <= 0.0f) continue;

        const float3 wall = CameraWorldPos.xyz + dir * t;

        // F2: marquise/interior cortam a cortina — banda apertada (0.25 m), a de 1.5 m do
        // wetness deixava vazar 30-60% debaixo de cobertura baixa (bug do A/B).
        // O cilindro de 28 m representa TODA a chuva de 12 m ao infinito: a visibilidade
        // dele e "existe trecho exposto no raio?" (max com o raymarch) — senao, com a boca
        // do tunel alem de 28 m, a saida ficava sem chuva (A/B do tunel, rodada 2)
        float occ = SkyVisibilityBand(wall, 6.0f);
        if (i == 3) occ = max(occ, farExpo);
        if (occ <= 0.01f) continue;

        // UV cilindrico com numero INTEIRO de colunas na volta (sem costura atras da camera)
        const float cols  = round(6.28318f * r * colsM[i]);
        const float un    = (atan2(dir.x, dir.z) * 0.159155f + 0.5f) * cols;
        const float u     = fmod(floor(un), cols) + frac(un);
        const float v     = (wall.y + time * speed) * scaleV[i];

        const float presence = lerp(0.06f, 0.50f, rain);
        float s = StreakPattern(u, v, presence);
        s += StreakPattern(u * 1.53f + 11.0f, v * 1.31f + 3.0f, presence * 0.8f) * 0.6f;
        s *= gfade * occ * upFade;
        if (s <= 0.001f) continue;

        const float a = s * alphaL[i];
        acc  += tint * a * (1.0f - accA);
        accA += a * (1.0f - accA);
    }

    // ---- Veu de chuva distante (haze sem streak, atras dos cilindros) ----
    if (farExpo > 0.001f) {
        // peso pelo comprimento do trecho chuvoso na frente do pixel
        float veil = farExpo * saturate((veilEnd - veilStart) / 30.0f) * rain * 0.18f * upFade;
        acc  += tint * veil * (1.0f - accA);
        accA += veil * (1.0f - accA);
    }

    if (accA <= 0.0015f) discard;
    return float4(acc, accA * 0.6f); // premultiplicado; a chuva vela pouco o fundo
}
