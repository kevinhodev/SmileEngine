#ifndef SMILE_ATMOSPHERE_MATH_HLSLI
#define SMILE_ATMOSPHERE_MATH_HLSLI

// Matematica da atmosfera (Hillaire 2020) SEM estado: nenhum cbuffer, nenhum sampler, cada
// parametro do planeta como argumento explicito.
//
// Por que existe separado do AtmosphereCommon.hlsli: aquele header declara
// `cbuffer AtmosphereCB : register(b0)`, que colide com o b0 proprio de todo shader de trace
// (DDGI, ReSTIR, reflexoes). Sem este arquivo, o unico jeito de o caminho de GI ler o sky-view
// LUT era reescrever a parameterizacao a mao — foi exatamente o que aconteceu, e a copia
// divergiu do bake (view height 6360.5 contra 6360.001, ~3,8 texels dos 104 no horizonte).
// Agora ha UMA implementacao: o AtmosphereCommon vira um wrapper fino que so injeta o CB.
//
// Consequencia pratica: qualquer mudanca de parameterizacao (o FromUnitToSubUvs da paridade de
// ray march, por ex.) entra em UM lugar e atinge os seis leitores do LUT de uma vez. Nao ha
// como aplicar pela metade.
static const float kAtmoPI = 3.14159265358979323846f;

float AtmoRayleighPhase(float cosTheta) {
    return (3.0f / (16.0f * kAtmoPI)) * (1.0f + cosTheta * cosTheta);
}

float AtmoMiePhaseHG(float g, float cosTheta) {
    float g2    = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * kAtmoPI * pow(max(denom, 1e-4f), 1.5f));
}

float AtmoUniformPhase() { return 1.0f / (4.0f * kAtmoPI); }

// Primeira intersecao POSITIVA com a esfera de raio `radius` centrada na origem; -1 se nao ha.
// Origem em espaco km centrado no planeta.
float AtmoRaySphereNearest(float3 ro, float3 rd, float radius) {
    float b    = dot(ro, rd);
    float c    = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    float s  = sqrt(disc);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t1 < 0.0f) return -1.0f;
    return (t0 < 0.0f) ? t1 : t0;
}

float AtmoRaySphereFar(float3 ro, float3 rd, float radius) {
    float b    = dot(ro, rd);
    float c    = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    return -b + sqrt(disc);
}

// ---------------------------------------------------------------------------------------------
// LUT de transmitancia: u = distancia ate o topo normalizada, v = altura normalizada.
// Identica a da UE (SkyAtmosphereCommon.ush, LutTransmittanceParamsToUv).
// ---------------------------------------------------------------------------------------------
void AtmoUvToTransmittanceParams(out float viewHeight, out float viewZenithCos,
                                 float2 uv, float bottomR, float topR) {
    float xMu = uv.x;
    float xR  = uv.y;
    float H   = sqrt(max(0.0f, topR * topR - bottomR * bottomR));
    float rho = H * xR;
    viewHeight = sqrt(max(0.0f, rho * rho + bottomR * bottomR));

    float dMin = topR - viewHeight;
    float dMax = rho + H;
    float d    = dMin + xMu * (dMax - dMin);
    viewZenithCos = (d == 0.0f) ? 1.0f
                                : (H * H - rho * rho - d * d) / (2.0f * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0f, 1.0f);
}

float2 AtmoTransmittanceParamsToUv(float viewHeight, float viewZenithCos,
                                   float bottomR, float topR) {
    float H   = sqrt(max(0.0f, topR * topR - bottomR * bottomR));
    float rho = sqrt(max(0.0f, viewHeight * viewHeight - bottomR * bottomR));

    float discriminant = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0f)
                       + topR * topR;
    float d = max(0.0f, -viewHeight * viewZenithCos + sqrt(max(0.0f, discriminant)));

    float dMin = topR - viewHeight;
    float dMax = rho + H;
    float xMu  = (d - dMin) / max(dMax - dMin, 1e-5f);
    float xR   = rho / max(H, 1e-5f);
    return float2(xMu, xR);
}

// ---------------------------------------------------------------------------------------------
// Sky-view LUT: u = azimute dobrado no plano do sol, v = zenite com warp sqrt ASSIMETRICO em
// torno do horizonte (v=0.5), que concentra texels onde o gradiente e maior.
//
// O `viewHeight` NAO e um detalhe: ele define o angulo do horizonte (beta), logo onde cai a
// dobra do warp. Dois consumidores com view heights diferentes leem texels diferentes para a
// MESMA direcao — e o erro se concentra justo na banda mais brilhante do LUT.
// ---------------------------------------------------------------------------------------------
void AtmoUvToSkyViewParams(out float viewZenithCos, out float lightViewCos,
                           float2 uv, float viewHeight, float bottomR) {
    float vHorizon = sqrt(max(0.0f, viewHeight * viewHeight - bottomR * bottomR));
    float cosBeta  = vHorizon / max(viewHeight, 1e-4f);
    float beta     = acos(clamp(cosBeta, -1.0f, 1.0f));
    float zenithHorizonAngle = kAtmoPI - beta;

    float viewZenithAngle;
    if (uv.y < 0.5f) {
        float coord = 2.0f * uv.y;
        coord = 1.0f - coord;
        coord = 1.0f - coord * coord;
        viewZenithAngle = zenithHorizonAngle * coord;
    } else {
        float coord = uv.y * 2.0f - 1.0f;
        coord = coord * coord;
        viewZenithAngle = zenithHorizonAngle + beta * coord;
    }
    viewZenithCos = cos(viewZenithAngle);

    float coord = uv.x;
    coord = coord * coord;
    lightViewCos = -(coord * 2.0f - 1.0f);
}

float2 AtmoSkyViewParamsToUv(float viewZenithCos, float lightViewCos,
                             float viewHeight, float bottomR) {
    float vHorizon = sqrt(max(0.0f, viewHeight * viewHeight - bottomR * bottomR));
    float cosBeta  = vHorizon / max(viewHeight, 1e-4f);
    float beta     = acos(clamp(cosBeta, -1.0f, 1.0f));
    float zenithHorizonAngle = kAtmoPI - beta;

    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0f, 1.0f));
    float u, v;
    if (viewZenithAngle < zenithHorizonAngle) {
        float coord = viewZenithAngle / max(zenithHorizonAngle, 1e-4f);
        coord = 1.0f - coord;
        coord = 1.0f - sqrt(max(0.0f, coord));
        v = coord * 0.5f;
    } else {
        float coord = (viewZenithAngle - zenithHorizonAngle) / max(beta, 1e-4f);
        coord = sqrt(max(0.0f, coord));
        v = coord * 0.5f + 0.5f;
    }
    {
        float coord = -lightViewCos * 0.5f + 0.5f;
        u = sqrt(max(0.0f, coord));
    }
    return float2(u, v);
}

// Direcao de mundo -> uv do sky-view LUT. O LUT e dobrado no azimute do sol (u mede o angulo
// relativo sol->visada no plano horizontal), entao esta conversao precisa das duas direcoes.
// Extraida aqui porque o sky PS, o cubo de reflexo e o caminho de GI faziam a mesma conta.
float2 AtmoWorldDirToSkyViewUv(float3 dir, float3 sunDir, float viewHeight, float bottomR) {
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float  viewZenithCos = dot(dir, up);
    float3 viewHoriz = dir    - up * viewZenithCos;
    float3 sunHoriz  = sunDir - up * dot(sunDir, up);
    float  lightViewCos = dot(normalize(viewHoriz + 1e-6f), normalize(sunHoriz + 1e-6f));
    return AtmoSkyViewParamsToUv(viewZenithCos, lightViewCos, viewHeight, bottomR);
}

#endif
