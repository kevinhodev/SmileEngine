#ifndef SMILE_DI_LIGHT_SAMPLING_HLSLI
#define SMILE_DI_LIGHT_SAMPLING_HLSLI

// Amostragem unificada de luz para o ReSTIR DI, em medida de ANGULO SOLIDO.
//
// Por que existe: o reservoir do DI tinha dominio DISCRETO — a amostra era um indice de luz, e a
// atenuacao 1/d^2 morava dentro do PunctualLightIncoming. Um triangulo emissivo e continuo: a
// amostra e um ponto numa area. Misturar os dois num unico stream de WRS exige uma medida comum,
// e a escolhida e o angulo solido: toda amostra vira (ponto na luz, pdf em sr), e o 1/d^2 mais o
// cosseno da superficie da luz saem da avaliacao e entram na CONVERSAO DE MEDIDA.
//
// ENERGIA — a parte que nao pode errar. A esfera passa a ser amostrada de verdade, entao a
// radiancia dela tem de sair da intensidade que o artista ja autorou, senao toda luz da cena muda
// de brilho. Para d >> r a irradiancia de uma esfera de radiancia L e E ~= L*pi*r^2*cos/d^2, e a
// da pontual e E = I*cos/d^2. Logo L = I / (pi*r^2). E o que DI_SphereRadiance faz.
//
// O MODELO AUTORAL FICA: a janela de alcance (1-(d/r)^4)^2 e a mascara de cone do spot continuam
// multiplicando a radiancia, avaliadas na distancia ao CENTRO. Elas nao sao fisicas — sao o modelo
// Unreal/Cry que a engine ja usa — e avaliar no centro mantem a forma da queda identica a de hoje,
// entao subir o raio da fonte nao muda o alcance percebido da luz.

#ifndef SMILE_PI
#define SMILE_PI 3.14159265358979f
#endif

struct DILightSample {
    float3 Position;      // ponto amostrado na superficie da luz, em mundo
    float3 Radiance;      // radiancia emitida nesse ponto, ja com janela/cone do modelo autoral
    float  SolidAnglePdf; // pdf da amostra em angulo solido, vista do ponto sombreado
    bool   Valid;
};

void DISampleInit(out DILightSample s) {
    s.Position = 0.0f; s.Radiance = 0.0f; s.SolidAnglePdf = 0.0f; s.Valid = false;
}

// Base ortonormal branchless (Duff et al. 2017). n precisa estar normalizado.
void DI_Onb(float3 n, out float3 t1, out float3 t2) {
    const float s = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float b = n.x * n.y * a;
    t1 = float3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    t2 = float3(b, s + n.y * n.y * a, -n.y);
}

// Conversao area -> angulo solido. cosLight = |cos| entre a normal da luz e a direcao de chegada.
// Devolve 0 quando a conexao e degenerada, o que zera a amostra em vez de gerar Inf.
float DI_PdfAtoW(float areaPdf, float distSq, float cosLight) {
    return (cosLight > 1e-6f) ? (areaPdf * distSq / cosLight) : 0.0f;
}

// Radiancia da esfera derivada da intensidade autorada. Ver o bloco de ENERGIA acima.
float3 DI_SphereRadiance(float3 intensity, float radius) {
    const float r = max(radius, 1e-3f);
    return intensity / (SMILE_PI * r * r);
}

// Janela de alcance + cone do spot, avaliados na distancia ao CENTRO — identicos ao
// PunctualLightIncoming, para o modelo autoral nao mudar de significado.
float DI_AuthoredFalloff(FGPULightFull light, float3 shadePos, out float3 dirToCenter) {
    const float3 toC = light.PosInvRadius.xyz - shadePos;
    const float  d2  = dot(toC, toC);
    const float  d   = sqrt(d2);
    dirToCenter = toC / max(d, 1e-4f);

    float win = d2 * light.PosInvRadius.w * light.PosInvRadius.w;
    win = saturate(1.0f - win * win);
    win *= win;

    if (light.DirCosOuter.w > -1.5f) {
        const float sm = saturate((dot(-dirToCenter, light.DirCosOuter.xyz) - light.DirCosOuter.w)
                                  * light.SpotParams.x);
        win *= sm * sm;
    }
    return win;
}

// Amostra a esfera pelo CONE que ela subtende, visto do ponto sombreado. E o estimador certo para
// fonte esferica: uniforme no cone tem variancia muito menor que uniforme na superficie, porque
// nao gasta amostras no hemisferio de tras (que a propria esfera oclui).
DILightSample DI_SampleSphereLight(FGPULightFull light, float3 shadePos, float2 u) {
    DILightSample s;
    DISampleInit(s);

    float3 L;
    const float falloff = DI_AuthoredFalloff(light, shadePos, L);
    if (falloff <= 0.0f) return s;

    const float3 toC = light.PosInvRadius.xyz - shadePos;
    const float  d2  = dot(toC, toC);
    const float  d   = sqrt(d2);
    const float  r   = max(light.ColorSourceRadius.w, 1e-3f);
    const float3 radiance = DI_SphereRadiance(light.ColorSourceRadius.rgb, r) * falloff;

    if (d <= r * 1.0001f) {
        // Ponto DENTRO da esfera: o cone degenera. Amostra a direcao uniformemente na esfera
        // inteira (pdf 1/4pi) e projeta na superficie. Caso raro e a alternativa seria NaN.
        const float z   = 1.0f - 2.0f * u.x;
        const float rr  = sqrt(saturate(1.0f - z * z));
        const float phi = 2.0f * SMILE_PI * u.y;
        const float3 dir = float3(rr * cos(phi), rr * sin(phi), z);
        s.Position      = light.PosInvRadius.xyz + dir * r;
        s.Radiance      = radiance;
        s.SolidAnglePdf = 1.0f / (4.0f * SMILE_PI);
        s.Valid         = true;
        return s;
    }

    // Cone subtendido: sin(thetaMax) = r/d.
    const float sinThetaMax2 = saturate((r * r) / d2);
    const float cosThetaMax  = sqrt(saturate(1.0f - sinThetaMax2));

    const float cosTheta = 1.0f - u.x * (1.0f - cosThetaMax);
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    const float phi      = 2.0f * SMILE_PI * u.y;

    float3 t1, t2;
    DI_Onb(L, t1, t2);
    const float3 dir = normalize(t1 * (sinTheta * cos(phi)) +
                                 t2 * (sinTheta * sin(phi)) +
                                 L  *  cosTheta);

    // Intersecao com a esfera ao longo da direcao amostrada: o ponto e o que o raio de sombra
    // precisa mirar. Como a direcao esta dentro do cone, o discriminante e >= 0 a menos de erro
    // numerico; o max() cobre a borda.
    const float b    = dot(dir, toC);
    const float disc = max(b * b - (d2 - r * r), 0.0f);
    const float t    = b - sqrt(disc);
    if (t <= 0.0f) return s;

    s.Position      = shadePos + dir * t;
    s.Radiance      = radiance;
    s.SolidAnglePdf = 1.0f / (2.0f * SMILE_PI * (1.0f - cosThetaMax));
    s.Valid         = s.SolidAnglePdf > 0.0f;
    return s;
}

// Amostra uniforme na AREA do triangulo, convertida para angulo solido. A raiz no primeiro termo
// e o mapeamento padrao de quadrado para baricentricas com densidade uniforme.
DILightSample DI_SampleTriangleLight(FTriangleLightGPU tri, float3 shadePos, float2 u) {
    DILightSample s;
    DISampleInit(s);

    const float3 e0 = MeshLight_Edge0(tri);
    const float3 e1 = MeshLight_Edge1(tri);
    const float3 cr = cross(e0, e1);
    const float  crLen = length(cr);
    if (crLen < 1e-12f) return s; // degenerado

    const float area   = 0.5f * crLen;
    const float3 normal = cr / crLen;

    const float su = sqrt(u.x);
    const float b0 = 1.0f - su;
    const float b1 = u.y * su;

    s.Position = tri.Base + e0 * b0 + e1 * b1;

    const float3 toL   = s.Position - shadePos;
    const float  distSq = dot(toL, toL);
    if (distSq < 1e-8f) return s;
    const float3 dir = toL * rsqrt(distSq);

    // Emissivo e de face dupla aqui: a malha de uma luminaria costuma ter normal apontando para
    // fora e o interior tambem ilumina. abs() evita apagar metade dos triangulos por winding.
    const float cosLight = abs(dot(normal, -dir));

    s.Radiance      = MeshLight_UnpackRGB9E5(tri.Radiance);
    s.SolidAnglePdf = DI_PdfAtoW(1.0f / area, distSq, cosLight);
    s.Valid         = s.SolidAnglePdf > 0.0f && any(s.Radiance > 0.0f);
    return s;
}

// Pool combinado: indice linear. [0, analyticCount) = analitica; acima disso e triangulo, com
// indice do triangulo = index - analyticCount. Sem bit de tag — o espaco linear ja separa, e
// mantem o LightIndex do reservoir um indice simples.
bool DI_IsTriangleIndex(uint index, uint analyticCount) { return index >= analyticCount; }

DILightSample DI_SampleAnyLight(StructuredBuffer<FGPULightFull> lights, uint analyticCount,
                                StructuredBuffer<FTriangleLightGPU> tris, uint triCount,
                                uint index, float2 uv, float3 shadePos) {
    DILightSample s;
    DISampleInit(s);
    if (index >= analyticCount + triCount) return s;
    if (DI_IsTriangleIndex(index, analyticCount))
        return DI_SampleTriangleLight(tris[index - analyticCount], shadePos, uv);
    return DI_SampleSphereLight(lights[index], shadePos, uv);
}

// pHat em ANGULO SOLIDO: BRDF * radiancia * cos(theta) no ponto sombreado. NAO tem 1/d^2 nem o
// cosseno da superficie da luz — os dois moram na pdf agora, e repetir aqui contaria duas vezes.
//
// Tambem NAO usa AreaSphereSpecular: o alargamento do especular passa a emergir de amostrar a
// superficie da luz. Aplicar o ponto representativo por cima contaria a area duas vezes, e por
// isso Ld e Ls recebem a MESMA direcao amostrada e SpecEnergy vai 1.
float DI_TargetFromSample(DILightSample ls, GBufferData g, float3 worldPos, float3 cameraPos,
                          out float3 outDiffuse, out float3 outSpecular,
                          out float3 outL, out float outDist) {
    outDiffuse = 0.0f; outSpecular = 0.0f; outL = float3(0.0f, 1.0f, 0.0f); outDist = 0.0f;
    if (!ls.Valid) return 0.0f;

    const float3 toL = ls.Position - worldPos;
    const float  d2  = dot(toL, toL);
    if (d2 < 1e-8f) return 0.0f;
    outDist = sqrt(d2);
    outL    = toL / outDist;

    const float3 N = g.WorldNormal;
    const float3 V = normalize(cameraPos - worldPos);
    const float3 diffuseColor  = g.BaseColor * (1.0f - g.Metallic);
    const float3 specularColor = lerp(float3(0.04f, 0.04f, 0.04f), g.BaseColor, g.Metallic);
    const float3 transColor    = g.BaseColor * g.Subsurface;
    const float  roughness     = max(g.Roughness, 0.04f);
    const float  a2            = roughness * roughness * roughness * roughness;

    BRDF_DirectAreaSplit(N, V, outL, outL, 1.0f, ls.Radiance,
                         diffuseColor, specularColor, roughness, a2, transColor,
                         outDiffuse, outSpecular);
    return DI_Luminance(outDiffuse + outSpecular);
}

#endif
