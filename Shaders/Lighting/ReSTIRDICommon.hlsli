#ifndef SMILE_RESTIR_DI_COMMON_HLSLI
#define SMILE_RESTIR_DI_COMMON_HLSLI

// Contrato do LightBuffer do deferred. O ReSTIR DI amostra o conjunto LOCAL inteiro;
// SpotParams.w preserva o pedido CastShadows do artista, tenha a luz recebido slice raster ou nao.
struct FGPULightFull {
    float4 PosInvRadius;
    float4 ColorSourceRadius;
    float4 DirCosOuter;
    float4 SpotParams;
    row_major float4x4 ShadowMatrix;
    float4 PrevPosInvRadius;
};

struct ReSTIRDIReservoir {
    uint   LightIndex;
    float  M;
    float  W;
    float  WeightSum;
    float3 X1;
    float2 N1Oct;
};

float DI_Luminance(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float DI_RandNext(inout uint state) {
    state = GGX_PCG(state);
    return (state & 0x00FFFFFFu) / 16777216.0f;
}

void DIResInit(out ReSTIRDIReservoir r) {
    r.LightIndex = 0xFFFFFFFFu;
    r.M = 0.0f;
    r.W = 0.0f;
    r.WeightSum = 0.0f;
    r.X1 = 0.0f;
    r.N1Oct = 0.0f;
}

// Um candidato da proposta inicial. M cresce inclusive para peso zero: M e o numero de amostras
// vistas pelo stream, nao o numero das que contribuiram (cap. 22 e Alg. 3 do paper original).
bool DIResUpdate(inout ReSTIRDIReservoir r, uint lightIndex, float weight, inout uint rng) {
    r.M += 1.0f;
    r.WeightSum += weight;
    if (weight > 0.0f && DI_RandNext(rng) * r.WeightSum < weight) {
        r.LightIndex = lightIndex;
        return true;
    }
    return false;
}

// Combina o stream inteiro de `other` em O(1). Para amostra discreta de luz nao existe Jacobiano
// de reconexao: o dominio e o indice da luz; so a target PDF e reavaliada no pixel de destino.
bool DIResMerge(inout ReSTIRDIReservoir r, ReSTIRDIReservoir other,
                float targetAtDestination, inout uint rng) {
    const float weight = targetAtDestination * other.W * other.M;
    r.M += other.M;
    r.WeightSum += weight;
    if (weight > 0.0f && DI_RandNext(rng) * r.WeightSum < weight) {
        r.LightIndex = other.LightIndex;
        return true;
    }
    return false;
}

void DIResFinalize(inout ReSTIRDIReservoir r, float selectedTarget) {
    r.W = (r.LightIndex != 0xFFFFFFFFu && r.M > 0.0f && selectedTarget > 0.0f)
        ? r.WeightSum / (r.M * selectedTarget) : 0.0f;
}

// ResB e R32G32B32A32_UINT. Layout:
//   .x = LightIndex, 32 bits (0xFFFFFFFF = sem amostra)
//   .y = RESERVADO p/ a UV da amostra na superficie da luz (16+16), necessaria quando mesh lights
//        entrarem: um triangulo emissivo nao e identificado so pelo indice. Hoje sempre 0.
//   .z = M (16 bits) | idade (8 bits) | livre (8)
//   .w = normal do ponto visivel, octaedrica em unorm 16+16
//
// Era RGBA16F, e o formato antigo tinha DOIS tetos que ja doiam: o LightIndex em fp16 e exato so
// ate 2048 (a cena de teste tem 26 mil triangulos emissivos, entao mesh lights quebravam na hora),
// e M + idade dividiam um canal, prendendo M em 63 — o que impedia o MCap relativo do paper
// (20x o M do frame atual = 160 com 8 candidatas). Custa 8 B/pixel a mais por buffer.
uint DI_PackUnorm16x2(float2 v) {
    const uint2 q = (uint2)(saturate(v * 0.5f + 0.5f) * 65535.0f + 0.5f);
    return q.x | (q.y << 16);
}

float2 DI_UnpackUnorm16x2(uint p) {
    return float2(p & 0xFFFFu, p >> 16) * (2.0f / 65535.0f) - 1.0f;
}

uint DI_PackMAge(float M, float age) {
    return min((uint)(M + 0.5f), 65535u) | (min((uint)max(age, 0.0f), 255u) << 16);
}

void DI_UnpackMAge(uint packed, out float M, out float age) {
    M   = (float)(packed & 0xFFFFu);
    age = (float)((packed >> 16) & 0xFFu);
}

ReSTIRDIReservoir DI_LoadReservoir(float4 a, uint4 b) {
    ReSTIRDIReservoir r;
    DIResInit(r);
    r.X1 = a.xyz;
    r.W = a.w;
    r.LightIndex = b.x;
    float age;
    DI_UnpackMAge(b.z, r.M, age);
    r.N1Oct = DI_UnpackUnorm16x2(b.w);
    return r;
}

void DI_StoreReservoir(ReSTIRDIReservoir r, float age,
                       out float4 a, out uint4 b) {
    a = float4(r.X1, r.W);
    b = uint4(r.LightIndex, 0u, DI_PackMAge(r.M, age), DI_PackUnorm16x2(r.N1Oct));
}

bool DI_IsShadowCaster(FGPULightFull light) {
    return light.SpotParams.w > 0.5f;
}

// Ponto amostrado na fonte esferica, alvo do raio de sombra. Sem isto o raio vai ao CENTRO, a
// visibilidade e binaria e a sombra sai DURA — incoerente com o especular, que ja trata a luz como
// esfera de raio SourceRadius (AreaSphereSpecular). Uma esfera emissora produz penumbra que abre
// com a distancia ao bloqueador; e o mesmo raio da UI (LightsBridge clampa em 0,01..2,0 m) que
// controla os tres usos, porque os tres sao a mesma esfera: piso do inverse-square, ponto
// representativo do especular e agora o alvo da sombra.
//
// Amostra o disco da SILHUETA (perpendicular a L, raio SourceRadius): e ele que define a extensao
// angular do bloqueio, e evita sortear pontos no verso da esfera. Aproximacao boa enquanto
// SourceRadius << distancia, no mesmo espirito do ponto representativo do especular.
//
// O dominio do reservoir NAO muda: continua sendo o indice da luz. O ponto e estratificacao
// POS-selecao, entao a matematica do resampling fica intacta — o que muda e so o alvo do raio.
// Seed do ponto na fonte. Depende do pixel, do frame e do INDICE DA LUZ — e a dependencia da luz
// que faz o Pass A e o Pass B sortearem o MESMO ponto quando testam a mesma luz.
//
// Se sorteassem pontos independentes, o descarte do Alg. 5 (sobrevive com probabilidade 1-f) e a
// visibilidade do resolve (outra 1-f) se multiplicariam e a penumbra sairia com (1-f)^2, escura
// demais. Concordando o ponto, o descarte e o resolve concordam e nao ha dupla aplicacao — e o
// mesmo efeito que o RTXDI obtem de graca por guardar a UV da amostra no reservoir. Quando os
// passes escolhem luzes DIFERENTES nao ha o que casar, e cada um sorteia o seu.
uint DI_LightPointSeed(uint2 px, uint frame, uint lightIndex) {
    return GGX_SeedE(px, frame ^ (lightIndex * 0x9E3779B9u), SMILE_RNG_DI_LIGHTPOINT);
}

float3 DI_SampleLightPoint(FGPULightFull light, float3 L, inout uint rng) {
    const float radius = light.ColorSourceRadius.w;
    if (radius <= 0.0f) return light.PosInvRadius.xyz;

    // Base ortonormal em torno de L sem branch (Duff et al. 2017). L ja vem normalizado do
    // PunctualLightIncoming; o denominador (s + L.z) vale -2 no pior caso, nunca zero.
    const float  s  = (L.z >= 0.0f) ? 1.0f : -1.0f;
    const float  a  = -1.0f / (s + L.z);
    const float  b  = L.x * L.y * a;
    const float3 t1 = float3(1.0f + s * L.x * L.x * a, s * b, -s * L.x);
    const float3 t2 = float3(b, s + L.y * L.y * a, -L.y);

    // sqrt no raio = uniforme em AREA do disco (senao concentra no centro e estreita a penumbra).
    const float rr  = radius * sqrt(DI_RandNext(rng));
    const float phi = 6.28318530718f * DI_RandNext(rng);
    return light.PosInvRadius.xyz + (t1 * cos(phi) + t2 * sin(phi)) * rr;
}

// Avalia a target PDF nao normalizada do ReSTIR: luminancia da contribuicao SEM visibilidade.
// Devolve os lobos separados para o resolve futuro do NRD; o marco atual compoe a soma crua.
float DI_Evaluate(FGPULightFull light, GBufferData g, float3 worldPos, float3 cameraPos,
                  out float3 outDiffuse, out float3 outSpecular,
                  out float3 outL, out float outDist) {
    outDiffuse = 0.0f;
    outSpecular = 0.0f;
    outL = float3(0.0f, 1.0f, 0.0f);
    outDist = 0.0f;

    FPunctualLight punctual;
    punctual.PosInvRadius = light.PosInvRadius;
    punctual.ColorSourceRadius = light.ColorSourceRadius;
    punctual.DirCosOuter = light.DirCosOuter;
    punctual.SpotParams = light.SpotParams;

    const float3 incoming = PunctualLightIncoming(punctual, worldPos, outL, outDist);
    if (all(incoming <= 0.0f)) return 0.0f;

    const float3 N = g.WorldNormal;
    const float3 V = normalize(cameraPos - worldPos);
    const float3 diffuseColor = g.BaseColor * (1.0f - g.Metallic);
    const float3 specularColor = lerp(float3(0.04f, 0.04f, 0.04f), g.BaseColor, g.Metallic);
    const float3 transColor = g.BaseColor * g.Subsurface;
    const float roughness = max(g.Roughness, 0.04f);
    const float a2 = roughness * roughness * roughness * roughness;

    float3 Ls;
    const float specEnergy = AreaSphereSpecular(light.ColorSourceRadius.w, roughness,
                                                 outL * outDist, V, N, Ls);
    BRDF_DirectAreaSplit(N, V, outL, Ls, specEnergy, incoming,
                         diffuseColor, specularColor, roughness, a2, transColor,
                         outDiffuse, outSpecular);
    return DI_Luminance(outDiffuse + outSpecular);
}

#endif
