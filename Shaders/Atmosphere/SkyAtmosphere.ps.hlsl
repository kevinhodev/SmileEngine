#include "AtmosphereCommon.hlsli"

Texture2D<float4> SkyViewLUT       : register(t0);
Texture2D<float4> TransmittanceLUT : register(t1);
Texture2D<float4> MoonTex          : register(t2);

struct PSInput {
    float4 pos    : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

float3 Hash33(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float3 StarField(float3 dir, float time) {
    // Rotacao sideral em torno do polo celeste (StarAxis.xyz = polo pela latitude do TOD),
    // angulo StarAxis.w dirigido pelo relogio do TOD — pausa/acelera junto com sol e lua.
    float3 axis = StarAxis.xyz;
    float  a    = StarAxis.w;
    float  c    = cos(a), s = sin(a);
    float3 d = dir * c + cross(axis, dir) * s + axis * dot(axis, dir) * (1.0f - c);

    const float density = 200.0f;
    float3 cell   = floor(d * density);
    float3 rnd    = Hash33(cell);
    float  present = step(0.992f, rnd.x);
    // Jitter <= 0.2 celula: com raio visivel ~0.28 celula a estrela nao cruza a borda da
    // celula vizinha (0.8 deixava o centro cair fora da propria celula -> estrela cortada).
    float3 starDir = normalize(cell + 0.5f + (rnd - 0.5f) * 0.4f);

    float3 delta = starDir - d;
    // Derivadas estao em pixels da resolucao INTERNA. Converte para pixels da saida antes
    // de definir sigma, mantendo o fallback com ~0.65px em qualquer render scale/FOV.
    float3 dOutDx = ddx(d) * kRenderToOutputX;
    float3 dOutDy = ddy(d) * kRenderToOutputY;
    float outputPixelAngle2 = max(0.5f * (dot(dOutDx, dOutDx) + dot(dOutDy, dOutDy)),
                                      1e-12f);
    const float sigmaPx = 0.65f;
    float sigma2 = outputPixelAngle2 * sigmaPx * sigmaPx;
    float core = exp(-dot(delta, delta) / (2.0f * sigma2));
    float  bright = 0.15f + 0.45f * rnd.y;
    float  twinkle = 0.65f + 0.35f * sin(time * 3.0f + rnd.z * 6.2831853f);
    float3 tint = lerp(float3(1.0f, 0.82f, 0.65f), float3(0.7f, 0.8f, 1.0f), rnd.z);
    return present * core * bright * twinkle * tint;
}

// Polo norte lunar medio no frame equatorial ICRF/J2000 usado pelo catalogo de estrelas:
// RA=269.9949, Dec=66.5392 graus (NAIF pck00011.tpc, BODY301_POLE_RA/DEC). A matriz sideral
// leva esse vetor ao mundo. Nao inclui libracao/nutacao fina: o TOD ainda usa uma orbita lunar
// simplificada, mas o roll deixa de ser preso ao horizonte e passa a acompanhar o ceu.
static const float3 kMoonNorthPoleJ2000 =
    float3(-0.0000354375f, 0.917332672f, -0.398121550f);

// Lunar-Lambert simplificado: 90% do single scattering de Lommel-Seeliger e 10% Lambert
// representando o multiple scattering que o modelo puro omite. O fator 2 normaliza LS para
// resposta 1 no centro da Lua cheia (mu0=mu=1), preservando a semantica de brilho do disco.
static const float kMoonLommelSeeligerWeight = 0.90f;

// Shadow-Hiding Opposition Effect de Hapke. hS=0.055 fica entre os valores tipicos medidos
// pelo LROC para maria (~0.050) e planaltos (~0.074). A amplitude e deliberadamente discreta:
// o JPEG ja e uma composicao fotometricamente normalizada e nao traz o mapa B_S0 por texel.
static const float kMoonOppositionAmplitude = 0.14f;
static const float kMoonOppositionWidth     = 0.055f;

float MoonPhotometricResponse(float mu0, float mu, float cosPhase) {
    float ls = 2.0f * mu0 / max(mu0 + mu, 1e-5f);

    // tan(g/2) sem acos: estavel tanto em g=0 (Lua cheia) quanto proximo de pi (Lua nova).
    float tanHalfPhase = sqrt(max((1.0f - cosPhase) / max(1.0f + cosPhase, 1e-5f), 0.0f));
    float opposition = kMoonOppositionAmplitude /
                       (1.0f + tanHalfPhase / kMoonOppositionWidth);
    float singleScatter = ls * (1.0f + opposition);
    return lerp(mu0, singleScatter, kMoonLommelSeeligerWeight);
}

float3 MoonDisk(float3 viewDir, float3 moonDir, float cosRadius, float3 sunDir, float brightness,
                out float coverage) {
    float3 moonDirN  = normalize(moonDir);
    float3 moonNorth = normalize(mul(kMoonNorthPoleJ2000, (float3x3)StarMatrix));
    float3 northProj = moonNorth - moonDirN * dot(moonNorth, moonDirN);

    // A direcao do polo projetada so degenera se a camera olhar ao longo do eixo lunar. O
    // fallback pelo polo celeste preserva a continuidade nesse caso artificial; o ultimo eixo
    // cobre tambem os polos geograficos do TOD sem normalizar um vetor quase nulo.
    if (dot(northProj, northProj) < 1e-8f) {
        float3 celestialNorth = normalize(StarAxis.xyz);
        northProj = celestialNorth - moonDirN * dot(celestialNorth, moonDirN);
    }
    if (dot(northProj, northProj) < 1e-8f) {
        float3 fallback = abs(moonDirN.y) < 0.99f
                        ? float3(0.0f, 1.0f, 0.0f)
                        : float3(1.0f, 0.0f, 0.0f);
        northProj = fallback - moonDirN * dot(fallback, moonDirN);
    }

    float3 up0   = normalize(northProj);
    float3 right = normalize(cross(up0, moonDirN));
    float3 up    = cross(moonDirN, right);

    float  rad = sqrt(max(1.0f - cosRadius * cosRadius, 1e-6f));
    float2 uv  = float2(dot(viewDir, right), dot(viewDir, up)) / rad;
    float  rr  = dot(uv, uv);

    // Cobertura analitica de aproximadamente um pixel. Os returns antigos aconteciam na borda
    // geometrica antes do smoothstep e descartavam a metade externa do filtro; a largura fixa
    // de 8% do raio tambem deixava luas grandes borradas e luas pequenas serrilhadas.
    float edgeAA = max(0.5f * fwidth(rr), 1e-6f);
    float edge   = 1.0f - smoothstep(1.0f - edgeAA, 1.0f + edgeAA, rr);
    coverage = edge;

    // O filtro do limbo cobre meio pixel fora da esfera. Avalie a fotometria nesse trecho no
    // limite INTERNO da superficie; usar z=0 fora fazia o LS saltar de 1 para 0 na Lua cheia e
    // anulava a metade externa do antialias.
    float  surfaceRr = min(rr, 1.0f - 1e-6f);
    float  surfaceScale = sqrt(surfaceRr / max(rr, 1e-8f));
    float2 surfaceUv = uv * surfaceScale;
    float  z      = sqrt(max(1.0f - surfaceRr, 0.0f));
    float3 fwd    = -moonDirN;
    float3 normal = normalize(right * surfaceUv.x + up * surfaceUv.y + fwd * z);

    float  nx  = dot(normal, right), ny = dot(normal, up), nz = dot(normal, fwd);
    float2 muv = float2(0.5f + atan2(nx, nz) / (2.0f * PI),
                        0.5f - asin(clamp(ny, -1.0f, 1.0f)) / PI);
    float ndl = dot(normal, sunDir);

    // Todas as derivadas sao calculadas antes do early-out para permanecerem validas no quad
    // da borda. A Lua nao tem atmosfera: o terminador e duro, mas ainda recebe cobertura de
    // aproximadamente um pixel em qualquer fase/tamanho.
    // A textura e um SRV sRGB: o hardware lineariza e escolhe a mip pelo footprint do disco.
    float2 muvDx = ddx(muv);
    float2 muvDy = ddy(muv);
    float terminatorAA = max(0.5f * fwidth(ndl), 1e-6f);
    float terminator = smoothstep(-terminatorAA, terminatorAA, ndl);

    float mu0 = max(ndl, 0.0f); // cos(incidencia): normal -> Sol
    float mu  = max(dot(normal, fwd), 0.0f); // cos(emissao): normal -> observador
    float cosPhase = clamp(dot(sunDir, fwd), -1.0f, 1.0f);
    float photometric = MoonPhotometricResponse(mu0, mu, cosPhase) * terminator;

    if (edge <= 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float3 albedo = MoonTex.SampleGrad(LinearClampSampler, muv, muvDx, muvDy).rgb;
    // Contraste dos mares reforcado: o color map LROC e raso e o tonemap ainda comprime.
    albedo = max((float3)0.0f, 0.13f + (albedo - 0.13f) * 1.8f);

    // Sem earthshine: a olho nu a parte escura e invisivel contra o ceu; ela continua tapando
    // as estrelas atras (coverage cobre o disco inteiro), entao vira silhueta como na vida real.
    return photometric * edge * brightness * albedo;
}

float4 main(PSInput input) : SV_TARGET {
    float4 worldFar = mul(float4(input.clipXY, 1.0f, 1.0f), InvViewProjNoTrans);
    float3 viewDir  = normalize(worldFar.xyz);

    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 sunDir   = normalize(SunDir.xyz);

    float viewZenithCos = dot(viewDir, up);

    float3 viewHoriz = viewDir - up * viewZenithCos;
    float3 sunHoriz  = sunDir  - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));

    float2 uv = SkyViewParamsToUv(viewZenithCos, lightViewCos, kViewHeight);
    float3 L  = SkyViewLUT.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;

    float  cosToSun = dot(viewDir, sunDir);
    // Transmitancia do disco ao longo do raio de VISTA + sombra analitica do planeta (UE
    // GetAtmosphereTransmittance): corta o disco no horizonte virtual e mata o disco fantasma
    // com o sol logo abaixo do horizonte (a borda do LUT nunca chega a zero de proposito).
    float3 camPos   = float3(0.0f, kViewHeight, 0.0f);
    float3 sunTrans = (RaySphereNearest(camPos, viewDir, kBottomR) > 0.0f)
                    ? (float3)0.0f
                    : SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, viewZenithCos);

    float core = smoothstep(kSunDiskCos, lerp(kSunDiskCos, 1.0f, 0.5f), cosToSun);
    float glare = pow(saturate(cosToSun), 350.0f);

    L += sunTrans * (kSunDiskInt * core + kSunGlareInt * glare);

    float night        = MoonParams.z;
    float aboveHorizon = smoothstep(-0.02f, 0.06f, viewDir.y);
    float moonCover    = 0.0f;

    // Lua SEM gate de noite: de dia o ceu claro a lava naturalmente (aditivo sobre o LUT),
    // como na vida real e na Cry. So estrelas e corona sao coisas de noite.
    if (MoonParams.x > 0.0f) {
        float3 viewTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, viewZenithCos);
        float3 moon = MoonDisk(viewDir, MoonDir.xyz, MoonDir.w, sunDir, MoonParams.x, moonCover);

        // Corona (inner + outer, estilo Cry): halo do luar em volta do disco. Tambem carrega o
        // lobo direcional do ceu de luar — o sky-view LUT usa fase uniforme p/ a lua de
        // proposito (a dobra azimutal espelharia o lobo).
        // Aureole apertado (~0.75 grau, fracao do brilho do disco) + halo largo (~2.7 graus) na
        // ordem do brilho do ceu de luar — a versao forte (pow200*0.10) virava uma bola gigante.
        // Centro no LIMBO ILUMINADO: o halo real e luz do lado claro espalhada no caminho, entao
        // abraca a banda clara; desloca com a fase (cheia = centrado, crescente fina = ~0.7 raio).
        float3 moonDirN = normalize(MoonDir.xyz);
        float3 litTan   = sunDir - moonDirN * dot(sunDir, moonDirN); // dir do limbo claro no ceu
        float  litLen   = length(litTan);
        float  sinRad   = sqrt(saturate(1.0f - MoonDir.w * MoonDir.w)); // raio angular do disco
        float  shift    = sinRad * 0.7f * (1.0f - saturate(kMoonCorona));
        float3 coronaDir = normalize(moonDirN +
                                     (litLen > 1e-4f ? litTan / litLen : (float3)0.0f) * shift);
        float  cosToMoon = saturate(dot(viewDir, coronaDir));
        float  corona    = (pow(cosToMoon, 8000.0f) * 0.35f + pow(cosToMoon, 600.0f) * 0.025f)
                         * kMoonCorona * night;
        // Atenua a corona DENTRO do disco de forma radial: 1.0 na borda (continua com o glow de
        // fora, sem degrau) caindo a ~30% no centro — protege a textura dos mares sem criar o
        // "buraco preto" que a mascara uniforme por coverage criava na parte escura.
        float  cosDisk  = dot(viewDir, moonDirN);
        float  interior = saturate((cosDisk - MoonDir.w) / max(1.0f - MoonDir.w, 1e-6f));
        corona *= 1.0f - 0.7f * sqrt(interior);

        L += (moon + (float3)corona) * viewTrans * aboveHorizon;
    }

    // Hash procedural so como FALLBACK: com o catalogo real carregado (Assets/Sky/stars.sstars)
    // as estrelas viram um passe proprio (StarField.vs/ps) depois do ceu.
    if (night > 0.001f && kStarCatalogOn < 0.5f) {
        float3 viewTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, viewZenithCos);
        // Mascara pela cobertura do disco: sem ela as estrelas vazam pela parte escura da lua.
        float3 stars = StarField(viewDir, MoonParams.w) * MoonParams.y * (1.0f - moonCover);
        L += stars * viewTrans * (night * aboveHorizon);
    }

    return float4(L, 1.0f);
}
