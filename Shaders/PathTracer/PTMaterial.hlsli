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
    float3 Weight;  // f * cos / (pdf_dir * pLobe) — multiplicador do throughput
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

// Amostra UM lobe do BSDF. Weight ja embute f*cos/pdf e a probabilidade do lobe.
FPTBsdfSample PTSampleBsdf(FPTSurface s, float3 V, uint seed, uint bounce) {
    // Consome as dims SEMPRE, na mesma ordem (contrato do replay).
    float  uLobe = PTRand(seed, bounce, PT_DIM_LOBE);
    float2 u     = PTRand2(seed, bounce, PT_DIM_BSDF_U);

    FPTBsdfSample o;
    o.Valid = false;
    o.Dir   = s.N;
    o.Weight = float3(0.0f, 0.0f, 0.0f);

    float pSpec = PT_SpecProb(s);
    float3x3 basis = GGX_TangentBasis(s.N);

    if (uLobe < pSpec) {
        // GGX especular via VNDF: amostra H no espaco tangente e reflete V.
        o.Lobe = PT_LOBE_SPECULAR;
        float3 Vt = mul(basis, V); // mundo -> tangente (basis ortonormal: mul(M,v) = M*v)
        if (Vt.z <= 0.0f) return o;
        float  a  = s.Roughness * s.Roughness;
        float4 Hp = GGX_SampleVNDF(u, a, Vt);
        float3 Lt = reflect(-Vt, Hp.xyz);
        if (Lt.z <= 0.0f || Hp.w <= 0.0f) return o;
        // Peso do VNDF: f*cos/pdf = F * G2/G1 — aproximado por F (padrao em tempo real).
        float voh = saturate(dot(Vt, Hp.xyz));
        o.Dir    = normalize(mul(Lt, basis)); // tangente -> mundo
        o.Weight = PT_FresnelSchlick(PT_SpecularF0(s), voh) / pSpec;
        o.Valid  = true;
    } else {
        // Lambert via hemisferio cosseno (Malley). f*cos/pdf = albedo difuso.
        o.Lobe = PT_LOBE_DIFFUSE;
        float  r    = sqrt(u.x);
        float  phi  = 2.0f * SMILE_PI * u.y;
        float  cosT = sqrt(saturate(1.0f - u.x));
        float3 d    = float3(r * cos(phi), r * sin(phi), cosT);
        o.Dir    = normalize(mul(d, basis));
        o.Weight = PT_DiffuseAlbedo(s) / (1.0f - pSpec);
        o.Valid  = true;
    }
    return o;
}

#endif
