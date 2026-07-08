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
    // Rotacao diurna em torno do polo celeste (StarAxis.xyz = polo pela latitude do TOD),
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
    float  core  = exp(-dot(delta, delta) * 1.2e6f);  
    float  bright = 0.15f + 0.45f * rnd.y;            
    float  twinkle = 0.65f + 0.35f * sin(time * 3.0f + rnd.z * 6.2831853f);
    float3 tint = lerp(float3(1.0f, 0.82f, 0.65f), float3(0.7f, 0.8f, 1.0f), rnd.z);
    return present * core * bright * twinkle * tint;
}

float3 MoonDisk(float3 viewDir, float3 moonDir, float cosRadius, float3 sunDir, float brightness,
                out float coverage) {
    coverage = 0.0f;
    float cosToMoon = dot(viewDir, moonDir);
    if (cosToMoon <= cosRadius) return float3(0.0f, 0.0f, 0.0f);

    float3 up0   = abs(moonDir.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up0, moonDir));
    float3 up    = cross(moonDir, right);

    float  rad = sqrt(max(1.0f - cosRadius * cosRadius, 1e-6f)); 
    float2 uv  = float2(dot(viewDir, right), dot(viewDir, up)) / rad; 
    float  rr  = dot(uv, uv);
    if (rr > 1.0f) return float3(0.0f, 0.0f, 0.0f);

    float  z      = sqrt(max(1.0f - rr, 0.0f));
    float3 fwd    = -moonDir;                            
    float3 normal = normalize(right * uv.x + up * uv.y + fwd * z);

    float  nx  = dot(normal, right), ny = dot(normal, up), nz = dot(normal, fwd);
    float2 muv = float2(0.5f + atan2(nx, nz) / (2.0f * PI),
                        0.5f - asin(clamp(ny, -1.0f, 1.0f)) / PI);
    float3 albedo = MoonTex.SampleLevel(LinearClampSampler, muv, 0.0f).rgb;
    albedo = pow(max(albedo, 0.0f), 2.2f);
    // Contraste dos mares reforcado: o color map LROC e raso e o tonemap ainda comprime.
    albedo = max((float3)0.0f, 0.13f + (albedo - 0.13f) * 1.8f);

    float  ndl        = dot(normal, sunDir);
    // Terminator estreito: a lua real tem transicao dia/noite dura (sem atmosfera); a banda
    // antiga (-0.05..0.18) borrava as fases. Disco iluminado ~uniforme = Lommel-Seeliger, ok.
    float  lit        = smoothstep(-0.015f, 0.04f, ndl);
    float  edge = smoothstep(1.0f, 0.92f, rr);
    coverage = edge;
    // Sem earthshine: a olho nu a parte escura e invisivel contra o ceu; ela continua tapando
    // as estrelas atras (coverage cobre o disco inteiro), entao vira silhueta como na vida real.
    return lit * edge * brightness * albedo;
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

    if (night > 0.001f) {
        float3 viewTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, viewZenithCos);
        // Mascara pela cobertura do disco: sem ela as estrelas vazam pela parte escura da lua.
        float3 stars = StarField(viewDir, MoonParams.w) * MoonParams.y * (1.0f - moonCover);
        L += stars * viewTrans * (night * aboveHorizon);
    }

    return float4(L, 1.0f);
}