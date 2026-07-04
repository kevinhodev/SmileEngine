#ifndef SMILE_PT_MATERIAL_HLSLI
#define SMILE_PT_MATERIAL_HLSLI

// ReSTIR PT — BSDF unificado (Lambert difuso + GGX especular VNDF), com UM lobe amostrado por
// vertice (como o paper: shifts por-lobe; o indice do lobe vira parametro do caminho no F2).
//
// CRITICO p/ o random replay (F2): Sample e Eval sao as MESMAS funcoes no sampling inicial e no
// replay, e PTSampleBsdf consome SEMPRE as mesmas dims (LOBE, BSDF_U, BSDF_V) na mesma ordem,
// mesmo quando a amostra sai invalida — o cursor de dims nunca diverge.
//
// Requer: PTRng.hlsli (dims/PTRand), PTRayUtil.hlsli (FPTSurface), GGXSample.hlsli, PTCommon.hlsli.

#define PT_LOBE_DIFFUSE  0u
#define PT_LOBE_SPECULAR 1u

struct FPTBsdfSample {
    float3 Dir;     // direcao de continuacao (mundo)
    float3 Weight;  // f * cos / pdf — multiplicador do throughput (RGB)
    float  Pdf;     // pdf de solid-angle da direcao amostrada (one-sample MIS entre lobes)
    uint   Lobe;
    bool   Valid;
};

float3 PT_DiffuseAlbedo(FPTSurface s) { return s.Albedo * (1.0f - s.Metallic); }
float3 PT_SpecularF0(FPTSurface s)    { return lerp(float3(0.04f, 0.04f, 0.04f), s.Albedo, s.Metallic); }

// Probabilidade de amostrar o lobe especular (one-sample MIS entre lobes).
float PT_SpecProb(FPTSurface s) {
    float ld = PT_Luminance(PT_DiffuseAlbedo(s));
    float ls = PT_Luminance(PT_SpecularF0(s));
    return clamp(ls / max(ld + ls, 1e-4f), 0.05f, 0.95f);
}

float3 PT_FresnelSchlick(float3 f0, float voh) {
    float f = pow(saturate(1.0f - voh), 5.0f);
    return f0 + (1.0f - f0) * f;
}

// Smith GGX height-correlated (visibilidade V = G / (4 NoV NoL)).
float PT_SmithGGXVis(float a2, float nov, float nol) {
    float gv = nol * sqrt(nov * nov * (1.0f - a2) + a2);
    float gl = nov * sqrt(nol * nol * (1.0f - a2) + a2);
    return 0.5f / max(gv + gl, 1e-5f);
}

// f * |cos| p/ NEE (avalia os DOIS lobes — barato comparado ao shadow ray).
// Folhagem: frente = Lambert normal; atras = transmissao difusa (albedo*0.6), estilo deferred.
float3 PT_EvalBsdf(FPTSurface s, float3 V, float3 L) {
    float ndl = dot(s.N, L);
    float3 diffAlb = PT_DiffuseAlbedo(s);

    if (ndl <= 0.0f) {
        if (s.Foliage)
            return diffAlb * 0.6f / SMILE_PI * saturate(-ndl); // transmissao pela folha
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 f = diffAlb / SMILE_PI * ndl;

    float nov = saturate(dot(s.N, V));
    if (nov > 0.0f) {
        float3 H   = normalize(V + L);
        float  noh = saturate(dot(s.N, H));
        float  voh = saturate(dot(V, H));
        float  a   = s.Roughness * s.Roughness;
        float  a2  = a * a;
        float  D   = GGX_D(a2, noh);
        float  Vis = PT_SmithGGXVis(a2, nov, ndl);
        float3 F   = PT_FresnelSchlick(PT_SpecularF0(s), voh);
        f += D * Vis * F * ndl;
    }
    return f;
}

float PT_SmithG1(float a2, float nov) {
    return 2.0f * nov / max(nov + sqrt(a2 + (1.0f - a2) * nov * nov), 1e-6f);
}

// pdf de solid-angle COMBINADO (one-sample MIS difuso+especular) da direcao L. Usada p/ montar
// o peso f*cos/pdf de forma consistente entre o sampling inicial e o resolve (unbiased).
float PT_BsdfPdf(FPTSurface s, float3 V, float3 L) {
    float nol = dot(s.N, L);
    float nov = saturate(dot(s.N, V));
    float pSpec = PT_SpecProb(s);

    float pdfDiff = max(nol, 0.0f) / SMILE_PI;
    float pdfSpec = 0.0f;
    if (nol > 0.0f && nov > 0.0f) {
        float3 H   = normalize(V + L);
        float  noh = saturate(dot(s.N, H));
        float  a   = s.Roughness * s.Roughness;
        float  a2  = a * a;
        float  D   = GGX_D(a2, noh);
        pdfSpec = D * PT_SmithG1(a2, nov) / (4.0f * nov); // pdf VNDF da direcao refletida
    }
    return (1.0f - pSpec) * pdfDiff + pSpec * pdfSpec;
}

// Amostra UM lobe do BSDF. Weight = f*cos/pdf EXATO (usa PT_EvalBsdf + PT_BsdfPdf), garantindo
// que o sampling inicial e o resolve sejam consistentes (ReSTIR unbiased). Pdf tambem exposto.
FPTBsdfSample PTSampleBsdf(FPTSurface s, float3 V, uint seed, uint bounce) {
    // Consome as dims SEMPRE, na mesma ordem (contrato do replay).
    float  uLobe = PTRand(seed, bounce, PT_DIM_LOBE);
    float2 u     = PTRand2(seed, bounce, PT_DIM_BSDF_U);

    FPTBsdfSample o;
    o.Valid  = false;
    o.Dir    = s.N;
    o.Weight = float3(0.0f, 0.0f, 0.0f);
    o.Pdf    = 0.0f;

    float pSpec = PT_SpecProb(s);
    float3x3 basis = GGX_TangentBasis(s.N);

    if (uLobe < pSpec) {
        o.Lobe = PT_LOBE_SPECULAR;
        float3 Vt = mul(basis, V); // mundo -> tangente (basis ortonormal: mul(M,v) = M*v)
        if (Vt.z <= 0.0f) return o;
        float  a  = s.Roughness * s.Roughness;
        float4 Hp = GGX_SampleVNDF(u, a, Vt);
        float3 Lt = reflect(-Vt, Hp.xyz);
        if (Lt.z <= 0.0f) return o;
        o.Dir = normalize(mul(Lt, basis)); // tangente -> mundo
    } else {
        o.Lobe = PT_LOBE_DIFFUSE;
        float  r    = sqrt(u.x);
        float  phi  = 2.0f * SMILE_PI * u.y;
        float  cosT = sqrt(saturate(1.0f - u.x));
        o.Dir = normalize(mul(float3(r * cos(phi), r * sin(phi), cosT), basis));
    }

    o.Pdf = PT_BsdfPdf(s, V, o.Dir);
    if (o.Pdf <= 1e-6f) return o;
    o.Weight = PT_EvalBsdf(s, V, o.Dir) / o.Pdf; // f*cos/pdf exato
    o.Valid  = true;
    return o;
}

// Contribuicao (RGB) de reconectar x1 -> xk: BSDF em x1 na direcao de xk, vezes a radiancia Lo
// que sai de xk. Base do target function BRDF-aware do F2 (glossy volta).
//
// Roughness clampada por BAIXO em ShiftParams.y: a reconexao trata o lobe especular como se a
// superficie tivesse roughness >= roughMin, limitando o pico do D (com roughness 0.04 o D chega
// a ~1e5 — qualquer xk difuso que cruze a direcao de espelho quando a camera mexe explodia o
// peso do merge). Isso mantem target e contribuicao SIMETRICOS com o veto de connectability do
// candidato; o especular liso de verdade nao e representavel por reconexao (quase-delta) e fica
// p/ o random replay / hybrid shift (F2b).
float3 PT_ReconnectContribution(FPTSurface s1, float3 V, float3 xk, float3 Lo) {
    float3 d = xk - s1.Pos;
    float  l = length(d);
    if (l < 1e-4f) return float3(0.0f, 0.0f, 0.0f);
    FPTSurface sr = s1;
    sr.Roughness = max(s1.Roughness, ShiftParams.y);
    return PT_EvalBsdf(sr, V, d / l) * Lo; // PT_EvalBsdf ja embute f*cos
}

// Target function do F2: luminancia da contribuicao de reconexao (inclui o BRDF de x1).
float PT_PHatBrdf(FPTSurface s1, float3 V, float3 xk, float3 Lo) {
    return PT_Luminance(PT_ReconnectContribution(s1, V, xk, Lo));
}

#endif
