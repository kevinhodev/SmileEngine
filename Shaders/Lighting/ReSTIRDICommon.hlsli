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

// ResB e RGBA16F. Com MCap <= 32 e MaxAge <= 16, M + 64*age fica abaixo de 1088 e todo inteiro e
// exatamente representavel em fp16. LightIndex <= 255 tambem e exato.
float DI_PackMAge(float M, float age) {
    return min(M, 63.0f) + 64.0f * min(floor(age), 16.0f);
}

void DI_UnpackMAge(float packed, out float M, out float age) {
    age = floor(packed / 64.0f);
    M = packed - age * 64.0f;
}

ReSTIRDIReservoir DI_LoadReservoir(float4 a, float4 b) {
    ReSTIRDIReservoir r;
    DIResInit(r);
    r.X1 = a.xyz;
    r.W = a.w;
    if (b.x >= 0.0f) r.LightIndex = (uint)(b.x + 0.5f);
    float age;
    DI_UnpackMAge(b.y, r.M, age);
    r.N1Oct = b.zw;
    return r;
}

void DI_StoreReservoir(ReSTIRDIReservoir r, float age,
                       out float4 a, out float4 b) {
    a = float4(r.X1, r.W);
    const float lightIndex = (r.LightIndex == 0xFFFFFFFFu) ? -1.0f : (float)r.LightIndex;
    b = float4(lightIndex, DI_PackMAge(r.M, age), r.N1Oct);
}

bool DI_IsShadowCaster(FGPULightFull light) {
    return light.SpotParams.w > 0.5f;
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
