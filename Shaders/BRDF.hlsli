#ifndef SMILE_BRDF_HLSLI
#define SMILE_BRDF_HLSLI

#define USE_KULLA_CONTY_ENERGY_CONSERVATION 1
#define USE_BURLEY_DIFFUSE 1
#define USE_EXACT_SMITH 1

static const float BRDF_PI = 3.14159265359f;

void GGXEnergyLookup(float Roughness, float NoV, out float E, out float Ef) {
    float r = Roughness;
    float c = NoV;
    E = 1.0f - saturate(pow(r, c / (r + 1e-5f)) * ((r * c + 0.0266916f) / (0.466495f + c)));
    Ef = pow(1.0f - c, 5.0f) * pow(2.36651f * pow(c, 4.7703f * r) + 0.0387332f, r);
}

float3 Diffuse_Burley(float3 DiffuseColor, float Roughness, float NoV, float NoL, float VoH) {
    float FD90 = 0.5f + 2.0f * VoH * VoH * Roughness;
    float FdV = 1.0f + (FD90 - 1.0f) * pow(1.0f - NoV, 5.0f);
    float FdL = 1.0f + (FD90 - 1.0f) * pow(1.0f - NoL, 5.0f);
    return DiffuseColor * ((1.0f / BRDF_PI) * FdV * FdL);
}

float D_GGX(float a2, float NoH) {
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / (BRDF_PI * d * d);
}

float Vis_SmithJointApprox(float a2, float NoV, float NoL) {
    float a = sqrt(a2);
    float Vis_SmithV = NoL * (NoV * (1.0f - a) + a);
    float Vis_SmithL = NoV * (NoL * (1.0f - a) + a);
    return 0.5f / (Vis_SmithV + Vis_SmithL + 1e-5f);
}

float Vis_SmithJointExact(float a2, float NoV, float NoL) {
    float Vis_SmithV = NoL * sqrt(NoV * (NoV - NoV * a2) + a2);
    float Vis_SmithL = NoV * sqrt(NoL * (NoL - NoL * a2) + a2);
    return 0.5f / (Vis_SmithV + Vis_SmithL + 1e-5f);
}

float3 F_Schlick(float3 SpecularColor, float VoH) {
    float Fc  = pow(1.0f - VoH, 5.0f);
    float f90 = saturate(50.0f * SpecularColor.g);
    return f90 * Fc + (1.0f - Fc) * SpecularColor;
}

float3 F_SchlickRoughness(float3 F0, float cosTheta, float roughness) {
    float r1 = 1.0f - roughness;
    return F0 + (max(float3(r1, r1, r1), F0) - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 RotateY(float3 v, float angle) {
    float s = sin(angle), c = cos(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 FoliageTransmission(float3 N, float3 V, float3 L, float3 Radiance, float3 TransColor) {
    const float Wrap    = 0.5f;
    float  WrapNoL = saturate((-dot(N, L) + Wrap) / ((1.0f + Wrap) * (1.0f + Wrap)));
    float  VoL     = dot(V, L);
    float  Scatter = D_GGX(0.6f * 0.6f, saturate(-VoL));
    return Radiance * (WrapNoL * Scatter) * TransColor;
}

// Variante com saidas SEPARADAS de difuso e especular. Usada pelos translucidos: o tinte
// difuso e pesado por alpha, mas o reflexo especular da superficie soma inteiro (estilo
// ThinTranslucent da UE). A transmissao de folhagem entra no difuso.
// CONVENCAO DE METALLIC (vale para as tres funcoes deste arquivo e para todo o ambiente):
// o fator (1 - Metallic) e aplicado UMA UNICA VEZ, na montagem do DiffuseColor
// (`BaseColor * (1 - Metallic)`, no G-buffer/deferred/forward). Aqui dentro o Kd carrega so a
// conservacao de energia com o especular — Kulla-Conty ou (1 - F). O `Metallic` saiu da
// assinatura porque virou parametro morto: o SpecularColor ja encapsula o metal
// (lerp(0.04, BaseColor, Metallic)).
//
// Antes o Kd comecava em `1 - Metallic` e multiplicava um DiffuseColor que JA trazia o fator,
// ou seja (1-M)^2 — em M = 0,5 o difuso saia com 0,25*BaseColor em vez de 0,5*BaseColor. Isso
// valia igualmente no direto e no ambiente, entao nao havia divergencia entre eles: a cadeia
// difusa inteira estava escura. Referencias: Filament (diffuseColor = baseColor*(1-metallic)),
// glTF 2.0 (interpolacao linear dielectrico<->metal) e o Flax, que calcula GetDiffuseColor uma
// vez e a consome direto no direto e no bounce do DDGI.
void BRDF_DirectSplit(float3 N, float3 V, float3 L, float3 Radiance,
                      float3 DiffuseColor, float3 SpecularColor,
                      float Roughness, float a2, float3 TransColor,
                      out float3 OutDiffuse, out float3 OutSpecular) {
    OutDiffuse  = float3(0.0f, 0.0f, 0.0f);
    OutSpecular = float3(0.0f, 0.0f, 0.0f);

    float NoL = saturate(dot(N, L));
    if (NoL > 0.0f) {
        float3 H   = normalize(L + V);
        float  NoV = saturate(dot(N, V));
        float  NoH = saturate(dot(N, H));
        float  VoH = saturate(dot(V, H));

        float D = D_GGX(a2, NoH);
        #if USE_EXACT_SMITH
            float Vis = Vis_SmithJointExact(a2, NoV, NoL);
        #else
            float Vis = Vis_SmithJointApprox(a2, NoV, NoL);
        #endif
        float3 F = F_Schlick(SpecularColor, VoH);
        float3 Specular = (D * Vis) * F;

        float3 Kd = 1.0f; // metallic ja esta no DiffuseColor (ver a nota da convencao acima)

        #if USE_KULLA_CONTY_ENERGY_CONSERVATION
            float E_val, Ef_val;
            GGXEnergyLookup(Roughness, NoV, E_val, Ef_val);
            float3 W = 1.0f + SpecularColor * ((1.0f - E_val) / max(E_val, 1e-5f));
            Specular *= W;

            float3 F90    = saturate(50.0f * SpecularColor.g);
            float3 E_spec = W * (E_val * SpecularColor + Ef_val * (F90 - SpecularColor));
            float  DiffuseScale = 1.0f - saturate(dot(E_spec, float3(0.2126f, 0.7152f, 0.0722f)));
            Kd *= DiffuseScale;
        #else
            Kd *= (1.0f - F);
        #endif

        #if USE_BURLEY_DIFFUSE
            float3 Diffuse = Kd * Diffuse_Burley(DiffuseColor, Roughness, NoV, NoL, VoH);
        #else
            float3 Diffuse = (Kd * DiffuseColor) / BRDF_PI;
        #endif

        OutDiffuse  = Diffuse * Radiance * NoL;
        OutSpecular = Specular * Radiance * NoL;
    }

    if (any(TransColor > 0.0f))
        OutDiffuse += FoliageTransmission(N, V, L, Radiance, TransColor);
}

float3 BRDF_Direct(float3 N, float3 V, float3 L, float3 Radiance,
                   float3 DiffuseColor, float3 SpecularColor,
                   float Roughness, float a2, float3 TransColor) {
    float3 Diffuse, Specular;
    BRDF_DirectSplit(N, V, L, Radiance, DiffuseColor, SpecularColor,
                     Roughness, a2, TransColor, Diffuse, Specular);
    return Diffuse + Specular;
}

// F4 — representative point na ESFERA da fonte (porte da parte de esfera do AreaLightSpecular do
// Flax / UE): desloca a direcao do especular pro ponto da esfera mais proximo do raio refletido e
// devolve a normalizacao de energia do lobo alargado (sem ela o highlight de fonte grande estoura).
// Difuso/atenuacao/sombra seguem usando o CENTRO.
//
// Mora aqui, e nao no DeferredLighting.ps, porque tem DOIS consumidores: o loop de luzes locais do
// deferred e o DI-lite (que sombreia a luz excedente e precisa do mesmo especular, senao a luz
// muda de aparencia ao cruzar a fronteira do orcamento de shadow map).
float AreaSphereSpecular(float SourceRadius, float Roughness, float3 ToLightCenter,
                         float3 V, float3 N, out float3 Ls) {
    float  m = Roughness * Roughness;
    float3 r = reflect(-V, N);
    float  invDist = rsqrt(dot(ToLightCenter, ToLightCenter));

    float sphereAngle = saturate(SourceRadius * invDist);
    float e = m / saturate(m + 0.5f * sphereAngle);
    float energy = e * e;

    float3 closestPointOnRay = dot(ToLightCenter, r) * r;
    float3 centerToRay = closestPointOnRay - ToLightCenter;
    float3 closest = ToLightCenter + centerToRay *
        saturate(SourceRadius * rsqrt(max(dot(centerToRay, centerToRay), 1e-8f)));
    // Superficie EXATAMENTE na posicao da luz => closest = 0 e normalize(0) = 0/0 = NaN.
    // NaN aqui nao custa um pixel: ele sobrevive ao bloom (downsample espalha) e ao
    // historico do TAA/upscaler, entao vira um bloco corrompido persistente. Cai pra
    // normal — a essa distancia o especular ja esta no piso do bulbo e nao se ve.
    float closest2 = dot(closest, closest);
    Ls = (closest2 > 1e-12f) ? closest * rsqrt(closest2) : N;
    return energy;
}

// F4 (luzes de area "soft", estilo UE/Flax): variante do BRDF_Direct com direcoes SEPARADAS —
// difuso usa o CENTRO da fonte (Ld: e de la que vem a energia media), especular usa o
// representative point na superficie da fonte (Ls) com o fator de normalizacao do lobo
// alargado (SpecEnergy) — o highlight cresce com o tamanho aparente da fonte em vez de
// estourar. Sol/lua e caminhos sem area seguem no BRDF_Direct.
float3 BRDF_DirectArea(float3 N, float3 V, float3 Ld, float3 Ls, float SpecEnergy,
                       float3 Radiance, float3 DiffuseColor, float3 SpecularColor,
                       float Roughness, float a2, float3 TransColor) {
    float3 Result = float3(0.0f, 0.0f, 0.0f);

    float NoLd = saturate(dot(N, Ld));
    float NoLs = saturate(dot(N, Ls));
    if (NoLd > 0.0f || NoLs > 0.0f) {
        float3 H   = normalize(Ls + V);
        float  NoV = saturate(dot(N, V));
        float  NoH = saturate(dot(N, H));
        float  VoH = saturate(dot(V, H));

        float D = D_GGX(a2, NoH);
        #if USE_EXACT_SMITH
            float Vis = Vis_SmithJointExact(a2, NoV, NoLs);
        #else
            float Vis = Vis_SmithJointApprox(a2, NoV, NoLs);
        #endif
        float3 F = F_Schlick(SpecularColor, VoH);
        float3 Specular = (D * Vis) * F * SpecEnergy;

        float3 Kd = 1.0f; // metallic ja esta no DiffuseColor (ver a nota da convencao acima)

        #if USE_KULLA_CONTY_ENERGY_CONSERVATION
            float E_val, Ef_val;
            GGXEnergyLookup(Roughness, NoV, E_val, Ef_val);
            float3 W = 1.0f + SpecularColor * ((1.0f - E_val) / max(E_val, 1e-5f));
            Specular *= W;

            float3 F90    = saturate(50.0f * SpecularColor.g);
            float3 E_spec = W * (E_val * SpecularColor + Ef_val * (F90 - SpecularColor));
            float  DiffuseScale = 1.0f - saturate(dot(E_spec, float3(0.2126f, 0.7152f, 0.0722f)));
            Kd *= DiffuseScale;
        #else
            Kd *= (1.0f - F);
        #endif

        #if USE_BURLEY_DIFFUSE
            float3 Diffuse = Kd * Diffuse_Burley(DiffuseColor, Roughness, NoV, NoLd, VoH);
        #else
            float3 Diffuse = (Kd * DiffuseColor) / BRDF_PI;
        #endif

        Result += Diffuse * Radiance * NoLd + Specular * Radiance * NoLs;
    }

    if (any(TransColor > 0.0f))
        Result += FoliageTransmission(N, V, Ld, Radiance, TransColor);

    return Result;
}

#endif
