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
Texture2D SceneB : register(t1); // copia de GBufferB (OctNormal + Rough + ShadingModelID)

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

// Fluxo/poças: gate topológico via occ map como heightmap (top-down). Suprime poça onde a água
// ESCOA — vizinho mais baixo (saída/pour point) = superfície em DECLIVE ou beirada/pedestal (topo
// de poste, barril, mureta, borda de telhado): a água corre em vez de empoçar. Chão PLANO (rua,
// pátio) e fundo de bacia NAO têm saída -> mantêm a poça. Substitui o antigo DrainageMask: o corte
// cm-escala (3-12 cm) já engloba as quedas francas de 0.3-0.8 m dele, entao 1 função/8 taps cobrem
// os dois casos (antes eram 4+8 taps). NAO exige concavidade DE PROPOSITO: exigir bacia mataria
// poça em rua plana legítima (Bistro). O detector de bacia completo (concavidade + ausência de
// saída) fica pro bake hidrológico (ver memoria da arquitetura de fluxo). Occ map serve aqui por
// ser uso EFEMERO por-frame — NAO e estado de agua persistente (viria do heightmap do FTerrain).
float SlopeDrainMask(float3 worldPos) {
    if (RainOccParams.x < 0.5f) return 1.0f; // sem occ: sem topologia -> nao gateia (legado)

    float3 uvz = mul(float4(worldPos, 1.0f), RainOccMatrix).xyz;
    if (any(uvz.xy != saturate(uvz.xy))) return 1.0f; // fora do mapa (longe) -> nao gateia

    const float  texel  = 1.0f / RainOccParams.w;
    const float  rangeM = 1.0f / RainParams1.y;     // metros por unidade de depth do ortho
    // 8 taps num circulo de raio ~2 texels (~0.5 m): diagonais a 1.414 p/ o MESMO raio dos
    // cardinais — senao a diagonal (1.41x mais longe) usaria o mesmo limiar em cm e ficaria
    // anisotropica (rejeitaria declive diagonal cedo demais).
    const float2 offs[8] = { float2( 2.0f,   0.0f  ), float2(-2.0f,   0.0f  ),
                             float2( 0.0f,   2.0f  ), float2( 0.0f,  -2.0f  ),
                             float2( 1.414f, 1.414f), float2( 1.414f,-1.414f),
                             float2(-1.414f, 1.414f), float2(-1.414f,-1.414f) };

    float maxDrop = 0.0f; // maior queda p/ um vizinho -> saida (declive/beirada)
    [unroll] for (int i = 0; i < 8; ++i) {
        float nz = OccDepthBilinear(uvz.xy + offs[i] * texel);
        // depth MAIOR = mais BAIXO (olho em cima); (nz - uvz.z) > 0 = vizinho mais baixo
        maxDrop = max(maxDrop, (nz - uvz.z) * rangeM);
    }
    // >~12 cm de queda p/ algum lado = escoa (sem poça); <~3 cm = plano/bacia (poça mantida)
    return 1.0f - smoothstep(0.03f, 0.12f, maxDrop);
}

PSOut main(VSOutput input) {
    int2  px       = int2(input.pos.xy);
    float rawDepth = SceneDepth.Load(int3(px, 0));
    if (rawDepth <= 0.0f) discard; // ceu (reverse-Z)

    float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float4 wH  = mul(float4(ndc, rawDepth, 1.0f), InvViewProj);
    float3 worldPos = wH.xyz / wH.w;

    // F2: oclusao escala a chuva inteira — interior/marquise ficam secos (discard preserva
    // o G-buffer original do geometry pass). F4: rain = molhado ACUMULADO (inercia de
    // secagem), rainNow = chuva instantanea (aneis/splashes param na hora junto com ela).
    const float vis     = SkyVisibility(worldPos);
    const float rain    = RainParams0.x * vis;
    const float rainNow = RainParams1.z * vis;
    if (rain <= 0.0005f) discard;

    float4 gA = SceneA.Load(int3(px, 0));
    float4 gB = SceneB.Load(int3(px, 0));

    float3 albedo = gA.rgb;
    float  ao     = gA.a;
    float3 N      = GBuffer_OctDecode(gB.rg);
    float  rough  = gB.b;
    float  shadingId = gB.a; // passthrough (2 bits); o metallic mora no GBufferC, intocado

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
    // Hotfix (2 eixos): SlopeDrainMask mata poça em declive/terreno inclinado e beirada (mantém
    // plano+bacia, entao rua plana do Bistro NAO regride); (1 - porosity) impede material poroso
    // (grama/terra) de virar espelho. A porosidade aqui e PROXY VISUAL TEMPORARIO derivado da
    // roughness — NAO permeabilidade fisica; troca por mascara WetResponse/Infiltration por
    // material numa fase futura. Nas regioes ELEGIVEIS (plano/exposto/impermeavel) o noise
    // (PuddleMask) ainda distribui ONDE cada poça aparece — vira "so quebra a borda" quando a
    // colocacao autoral/bake entrar; por ora ele so nao decide mais em declive/grama.
    const float puddleBlend = RainParams0.y * rain * flatUp
                            * SlopeDrainMask(worldPos) * (1.0f - porosity);
    // limiar do noise: mais PuddleAmount*rain = pocas maiores; borda suave de 0.12
    const float thresh = lerp(0.78f, 0.42f, saturate(RainParams0.y * rain));
    float puddle = smoothstep(thresh, thresh + 0.12f, mask) * (puddleBlend > 0.001f ? 1.0f : 0.0f);
    puddle *= saturate(puddleBlend * 4.0f);

    // fade com a distancia (alem de ~60 m ripple/splash viram ruido sob TAA/FSR)
    const float distFade = saturate(1.0f - length(worldPos - CameraWorldPos.xyz) / 60.0f);

    if (puddle > 0.001f) {
        // ripples escalam com rainNOW: parou de chover, a poça fica (rain/Wetness) mas os
        // aneis somem e ela vira espelho parado (F4)
        float2 grad = RippleGradient(worldPos.xz, CameraWorldPos.w * 0.9f)
                    * RainParams0.z * rainNow * distFade;
        float3 puddleN = normalize(float3(-grad.x, 1.0f, -grad.y));

        N     = normalize(lerp(N, puddleN, puddle));
        // 0.02: abaixo do corte de espelho EXATO do ReflectionTraceMirror (<0.05). Com 0.06
        // a poça caia no GGX jittered + temporal e o reflexo amaciava — espelho d'agua de
        // verdade precisa do reflect determinístico (rodada "reflexão molhada").
        rough = lerp(rough, 0.02f, puddle);
        ao    = lerp(ao, 1.0f, puddle * 0.5f); // agua nao cavita o AO do material seco
    }

    // ---- F4: splash de impacto ----
    // Celulas de ~12 cm; por ciclo um subconjunto proporcional a chuva flasha uma coroa
    // curta (~0.3 s) que perturba a normal e baixa o roughness — glint de especular na luz
    // (spatter mask da Cry, procedural). So onde chove AGORA, up-facing, perto da camera.
    if (rainNow > 0.001f && isUp > 0.0f && distFade > 0.001f) {
        float2 sp   = worldPos.xz * 8.0f;
        float2 cell = floor(sp);
        float2 fc   = sp - cell;
        float2 rnd  = Hash22(cell);
        float  ph   = CameraWorldPos.w * (2.4f + rnd.y * 1.6f) + rnd.x * 19.0f;
        float  cyc  = floor(ph);
        float  life = ph - cyc;
        float  gate = Hash21(cell + cyc * 0.618f) < rainNow * 0.45f ? 1.0f : 0.0f;
        if (gate * (1.0f - life) > 0.05f) {
            // coroa RADIAL dentro da celula (centro/dir aleatorios por ciclo): anel
            // gaussiano que expande e morre — sem isso a celula inteira flashava e o
            // splash parecia um quad de 12 cm (bug do A/B)
            float2 rnd2 = Hash22(cell + cyc);
            float2 ctr  = 0.25f + 0.5f * rnd2;
            float2 toP  = fc - ctr;
            float  d    = length(toP);
            float  x    = (d - life * 0.35f) * 7.5f;
            float  crown = exp(-x * x) * (1.0f - life) * (1.0f - life);
            float  flash = gate * crown * isUp * distFade;
            if (flash > 0.01f) {
                float2 dirR = toP / max(d, 1e-3f); // normal empurrada radialmente p/ fora
                float3 splashN = normalize(float3(dirR.x * 0.55f, 1.0f, dirR.y * 0.55f));
                N      = normalize(lerp(N, splashN, saturate(flash * 0.75f)));
                rough  = lerp(rough, 0.08f, saturate(flash * 0.8f));
                albedo += flash * 0.05f; // respingo claro (agua espumada e mais clara)
            }
        }
    }

    PSOut o;
    o.A = float4(albedo, ao);
    o.B = float4(GBuffer_OctEncode(N), rough, shadingId);
    return o;
}
