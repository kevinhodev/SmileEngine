#ifndef SMILE_RESTIR_RESERVOIR_HLSLI
#define SMILE_RESTIR_RESERVOIR_HLSLI

// Reservoir do ReSTIR GI (Ouyang et al. 2021) + helpers de WRS, compartilhado pelo Pass A (trace +
// temporal) e Pass B (spatial). A amostra e o ponto secundario (x2, n2) e sua radiancia (Lo).

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979f
#endif

float ReSTIR_Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// RNG por pixel com estado, p/ a selecao estocastica do WRS. O `effect` separa o stream do WRS
// do stream que gera a direcao do raio (e dos outros efeitos) — ver GGXSample.hlsli.
uint  RngSeed(uint2 px, uint frame, uint effect) { return GGX_SeedE(px, frame, effect); }
float RngNext(inout uint s) { s = GGX_PCG(s); return (s & 0x00FFFFFFu) / 16777216.0f; }

struct Reservoir {
    float3 x1;   // ponto visivel (validacao temporal/espacial)
    float3 x2;   // ponto da amostra (hit)
    float3 n2;   // normal no ponto da amostra (Jacobiano de reconexao)
    float3 Lo;   // radiancia em x2 na direcao de x1
    float  M;    // contagem de amostras
    float  W;    // peso de contribuicao nao-enviesado
    float  wSum; // soma dos pesos de resampling
};

void ResInit(out Reservoir r) {
    r.x1 = 0; r.x2 = 0; r.n2 = 0; r.Lo = 0; r.M = 0; r.W = 0; r.wSum = 0;
}

// pHat alvo p/ o pixel (x1,n1) dada a amostra (x2,Lo): luminancia*cosTheta1 (albedo cancela).
float TargetPHat(float3 x1, float3 n1, float3 x2, float3 Lo) {
    float3 d = x2 - x1;
    float  l = length(d);
    if (l < 1e-4f) return 0.0f;
    float cosT = saturate(dot(n1, d / l));
    return ReSTIR_Luminance(Lo) * cosT;
}

// Pack/unpack de M + idade da amostra no MESMO canal (ResA.w, fp32): M fica abaixo de 1024 e a
// idade (inteira) vive nos multiplos de 1024 — exato em fp32 ate idade ~4000. A idade conta ha
// quantos frames a amostra SELECIONADA sobrevive no reservoir (nao e o M, que o MCap limita):
// sem expiracao, amostra brilhante rara travada num bolsao escuro vira mancha persistente.
// Historico antigo (so M) decodifica idade 0 — migracao gratis.
float ResPackMAge(float M, float age) {
    return min(M, 1000.0f) + min(floor(age), 4000.0f) * 1024.0f;
}
void ResUnpackMAge(float packed, out float M, out float age) {
    age = floor(packed / 1024.0f);
    M   = packed - age * 1024.0f;
}

// Adiciona um candidato (M=1) com peso de resampling w. Retorna true se adotado.
bool ResUpdate(inout Reservoir r, float3 x2c, float3 n2c, float3 Loc, float w, inout uint rng) {
    r.wSum += w;
    r.M    += 1.0f;
    if (w > 0.0f && RngNext(rng) * r.wSum <= w) {
        r.x2 = x2c; r.n2 = n2c; r.Lo = Loc;
        return true;
    }
    return false;
}

// Funde um reservoir inteiro (other) no atual. pHatOther = pHat (do pixel atual) p/ a amostra de
// other; J = Jacobiano de reconexao (calculado no temporal E no espacial; o caller rejeita J
// extremo). Retorna true se a amostra de other foi adotada (o espacial rastreia o dominio
// vencedor p/ o peso MIS da correcao de bias).
bool ResMerge(inout Reservoir r, Reservoir other, float pHatOther, float J, inout uint rng) {
    float w = pHatOther * other.W * other.M * J;
    r.wSum += w;
    r.M    += other.M;
    if (w > 0.0f && RngNext(rng) * r.wSum <= w) {
        r.x2 = other.x2; r.n2 = other.n2; r.Lo = other.Lo;
        return true;
    }
    return false;
}

// Jacobiano de reconexao (Ouyang 2021): reusar a amostra x2 (normal n2) de um pixel visivel x1Src
// no pixel x1Dst. J = (cosPhiDst/cosPhiSrc) * (|x2-x1Src|^2 / |x2-x1Dst|^2). Clampado pelo caller.
float ReconnectionJacobian(float3 x1Dst, float3 x1Src, float3 x2, float3 n2) {
    float3 dDst = x2 - x1Dst; float lDst2 = dot(dDst, dDst);
    float3 dSrc = x2 - x1Src; float lSrc2 = dot(dSrc, dSrc);
    if (lDst2 < 1e-8f || lSrc2 < 1e-8f) return 0.0f;
    float lDst = sqrt(lDst2), lSrc = sqrt(lSrc2);
    float cosDst = abs(dot(n2, -dDst / lDst));
    float cosSrc = abs(dot(n2, -dSrc / lSrc));
    if (cosSrc < 1e-4f) return 0.0f;
    return (cosDst / cosSrc) * (lSrc2 / lDst2);
}

// Finaliza W = wSum / (M * pHat(selecionado)).
void ResFinalizeW(inout Reservoir r, float3 x1, float3 n1) {
    float pHatSel = TargetPHat(x1, n1, r.x2, r.Lo);
    r.W = (pHatSel > 0.0f && r.M > 0.0f) ? (r.wSum / (r.M * pHatSel)) : 0.0f;
}

// Resolve a irradiancia: gi = Lo * cosTheta1 * W / pi (= (1/pi) * E). maxLuma > 0 aplica firefly
// clamp na SAIDA (o brilho vem de wSum/M·W, nao de Lo — clampar Lo so normaliza a cor).
float3 ResResolve(Reservoir r, float3 x1, float3 n1, float maxLuma) {
    float3 d = r.x2 - x1;
    float  l = length(d);
    float  cosSel = (l > 1e-4f) ? saturate(dot(n1, d / l)) : 0.0f;
    float3 gi = r.Lo * cosSel * r.W / SMILE_PI;
    float  gl = ReSTIR_Luminance(gi);
    if (maxLuma > 0.0f && gl > maxLuma) gi *= maxLuma / gl;
    return gi;
}

#endif
