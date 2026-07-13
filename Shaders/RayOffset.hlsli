#ifndef SMILE_RAY_OFFSET_HLSLI
#define SMILE_RAY_OFFSET_HLSLI

// Piso do offset p/ origens reconstruidas do G-buffer (ver OffsetRayGBuffer): cobre o erro de
// reconstrucao depth+normal E geometria embutida/coplanar (props rentes a parede). Knobs de
// calibracao — subir kRayOriginFloorMin se props flush escurecerem (raio nascendo dentro da
// casca do asset vizinho) ou aparecer acne; o custo e reintroduzir erro de medida NA MESMA
// escala do piso (era 0.2 antes do fix — qualquer valor << 0.2 ja e ganho).
#define kRayOriginFloorPerMeter 2e-4f  // ~0.2 mm por metro de distancia da camera
// MODO LEGADO TEMPORARIO (2026-07-12): piso em 0.2 reproduz o normal-bias antigo — raios de
// gather "tunelam" p/ fora de bolsoes fechados (janelas) e os inundam de luz falsa ESTAVEL,
// mascarando a variancia da luz real (manchas que o REBLUR nao doma). Custo conhecido: energia
// ~1.9x em contatos, hitT do NRD nunca < 0.2, reflexo de contato deslocado. Voltar p/ 2e-2f
// (calibrado p/ props flush do Bistro) quando a migracao REBLUR->RELAX_DIFFUSE_SPECULAR sair.
#define kRayOriginFloorMin      2e-1f

// Origem robusta p/ raios que saem de uma superficie — Wächter & Binder, "A Fast and Robust
// Method for Avoiding Self-Intersection" (Ray Tracing Gems, cap. 6). Desloca a posicao em ULPs
// ao longo da normal GEOMETRICA operando na representacao inteira do float: o passo cresce com
// o expoente da coordenada (scale-invariant), sem constante de cena. Perto da origem (|p| <
// 1/32) o passo ULP degenera, entao cai num epsilon flutuante fixo.
float3 OffsetRayWB(float3 p, float3 n) {
    const float kOrigin     = 1.0f / 32.0f;
    const float kFloatScale = 1.0f / 65536.0f;
    const float kIntScale   = 256.0f;
    int3   ofI = int3(kIntScale * n);
    float3 pI  = asfloat(asint(p) + ((p < 0.0f) ? -ofI : ofI));
    return float3(abs(p.x) < kOrigin ? p.x + kFloatScale * n.x : pI.x,
                  abs(p.y) < kOrigin ? p.y + kFloatScale * n.y : pI.y,
                  abs(p.z) < kOrigin ? p.z + kFloatScale * n.z : pI.z);
}

// Variante p/ posicoes RECONSTRUIDAS do G-buffer (depth + normal octaedrica): Wächter/Binder
// assume o hit exato da propria geometria, mas a reconstrucao via InvViewProj + normal
// quantizada tem erro maior que ULP do hit. Soma o piso configuravel acima por cima do offset
// ULP. Substitui o normal-bias fixo de 0.2 que contaminava a medida do ReSTIR (pHat/Jacobiano/
// hitT do NRD nunca abaixo de ~0.2 em contato) e deslocava reflexos de contato em ate 20 cm.
float3 OffsetRayGBuffer(float3 p, float3 n, float camDist) {
    return OffsetRayWB(p, n) + n * max(kRayOriginFloorPerMeter * camDist, kRayOriginFloorMin);
}

#endif
