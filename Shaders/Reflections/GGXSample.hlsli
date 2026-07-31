#ifndef SMILE_GGX_SAMPLE_HLSLI
#define SMILE_GGX_SAMPLE_HLSLI

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979f
#endif

float3x3 GGX_TangentBasis(float3 N) {
    float Sign = N.z >= 0.0f ? 1.0f : -1.0f;
    float a = -rcp(Sign + N.z);
    float b = N.x * N.y * a;
    float3 Tx = float3(1.0f + Sign * a * N.x * N.x, Sign * b, -Sign * N.x);
    float3 Ty = float3(b, Sign + a * N.y * N.y, -N.y);
    return float3x3(Tx, Ty, N);
}

float GGX_D(float a2, float NoH) {
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / (SMILE_PI * d * d);
}

float4 GGX_SampleVNDF(float2 E, float alpha, float3 V) {
    float2 Alpha = float2(alpha, alpha);
    float3 Vh = normalize(float3(Alpha * V.xy, V.z));

    float Phi = (2.0f * SMILE_PI) * E.x;
    float aa = saturate(alpha);
    float s = 1.0f + length(V.xy);
    float aa2 = aa * aa, s2 = s * s;
    float k = (s2 - aa2 * s2) / (s2 + aa2 * V.z * V.z);

    float Z = lerp(1.0f, -k * Vh.z, E.y);
    float SinTheta = sqrt(saturate(1.0f - Z * Z));
    float3 H = float3(SinTheta * cos(Phi), SinTheta * sin(Phi), Z) + Vh;
    H = normalize(float3(Alpha * H.xy, max(0.0f, H.z)));

    float a2  = alpha * alpha;
    float NoV = V.z, NoH = H.z, VoH = dot(V, H);
    float d   = (NoH * a2 - NoH) * NoH + 1.0f;
    float D   = a2 / (SMILE_PI * d * d);
    float pdf = 2.0f * VoH * D / (k * NoV + sqrt(NoV * (NoV - NoV * a2) + a2));
    return float4(H, pdf);
}

// PCG de 1 entrada — usado p/ AVANCAR um stream ja semeado (RngNext), nao p/ semear.
uint GGX_PCG(uint v) {
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

// Hash 4->1 (xxHash32, Jarzynski & Olano 2020) usado p/ SEMEAR os sorteios: w = ID do efeito,
// x = frame, yz = pixel. Sem o ID no seed, GI difuso, reflexao glossy e mirror sorteiam a MESMA
// sequencia no mesmo pixel/frame — e um denoiser que aprende correlacao entre efeitos (DLSS Ray
// Reconstruction) revela isso na saida. GPU Zen 3 cap. 7, "Correlated Sampling": eles trocaram
// blue noise por esta funcao exatamente por isso, e a atribuicao dos canais e a deles.
uint GGX_XXHash32(uint4 p) {
    const uint PRIME32_2 = 2246822519u, PRIME32_3 = 3266489917u;
    const uint PRIME32_4 =  668265263u, PRIME32_5 =  374761393u;
    uint h32 = p.w + PRIME32_5 + p.x * PRIME32_3;
    h32 = PRIME32_4 * ((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.y * PRIME32_3;
    h32 = PRIME32_4 * ((h32 << 17) | (h32 >> (32 - 17)));
    h32 += p.z * PRIME32_3;
    h32 = PRIME32_4 * ((h32 << 17) | (h32 >> (32 - 17)));
    h32 = PRIME32_2 * (h32 ^ (h32 >> 15));
    h32 = PRIME32_3 * (h32 ^ (h32 >> 13));
    return h32 ^ (h32 >> 16);
}

// IDs de stream. Espacados de 1000 p/ que uma variante possa somar um indice local (tap do
// espacial, bounce futuro) sem invadir o vizinho.
#define SMILE_RNG_GI_INITIAL    1000u // direcao do raio do sample inicial do ReSTIR GI
#define SMILE_RNG_GI_WRS        2000u // selecao estocastica do WRS temporal
#define SMILE_RNG_SPATIAL_WRS   3000u // selecao estocastica do WRS espacial
#define SMILE_RNG_SPATIAL_TAP   4000u // + i: offset do i-esimo vizinho do reuso espacial
#define SMILE_RNG_REFL_GLOSSY   5000u // VNDF da reflexao glossy
#define SMILE_RNG_REFL_MIRROR   6000u // VNDF do passe mirror
#define SMILE_RNG_DI_INITIAL    8000u // proposta uniforme das candidatas iniciais do ReSTIR DI
#define SMILE_RNG_DI_TEMPORAL   9000u // WRS inicial + temporal do ReSTIR DI
#define SMILE_RNG_DI_SPATIAL   10000u // WRS espacial do ReSTIR DI
#define SMILE_RNG_DI_SPATIAL_TAP 11000u // + i: offset do vizinho espacial do ReSTIR DI

uint GGX_SeedE(uint2 px, uint frame, uint effect) {
    return GGX_XXHash32(uint4(frame, px.x, px.y, effect));
}
// NAO existe versao sem `effect`: a de 2 argumentos foi removida de proposito, para que um
// chamador novo nao volte a compartilhar stream por descuido.
float2 GGX_Rand2E(uint2 px, uint frame, uint effect) {
    uint s0 = GGX_SeedE(px, frame, effect);
    uint s1 = GGX_PCG(s0);
    return float2(s0 & 0xffffu, s1 & 0xffffu) * (1.0f / 65535.0f);
}

float2 GGX_Hammersley(uint i, uint n, uint2 random) {
    float e1 = frac((float)i / n + float(random.x & 0xffffu) / 65536.0f);
    float e2 = float(reversebits(i) ^ random.y) * 2.3283064365386963e-10f;
    return float2(e1, e2);
}

int2 RefTileJitter(uint2 HalfCoord, uint Frame) {
    uint2 Cell = HalfCoord % 2u;
    uint Lin = (Cell.x + Cell.y * 2u + Frame) % 4u;
    return int2((Lin & 0x2u) ? 1 : 0, (Lin & 0x1u) ? 0 : 1);
}

float2 GGX_ConcentricDisk(float2 E) {
    float2 p = 2.0f * E - 1.0f;
    if (p.x == 0.0f && p.y == 0.0f) return float2(0.0f, 0.0f);
    float r, theta;
    if (abs(p.x) > abs(p.y)) { r = p.x; theta = (SMILE_PI / 4.0f) * (p.y / p.x); }
    else                     { r = p.y; theta = SMILE_PI / 2.0f - (SMILE_PI / 4.0f) * (p.x / p.y); }
    return r * float2(cos(theta), sin(theta));
}

#endif 
