// Atmosphere sky pass: samples the sky-view LUT by world view direction and adds
// an analytic sun disk attenuated by the transmittance LUT at the camera.

#include "AtmosphereCommon.hlsli"

Texture2D<float4> SkyViewLUT       : register(t0);
Texture2D<float4> TransmittanceLUT : register(t1);
Texture2D<float4> MoonTex          : register(t2); // albedo equiretangular da lua (branca = procedural)

struct PSInput {
    float4 pos    : SV_POSITION;
    float2 clipXY : TEXCOORD0;
};

// --- Estrelas procedurais (sem catalogo) -----------------------------------
// Hash 3D barato (Dave Hoskins). Uma estrela candidata por celula da direcao celeste;
// celulas esparsas viram estrela, com brilho/cor/cintilacao por hash. Sem dependencia de
// dados (vs. o star-mesh do catalogo da Cry) — upgrade p/ Bright Star Catalog fica p/ depois.
float3 Hash33(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Brilho das estrelas na direcao 'dir' (rgb), com rotacao celeste lenta e cintilacao.
float3 StarField(float3 dir, float time) {
    // Giro celeste: estrelas "rodam" devagar em torno do eixo do mundo.
    float a = time * 0.004f;
    float c = cos(a), s = sin(a);
    float3 d = float3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

    const float density = 200.0f;
    float3 cell   = floor(d * density);
    float3 rnd    = Hash33(cell);
    float  present = step(0.992f, rnd.x);              // so ~0.8% das celulas viram estrela (esparso)
    float3 starDir = normalize(cell + 0.5f + (rnd - 0.5f) * 0.8f); // centro da estrela na celula

    float3 delta = starDir - d;
    float  core  = exp(-dot(delta, delta) * 1.2e6f);  // ponto bem fechado e pequeno (~1px)
    float  bright = 0.15f + 0.45f * rnd.y;            // bem mais fraco
    float  twinkle = 0.65f + 0.35f * sin(time * 3.0f + rnd.z * 6.2831853f);
    // Cor da estrela: quentes (rnd baixo) a azuladas (rnd alto), tenue.
    float3 tint = lerp(float3(1.0f, 0.82f, 0.65f), float3(0.7f, 0.8f, 1.0f), rnd.z);
    return present * core * bright * twinkle * tint;
}

// --- Disco da lua (com fase) ------------------------------------------------
// Reconstroi uma esfera no disco e ilumina pelo sol (terminador = fase crescente/cheia),
// com um leve earthshine no lado escuro. moonDir.w = cos(raio angular).
float3 MoonDisk(float3 viewDir, float3 moonDir, float cosRadius, float3 sunDir, float brightness) {
    float cosToMoon = dot(viewDir, moonDir);
    if (cosToMoon <= cosRadius) return float3(0.0f, 0.0f, 0.0f);

    // Base tangente em torno da direcao da lua.
    float3 up0   = abs(moonDir.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up0, moonDir));
    float3 up    = cross(moonDir, right);

    float  rad = sqrt(max(1.0f - cosRadius * cosRadius, 1e-6f)); // sin(raio angular) = meia-extensao
    float2 uv  = float2(dot(viewDir, right), dot(viewDir, up)) / rad; // disco -> circulo unitario
    float  rr  = dot(uv, uv);
    if (rr > 1.0f) return float3(0.0f, 0.0f, 0.0f);

    // Normal da esfera no ponto do disco, apontando PARA a camera (hemisferio visivel = -moonDir).
    // (Com +moonDir a normal apontava pro lado oposto -> fase invertida: "cheia" saia escura.)
    float  z      = sqrt(max(1.0f - rr, 0.0f));
    float3 fwd    = -moonDir;                            // p/ a camera (sub-observador no centro)
    float3 normal = normalize(right * uv.x + up * uv.y + fwd * z);

    // UV equiretangular do mapa da lua na face visivel (centro = sub-observador, lon/lat 0).
    float  nx  = dot(normal, right), ny = dot(normal, up), nz = dot(normal, fwd);
    float2 muv = float2(0.5f + atan2(nx, nz) / (2.0f * PI),
                        0.5f - asin(clamp(ny, -1.0f, 1.0f)) / PI);
    float3 albedo = MoonTex.SampleLevel(LinearClampSampler, muv, 0.0f).rgb;
    albedo = pow(max(albedo, 0.0f), 2.2f);              // sRGB (jpg) -> linear; branca default = 1
    // Realca o contraste dos mares em torno do albedo lunar medio (~0.13). MILD (1.5): contraste
    // forte demais + brilho alto joga tudo no "joelho" do ACES (onde tudo vira branco) e some a
    // textura. A visibilidade dos mares vem de manter a lua nos MEDIOS-TONS (brilho baixo, abaixo).
    albedo = max((float3)0.0f, 0.13f + (albedo - 0.13f) * 1.5f);

    // Achata o look de ESFERA -> DISCO: a face iluminada da lua e quase uniforme (limb-darkening
    // baixo, como a lua real), entao a TEXTURA vira a variacao dominante em vez do gradiente de
    // bola 3D. O terminador (fase/crescente) vira uma borda suave via smoothstep, nao a queda
    // cosseno do centro pra borda.
    float  ndl        = dot(normal, sunDir);
    float  lit        = smoothstep(-0.05f, 0.18f, ndl); // plano no lado iluminado, fase preservada
    float  earthshine = 0.03f;                          // brilho fraco no lado escuro
    float  edge       = smoothstep(1.0f, 0.92f, rr);    // borda suave do disco
    return (lit + earthshine) * edge * brightness * albedo;
}

float4 main(PSInput input) : SV_TARGET {
    // World view direction (translation removed → sky at infinity).
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

    // Sun disk + glare. The physical disk is tiny (~0.5°); games read "bigger"
    // mostly from bloom/glare, so we add a soft wide halo (glare) around the
    // bright core. Both are attenuated by the atmospheric transmittance toward
    // the sun (so the sun reddens near the horizon).
    float  cosToSun = dot(viewDir, sunDir);
    float3 sunTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, dot(up, sunDir));

    // Horizon occlusion check to prevent sun rendering under/through the ground.
    //float  cosHorizon = -sqrt(max(0.0f, kViewHeight * kViewHeight - kBottomR * kBottomR)) / max(kViewHeight, 1e-4f);
    //float  sunVisible = saturate((dot(up, sunDir) - cosHorizon) * 100.0f) * saturate((viewZenithCos - cosHorizon) * 100.0f);

    // Bright near-physical core disk.
    float core = smoothstep(kSunDiskCos, lerp(kSunDiskCos, 1.0f, 0.5f), cosToSun);
    // Wide soft glare: pow falloff that fades over a few degrees around the sun.
    float glare = pow(saturate(cosToSun), 350.0f);

    L += sunTrans * (kSunDiskInt * core + kSunGlareInt * glare);

    // --- Noite: estrelas + disco da lua (F2) ---
    // MoonParams: x = brilho disco, y = intensidade estrelas, z = night factor, w = tempo.
    float night = MoonParams.z;
    if (night > 0.001f) {
        // Atras da atmosfera: atenuado pela transmitancia ao longo da visada (some de dia, pois
        // o inscattering L domina; aparece de noite). Some perto do horizonte (extincao + chao).
        float3 viewTrans = SampleTransmittanceToTop(TransmittanceLUT, kViewHeight, viewZenithCos);
        float  aboveHorizon = smoothstep(-0.02f, 0.06f, viewDir.y);

        float3 stars = StarField(viewDir, MoonParams.w) * MoonParams.y;
        float3 moon  = MoonDisk(viewDir, MoonDir.xyz, MoonDir.w, sunDir, MoonParams.x);

        L += (stars + moon) * viewTrans * (night * aboveHorizon);
    }

    return float4(L, 1.0f);
}