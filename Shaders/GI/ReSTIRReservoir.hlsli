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

// === Empacotamento do reservoir (2 texturas) =================================================
//
// O reservoir mora em Res0 (RGBA32F) + Res1 (RGBA32_UINT), 32 B/pixel em vez dos 48 B das quatro
// texturas de antes. Duas mudancas pagam a diferenca:
//   1. x1 SAIU. O ponto visivel e reconstrutivel: no espacial, do proprio depth do vizinho (que o
//      passe ja carrega pro teste de rejeicao); no temporal, do historico de superficie do
//      FTemporalMotionVectors, que guarda exatamente `InvViewProj * (ndc, deviceZ)` do frame
//      anterior — a MESMA conta, com a MESMA matriz (InvViewProjFull), entao o valor e identico
//      ao que era gravado aqui. E o que a RTXDI faz: ela nunca guarda o ponto visivel.
//   2. Lo, n2 e n1 sao empacotados em 32 bits cada, no lugar de meio RGBA16F cada.
//
// M e idade dividem um canal inteiro (16+16). Antes eram um truque de fp32 (idade nos multiplos
// de 1024) que existia justamente por falta de um canal inteiro — o formato UINT o torna obsoleto.
uint ResPackMAge(float M, float age) {
    return min((uint)(M + 0.5f), 0xFFFFu) | (min((uint)max(age, 0.0f), 0xFFFFu) << 16);
}
void ResUnpackMAge(uint packed, out float M, out float age) {
    M   = (float)(packed & 0xFFFFu);
    age = (float)(packed >> 16);
}

// Normal em octaedrico + snorm 16:16. Precisao ANGULAR melhor que a do RGB16F que guardava n2 em
// tres meios floats: fp16 gasta bits em expoente que uma componente em [-1,1] nao usa.
uint ResPackNormal(float3 n) {
    const float2 o = clamp(DDGI_OctEncode(n), -1.0f, 1.0f);
    const int2   q = (int2)round(o * 32767.0f);
    return ((uint)(q.x & 0xFFFF)) | (((uint)(q.y & 0xFFFF)) << 16);
}
float3 ResUnpackNormal(uint p) {
    // Extensao de sinal manual: o campo de 16 bits e snorm.
    const int2 q = int2((int)(p << 16) >> 16, (int)p >> 16);
    return DDGI_OctDecode(clamp(float2(q) * (1.0f / 32767.0f), -1.0f, 1.0f));
}

// Radiancia em RGB9E5 (mantissa 9 bits por canal + expoente 5 bits COMPARTILHADO) — o mesmo
// formato do DXGI_FORMAT_R9G9B9E5_SHAREDEXP, empacotado na mao porque o canal e um uint generico.
// A RTXDI resolve o mesmo problema com LogLuv; RGB9E5 trata os tres canais igual, o que evita
// desvio de matiz no indireto. Faixa util: ~3e-5 ate 65408, de sobra pro Lo pos-firefly-clamp.
static const float kResRadianceMax = 65408.0f;

uint ResPackRadiance(float3 c) {
    c = clamp(c, 0.0f, kResRadianceMax);
    const float maxc = max(c.r, max(c.g, c.b));
    int e = (maxc > 1e-16f) ? ((int)floor(log2(maxc)) + 1) : -15;
    e = clamp(e, -15, 16);
    float denom = exp2((float)(e - 9));
    // O arredondamento pode empurrar a mantissa do canal dominante p/ 512 (estouro do campo de
    // 9 bits). Sobe o expoente uma vez em vez de saturar — saturar escureceria o pixel.
    if ((int)floor(maxc / denom + 0.5f) >= 512) { e = min(e + 1, 16); denom = exp2((float)(e - 9)); }
    const uint3 q = (uint3)clamp(floor(c / denom + 0.5f), 0.0f, 511.0f);
    return q.x | (q.y << 9) | (q.z << 18) | ((uint)(e + 15) << 27);
}

float3 ResUnpackRadiance(uint p) {
    const uint3 q = uint3(p & 0x1FFu, (p >> 9) & 0x1FFu, (p >> 18) & 0x1FFu);
    const int   e = (int)(p >> 27) - 15;
    return float3(q) * exp2((float)(e - 9));
}

// Res1 completo: n2 | Lo | M+idade | n1. Res0 leva x2.xyz e W em fp32 (x2 precisa da faixa
// inteira — e uma posicao de mundo, e a RTXDI tambem a guarda em fp32).
uint4 ResPack1(Reservoir r, float age, float3 n1) {
    return uint4(ResPackNormal(r.n2), ResPackRadiance(r.Lo),
                 ResPackMAge(r.M, age), ResPackNormal(n1));
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
