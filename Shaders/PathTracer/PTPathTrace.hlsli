#ifndef SMILE_PT_PATHTRACE_HLSLI
#define SMILE_PT_PATHTRACE_HLSLI

// ReSTIR PT — nucleo do path tracing: NEE do sol por vertice (DI+GI unificados, Enhanced §6.1),
// continuacao por BSDF, escape p/ o ceu, terminacao no DDGI (radiance cache) e Russian roulette
// so no sampling inicial (Enhanced §6.2.4). F0 usa direto como megakernel; F1+ reusa as mesmas
// funcoes p/ gerar candidatos e p/ o random replay.
//
// Convencoes de luz (identicas ao ShadeSurfaceHit/deferred):
//  - Sol so via NEE (o disco nao esta no SkyView LUT); ceu so via escape do BSDF.
//  - DDGI atlas guarda irradiancia ja na convencao "E/pi": difuso = albedo * E_atlas.
//
// Requer: PTCommon/PTRng/PTRayUtil/PTMaterial + HitShading.hlsli (ShadeSky, SMILE_RT_PROCEED).

// Amostra uma direcao no cone angular do sol (sombra suave + contact hardening).
float3 PT_SunConeDir(float3 sunDir, float cosMax, float2 u) {
    float cosT = lerp(1.0f, cosMax, u.x);
    float sinT = sqrt(saturate(1.0f - cosT * cosT));
    float phi  = 2.0f * SMILE_PI * u.y;
    float3 d   = float3(sinT * cos(phi), sinT * sin(phi), cosT);
    return normalize(mul(d, GGX_TangentBasis(sunDir)));
}

// NEE do sol num vertice: 1 shadow ray no cone + avaliacao dos dois lobes do BSDF.
// Folhagem recebe luz dos dois lados (transmissao no Eval); o raio parte do lado que encara o sol.
float3 PT_SunNEE(FPTSurface s, float3 V, uint seed, uint bounce) {
    float3 sunDir = normalize(SunDirIntensity.xyz);
    float2 u      = PTRand2(seed, bounce, PT_DIM_NEE_U);
    float3 dir    = PT_SunConeDir(sunDir, SunColorCosRadius.w, u);

    float ndl = dot(s.N, dir);
    if (ndl <= 0.0f && !s.Foliage)
        return float3(0.0f, 0.0f, 0.0f);

    float  bias   = max(TraceParams.w, 1e-3f);
    float3 side   = (ndl >= 0.0f) ? s.N : -s.N; // folhagem: sai pelo lado que encara o sol
    float  vis    = PTShadowRayVis(s.Pos + side * bias, dir, TraceParams.y);
    if (vis <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float3 Esun = SunColorCosRadius.rgb * SunDirIntensity.w;
    return PT_EvalBsdf(s, V, dir) * Esun;
}

// Cauda de terminacao no DDGI (radiance cache): albedo * irradiancia do atlas (convencao E/pi,
// identica ao ShadeSurfaceHit).
float3 PT_DDGITail(FPTSurface h) {
    return h.Albedo * SampleDDGIIrradiance(IrradAtlas, LinearClamp, h.Pos, h.N,
                                           GridMinSpacing.xyz, GridMinSpacing.w,
                                           (int3)GridCount.xyz, (int)AtlasParams.x,
                                           float2(1.0f / AtlasParams.y, 1.0f / AtlasParams.z));
}

// Radiancia de um caminho a partir da superficie s (a emissao de s fica com o caller).
// outFirstDist = |x2-x1| do 1o bounce (hitDist p/ o NRD).
// enableRR: Russian roulette so no sampling inicial; o replay (F2b) nunca entra aqui.
// bounceBase (F2b): offset GLOBAL das dims — o caminho inteiro usa UM seed e indices de bounce
// globais, entao qualquer prefixo e replayavel bit-exato e o sufixo (a partir de bounceBase)
// nunca colide com as dims do prefixo. maxBounces==0 = vertice terminal (NEE + cauda DDGI).
float3 PT_PathRadiance(FPTSurface s, float3 V, uint seed, uint maxBounces, uint rrStartBounce,
                       bool enableRR, uint bounceBase, out float outFirstDist) {
    outFirstDist = TraceParams.y;
    if (maxBounces == 0)
        return PT_SunNEE(s, V, seed, bounceBase) + PT_DDGITail(s);

    float3 L = float3(0.0f, 0.0f, 0.0f);
    float3 T = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (uint bounce = 0; bounce < maxBounces; ++bounce) {
        uint gb = bounceBase + bounce; // indice global da dim deste vertice

        // NEE do sol neste vertice (bounce 0 em x1 = luz direta unificada no mesmo estimador).
        L += T * PT_SunNEE(s, V, seed, gb);

        // Russian roulette (Enhanced §6.2.4: NUNCA no replay — a dim RR sai da parametrizacao).
        if (enableRR && gb >= rrStartBounce) {
            float p = clamp(max(T.x, max(T.y, T.z)), 0.05f, 0.95f);
            if (PTRand(seed, gb, PT_DIM_RR) >= p)
                break;
            T /= p;
        }

        // Continuacao por BSDF (um lobe).
        FPTBsdfSample bs = PTSampleBsdf(s, V, seed, gb);
        if (!bs.Valid)
            break;

        RayDesc ray;
        ray.Origin    = s.Pos + s.N * max(TraceParams.w, 1e-3f);
        ray.Direction = bs.Dir;
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;
        RayQuery<RAY_FLAG_NONE> q; // sem cull: folhagem two-sided e interiores dependem de backface
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
        SMILE_RT_PROCEED(q)

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            L += T * bs.Weight * ShadeSky(bs.Dir, normalize(SunDirIntensity.xyz), TraceParams.z);
            break;
        }

        float hitT = q.CommittedRayT();
        FPTSurface h = PTFetchHitSurface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                         q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                         ray.Origin, ray.Direction, hitT, PathParams.w);
        T *= bs.Weight;
        L += T * h.Emissive;
        if (bounce == 0)
            outFirstDist = hitT;

        if (bounce + 1 == maxBounces) {
            // Ultimo vertice: sol via NEE + cauda multi-bounce via DDGI.
            L += T * (PT_SunNEE(h, -bs.Dir, seed, gb + 1) + PT_DDGITail(h));
            break;
        }

        V = -bs.Dir;
        s = h;
    }
    return L;
}

#endif
