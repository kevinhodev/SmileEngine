#include "../GBuffer.hlsli"
#include "RainCommon.hlsli"

// Chuva deferred — F1 (porte do DeferredRainGBufferPS da CryEngine, mascaras procedurais no
// lugar das texturas de asset). Le as COPIAS de GBufferA/B (scratch) e reescreve os originais:
//   - albedo escurecido por porosidade (Lagarde: poroso molhado escurece ate ~0.2x)
//   - roughness baixada (filme d'agua alisa a microsuperficie)
//   - pocas em superficie up-facing: normal achatada p/ +Y + aneis de gota analiticos,
//     roughness ~0.06 (quase espelho — as reflexoes RT leem o G-buffer e molham de graca)
// Pixels sem chuva efetiva dao discard: o RT preserva o conteudo original do geometry pass.

// cbuffer + SceneDepth/RainOccMap + hashes + SkyVisibility vem do RainCommon.hlsli
Texture2D SceneA : register(t0); // copia de GBufferA (BaseColor + AO)
Texture2D SceneB : register(t1); // copia de GBufferB (OctNormal + Rough + Metal)

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSOut {
    float4 A : SV_Target0;
    float4 B : SV_Target1;
};

float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = p - i;
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Mascara de pocas: fbm curto em XZ do mundo — baixa frequencia decide ONDE empoca,
// a media (0.5) e deslocada pelo PuddleAmount na logica principal.
float PuddleMask(float2 xz) {
    return ValueNoise(xz) * 0.65f + ValueNoise(xz * 2.17f + 19.19f) * 0.35f;
}

// Um layer de aneis de gota: grade de celulas de ~0.35 m, cada celula tem uma gota com centro
// e fase proprios (hash). O anel expande e amortece dentro da vida da gota; devolve o
// GRADIENTE da altura em XZ (vira perturbacao de normal). Analitico — sem flipbook.
float2 RippleLayer(float2 p, float t) {
    float2 cell = floor(p);
    float2 f    = p - cell;
    float2 rnd  = Hash22(cell);
    float2 center = 0.30f + 0.40f * rnd;             // centro da gota dentro da celula
    float  life   = frac(t + rnd.x * 7.31f);         // fase propria: gotas dessincronizadas

    float2 toP = f - center;
    float  d   = length(toP);
    float  r   = life * 0.55f;                       // raio do anel cresce ate ~meia celula

    // envelope: anel gaussiano em torno de r, amplitude decai com a vida
    float x    = (d - r) * 14.0f;
    float ring = exp(-x * x) * (1.0f - life) * (1.0f - life);

    // gradiente radial: derivada do gaussiano (+ fallback no centro exato da gota)
    float2 dir = toP / max(d, 1e-4f);
    return dir * (ring * -2.0f * x * 14.0f) * 0.06f;
}

// 2 layers em escalas/offsets diferentes quebram a repeticao da grade.
float2 RippleGradient(float2 xz, float t) {
    float2 g = RippleLayer(xz * 2.85f, t);
    g += RippleLayer(xz * 4.07f + 13.7f, t * 1.31f + 0.5f) * 0.75f;
    return g;
}

// F2b: drenagem — poça só onde a vizinhança top-down é PLANA. Reusa o occlusion map como
// heightmap: vizinho ~0.7 m mais baixo = beirada/pedestal (topo de poste, barril, mureta,
// borda de telhado) e a agua escoa em vez de empocar; vizinhos na mesma altura = chao de
// verdade. Fisica de escoamento implicita, sem simulacao.
float DrainageMask(float3 worldPos) {
    if (RainOccParams.x < 0.5f) return 1.0f;

    float3 uvz = mul(float4(worldPos, 1.0f), RainOccMatrix).xyz;
    if (any(uvz.xy != saturate(uvz.xy))) return 1.0f;

    const float size   = RainOccParams.w;
    const float rangeM = 1.0f / RainParams1.y;      // metros por unidade de depth do ortho
    const int2  c      = int2(uvz.xy * size);
    const int2  offs[4] = { int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3) }; // ~0.7 m

    float drain = 0.0f;
    [unroll] for (int i = 0; i < 4; ++i) {
        int2  s  = clamp(c + offs[i], int2(0, 0), int2(size - 1.0f, size - 1.0f));
        float nz = RainOccMap.Load(int3(s, 0));
        // vizinho mais fundo que ~0.3 m = queda; 0.8 m = borda franca
        drain += smoothstep(0.3f, 0.8f, (nz - uvz.z) * rangeM);
    }
    return saturate(1.0f - drain * 0.6f); // 2+ lados em queda = seca por completo
}

PSOut main(VSOutput input) {
    int2  px       = int2(input.pos.xy);
    float rawDepth = SceneDepth.Load(int3(px, 0));
    if (rawDepth <= 0.0f) discard; // ceu (reverse-Z)

    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, rawDepth, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;

    // F2: oclusao escala a chuva inteira — interior/marquise ficam secos (discard preserva
    // o G-buffer original do geometry pass).
    const float rain = RainParams0.x * SkyVisibility(worldPos);
    if (rain <= 0.0005f) discard;

    float4 gA = SceneA.Load(int3(px, 0));
    float4 gB = SceneB.Load(int3(px, 0));

    float3 albedo = gA.rgb;
    float  ao     = gA.a;
    float3 N      = GBuffer_OctDecode(gB.rg);
    float  rough  = gB.b;
    float  metal  = gB.a;

    // Acumulo por orientacao: chao junta agua, parede drena (fica so umida). (Cry restringe a
    // camada deferred a up-facing; o leve wet de parede aqui e o termo do Lagarde.)
    const float isUp  = saturate(N.y * 5.0f - 4.0f);
    const float accum = rain * lerp(0.30f, 1.0f, isUp);

    // Porosidade derivada da roughness (Cry/Lagarde): material rugoso = poroso = absorve agua
    // -> escurece; polido nao muda de cor, so de gloss.
    const float porosity = saturate((rough - 0.5f) / 0.3f);
    albedo *= lerp(1.0f, lerp(1.0f, 0.2f, porosity * RainParams0.w), accum);

    // Filme d'agua alisa: poroso molhado ~0.40, liso molhado ~0.15 (gloss 0.60/0.85 da Cry).
    rough = lerp(rough, lerp(0.15f, 0.40f, porosity), accum);

    // ---- Pocas (so chao PLANO que retem agua) ----
    // Gate mais apertado que o isUp do wetness: superficie curva (barril, topo de poste)
    // drena; + mascara de drenagem top-down (F2b) mata poça em pedestal/beirada.
    const float flatUp      = smoothstep(0.82f, 0.95f, N.y);
    const float mask        = PuddleMask(worldPos.xz * RainParams1.x);
    const float puddleBlend = RainParams0.y * rain * flatUp * DrainageMask(worldPos);
    // limiar do noise: mais PuddleAmount*rain = pocas maiores; borda suave de 0.12
    const float thresh = lerp(0.78f, 0.42f, saturate(RainParams0.y * rain));
    float puddle = smoothstep(thresh, thresh + 0.12f, mask) * (puddleBlend > 0.001f ? 1.0f : 0.0f);
    puddle *= saturate(puddleBlend * 4.0f);

    if (puddle > 0.001f) {
        // ripples: fade com a distancia (alem de ~60 m o gradiente vira ruido sob TAA/FSR2)
        const float distFade = saturate(1.0f - length(worldPos - CameraWorldPos.xyz) / 60.0f);
        float2 grad = RippleGradient(worldPos.xz, CameraWorldPos.w * 0.9f)
                    * RainParams0.z * rain * distFade;
        float3 puddleN = normalize(float3(-grad.x, 1.0f, -grad.y));

        N     = normalize(lerp(N, puddleN, puddle));
        rough = lerp(rough, 0.06f, puddle);   // agua parada ~espelho (gloss 0.93 da Cry)
        ao    = lerp(ao, 1.0f, puddle * 0.5f); // agua nao cavita o AO do material seco
    }

    PSOut o;
    o.A = float4(albedo, ao);
    o.B = float4(GBuffer_OctEncode(N), rough, metal);
    return o;
}
