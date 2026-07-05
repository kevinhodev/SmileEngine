#ifndef SMILE_PT_SHIFT_HLSLI
#define SMILE_PT_SHIFT_HLSLI

// ReSTIR PT — F2b: random replay + hybrid shift (GRIS §8 / Enhanced §4).
//
// Parametrizacao HIBRIDA do caminho: os kb bounces ANTES do vertice de reconexao vivem em PSS
// (random replay: o shift copia as dims => Jacobiano 1), e o vertice de reconexao x_k vive em
// solid angle (Jacobiano de reconexao no shift). Divergencia deliberada do paper: o replay e PSS
// puro (nao forca os lobes do fonte — a dim LOBE re-decide na superficie destino); o paper força
// lobes p/ reduzir variancia, nao por correcao. Pairwise MIS fica p/ o F3 (aqui 1/M).
//
// PARTICAO da luz (mantem o estimador correto): o reservoir guarda so a CONTINUACAO do caminho
// (C = T_prefixo * f~_recon * Lo). A luz coletada NOS vertices do prefixo (emissivo + NEE) sai
// do reservoir e e somada DIRETO no frame (Cemit) — porque o peso RIS divide a contribuicao pelo
// pdf da reconexao, e o prefixo (ja pdf-dividido em T via bs.Weight) nao pode ser dividido de
// novo. Bonus: o replay do merge dispensa shadow rays (so BSDF sample + trace por bounce).
// Caminho sem NENHUM vertice conectavel = inteiro em Cemit (1spp por frame, fora do reservoir).
//
// Connectability por segmento x_j -> x_{j+1} (GRIS §7.5 + footprint Enhanced §4):
//   (lobe difuso OU rough(x_j) >= ShiftParams.y) E |seg| >= ShiftParams.x * dist(camera, x_j).
//
// Requer (via includes do host): PTCommon, PTRng, PTRayUtil, PTMaterial, PTPathTrace,
// PTReservoir, HitShading (ShadeSky/SMILE_RT_PROCEED) e os globals Scene/Instances/....

// Resultado do walk do candidato (sampling inicial).
struct FPTCandidate {
    bool   HasSample;  // caminho tem vertice de reconexao => vai p/ o reservoir
    uint   kb;         // bounces replayados antes da reconexao (0 = reconecta em x2)
    float3 xkm1;       // base da reconexao x_{k-1} (= x1 quando kb == 0)
    float3 xk;         // vertice de reconexao
    float3 nk;         // normal em xk
    float3 Lo;         // radiancia saindo de xk (sufixo completo, firefly-clampada por canal)
    float3 Crecon;     // T_pre * f~_recon * Lo — contribuicao do sample NESTE pixel
    float3 CreconDiff; // F4: parcela DIFUSA de Crecon (pelo lobe do 1o segmento; split em kb==0)
    float  pdfRecon;   // pdf solid-angle da direcao de reconexao em x_{k-1} (roughness REAL)
    float  hit1;       // |x2 - x1| (hitDist p/ o NRD)
    float3 Cemit;      // luz do prefixo + caminhos nao-conectaveis (soma direta no frame)
};

FPTCandidate PT_TraceCandidate(FPTSurface s1, float3 V, uint seed) {
    FPTCandidate o;
    o.HasSample = false; o.kb = 0;
    o.xkm1 = s1.Pos; o.xk = float3(0.0f, 0.0f, 0.0f); o.nk = float3(0.0f, 0.0f, 0.0f);
    o.Lo = float3(0.0f, 0.0f, 0.0f); o.Crecon = float3(0.0f, 0.0f, 0.0f);
    o.CreconDiff = float3(0.0f, 0.0f, 0.0f);
    o.pdfRecon = 0.0f; o.hit1 = 0.0f; o.Cemit = float3(0.0f, 0.0f, 0.0f);

    uint  maxBounces = max((uint)PathParams.x, 1u);
    float mx = PathParams.z;

    float3 T = float3(1.0f, 1.0f, 1.0f); // throughput do prefixo (f*cos/pdf acumulado)
    FPTSurface s = s1;
    float3 Vv = V;
    uint lobe0 = PT_LOBE_DIFFUSE; // lobe do 1o segmento (classifica diff/spec p/ o NRD, F4)

    [loop]
    for (uint j = 0; j < maxBounces; ++j) {
        // SEM RR na fase de busca: qualquer bounce daqui pode virar prefixo de replay, e o
        // replay nao pode depender de decisao baseada em throughput (que muda no destino).
        FPTBsdfSample bs = PTSampleBsdf(s, Vv, seed, j);
        if (j == 0)
            lobe0 = bs.Lobe;
        if (!bs.Valid)
            break; // caminho morre; Cemit ja coletado segue p/ o clamp no fim

        RayDesc ray;
        ray.Origin    = s.Pos + s.N * max(TraceParams.w, 1e-3f);
        ray.Direction = bs.Dir;
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
        SMILE_RT_PROCEED(q)

        bool  hit    = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
        float segLen = hit ? q.CommittedRayT() : TraceParams.y;
        if (j == 0)
            o.hit1 = segLen;

        bool connectable = (bs.Lobe == PT_LOBE_DIFFUSE || s.Roughness >= ShiftParams.y)
                        && (segLen > max(ShiftParams.x * length(CameraPos.xyz - s.Pos), 1e-3f));

        if (!hit) {
            float3 skyL = ShadeSky(bs.Dir, normalize(SunDirIntensity.xyz), TraceParams.z);
            if (connectable) {
                // Reconexao no "ponto do ceu" (ponto distante na direcao de escape).
                o.HasSample = true; o.kb = j;
                o.xkm1 = s.Pos;
                o.xk   = ray.Origin + bs.Dir * TraceParams.y;
                o.nk   = -bs.Dir;
                o.Lo   = skyL;
            } else {
                o.Cemit += T * bs.Weight * skyL;
            }
            break;
        }

        FPTSurface h = PTFetchHitSurface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                         q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                         ray.Origin, ray.Direction, segLen, PathParams.w);
        if (connectable) {
            o.HasSample = true; o.kb = j;
            o.xkm1 = s.Pos; o.xk = h.Pos; o.nk = h.N;
            // Sufixo a partir de x_k: dims globais (bounceBase = j+1), RR liberado (nunca
            // replayado). Budget 0 => PT_PathRadiance degenera na cauda NEE+DDGI.
            float dummy;
            o.Lo = h.Emissive + PT_PathRadiance(h, -bs.Dir, seed, maxBounces - (j + 1),
                                                (uint)PathParams.y, true, j + 1, dummy);
            break;
        }

        // Vertice replayavel: a luz local sai do sample e vai p/ Cemit (particao, ver topo).
        T *= bs.Weight;
        o.Cemit += T * h.Emissive;
        o.Cemit += T * PT_SunNEE(h, -bs.Dir, seed, j + 1);
        if (j + 1 == maxBounces) {
            o.Cemit += T * PT_DDGITail(h); // terminal sem reconexao: cauda no cache
            break;
        }
        Vv = -bs.Dir;
        s  = h;
    }

    if (o.HasSample) {
        // Firefly clamp por canal ANTES do reservoir (e do pack fp16).
        float mc = max(o.Lo.r, max(o.Lo.g, o.Lo.b));
        if (mx > 0.0f && mc > mx)
            o.Lo *= mx / mc;

        // pdf e contribuicao na direcao de reconexao CONSISTENTE (origem com bias => a direcao
        // real x_{k-1}->x_k difere de bs.Dir; usar a mesma nos dois evita descasamento pHat/pdf).
        // s/Vv/T ainda sao os de x_{k-1} (o break acontece antes de avancar o vertice).
        float3 wr  = o.xk - s.Pos;
        float  lwr = length(wr);
        if (lwr > 1e-4f) {
            o.pdfRecon = PT_BsdfPdf(s, Vv, wr / lwr);
            float3 fDiff;
            o.Crecon = T * PT_ReconnectContribution(s, Vv, o.xk, o.Lo, fDiff);
            // Classificacao diff/spec pelo 1o SEGMENTO: kb==0 => a reconexao e em x1 e o eval
            // mistura os lobes (usa o split); kb>0 => o lobe amostrado no bounce 0 decide tudo.
            o.CreconDiff = (o.kb == 0) ? T * fDiff
                         : ((lobe0 == PT_LOBE_DIFFUSE) ? o.Crecon : float3(0.0f, 0.0f, 0.0f));
        }
        if (o.pdfRecon <= 1e-5f)
            o.HasSample = false; // reconexao degenerada: descarta (raro; documentado)
    }

    // Clamp do Cemit por canal (T especular encadeado pode ter outlier).
    float mcE = max(o.Cemit.r, max(o.Cemit.g, o.Cemit.b));
    if (mx > 0.0f && mcE > mx)
        o.Cemit *= mx / mcE;

    return o;
}

// Resultado do hybrid shift de uma amostra de reservoir p/ o pixel atual.
struct FPTShift {
    bool   Ok;    // shift definido (replay reproduziu o prefixo + reconexao valida)
    float3 C;     // T'_pre * f~_recon * Lo no pixel destino (contribuicao/pHat)
    float3 Cdiff; // F4: parcela DIFUSA de C (lobe do 1o segmento; split quando kb==0)
    float3 xkm1;  // base replayada no destino (re-ancoragem do reservoir)
    float  hit1;  // |x2' - x1| no destino
    float  J;     // Jacobiano de reconexao (base fonte -> base destino)
};

// forceVis: o temporal reprojeta a MESMA superficie (kb==0 dispensa o visibility ray); o
// espacial (F3) reconecta a partir de um x1 de OUTRO pixel — sem o raio haveria light leak.
FPTShift PT_HybridShift(FPTSurface s1, float3 V, PTReservoir smp, bool forceVis) {
    FPTShift o;
    o.Ok = false; o.C = float3(0.0f, 0.0f, 0.0f); o.Cdiff = float3(0.0f, 0.0f, 0.0f);
    o.xkm1 = s1.Pos; o.hit1 = 0.0f; o.J = 0.0f;

    float3 T = float3(1.0f, 1.0f, 1.0f);
    FPTSurface s = s1;
    float3 Vv = V;
    uint lobe0 = PT_LOBE_DIFFUSE; // lobe do 1o segmento replayado (classificacao NRD, F4)

    // Random replay do prefixo: MESMO seed do fonte, mesmas dims globais. Sem NEE (a luz do
    // prefixo nao pertence ao sample — Cemit e por-frame) e sem RR (dim fora da parametrizacao).
    [loop]
    for (uint j = 0; j < smp.kb; ++j) {
        FPTBsdfSample bs = PTSampleBsdf(s, Vv, smp.seed, j);
        if (j == 0)
            lobe0 = bs.Lobe;
        if (!bs.Valid)
            return o;

        RayDesc ray;
        ray.Origin    = s.Pos + s.N * max(TraceParams.w, 1e-3f);
        ray.Direction = bs.Dir;
        ray.TMin      = 0.0f;
        ray.TMax      = TraceParams.y;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
        SMILE_RT_PROCEED(q)

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return o; // fonte tinha superficie neste bounce; destino escapou => shift indefinido

        float hitT = q.CommittedRayT();
        FPTSurface h = PTFetchHitSurface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                         q.CommittedTriangleBarycentrics(), q.CommittedWorldToObject3x4(),
                                         ray.Origin, ray.Direction, hitT, PathParams.w);
        if (j == 0)
            o.hit1 = hitT;
        T *= bs.Weight;
        Vv = -bs.Dir;
        s  = h;
    }

    // Reconexao x_{k-1}' -> xk, com o MESMO criterio de footprint do sampling inicial.
    float3 d = smp.xk - s.Pos;
    float  l = length(d);
    if (l < max(ShiftParams.x * length(CameraPos.xyz - s.Pos), 1e-3f))
        return o;
    if (smp.kb == 0)
        o.hit1 = l;

    if (smp.kb > 0 || forceVis) {
        // A base moveu de verdade (replay ou vizinho espacial): visibility ray da reconexao
        // evita light leak. kb==0 temporal e reproject de mesma superficie — dispensa (F1/F2a).
        float3 dir  = d / l;
        float  ndl  = dot(s.N, dir);
        float3 side = (ndl >= 0.0f) ? s.N : -s.N;
        if (PTShadowRayVis(s.Pos + side * max(TraceParams.w, 1e-3f), dir, l * 0.99f) <= 0.0f)
            return o;
    }

    float3 fDiff;
    o.C     = T * PT_ReconnectContribution(s, Vv, smp.xk, smp.Lo, fDiff);
    o.Cdiff = (smp.kb == 0) ? T * fDiff
            : ((lobe0 == PT_LOBE_DIFFUSE) ? o.C : float3(0.0f, 0.0f, 0.0f));
    o.J    = PTReconnectionJacobian(s.Pos, smp.xkm1, smp.xk, smp.nk);
    o.xkm1 = s.Pos;
    o.Ok   = true;
    return o;
}

#endif
