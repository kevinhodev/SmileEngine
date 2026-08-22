#ifndef SMILE_SKY_AMBIENT_SH_HLSLI
#define SMILE_SKY_AMBIENT_SH_HLSLI

// Avaliacao da irradiancia difusa de uma SH-L2 do ceu, projetada pelo IntegrateSkyAmbient.cs.
// Sem cbuffer e sem recurso: o shader hospedeiro passa 9 float4, um por coeficiente, em RGB.
//
// Convolucao de Ramamoorthi & Hanrahan 2001: irradiancia = radiancia convoluida com o cosseno,
// cujos fatores por banda sao A0 = PI, A1 = 2*PI/3 e A2 = PI/4. Devolvemos E/PI porque e essa
// a convencao que o consumidor ja usava com as 2 cores chapadas (multiplica direto pelo
// DiffuseColor):
//
//   E/PI = Y0*c0 + (2/3)*sum(Y1*ci) + (1/4)*sum(Y2*ci)
static const float kSHAmbientDC  = 0.282095f;
static const float kSHAmbientLin = 0.325735f; // (2/3) * 0.488603
static const float kSHAmbientQuadCross = 0.273137f; // (1/4) * 1.092548
static const float kSHAmbientQuadZonal = 0.078848f; // (1/4) * 0.315392
static const float kSHAmbientQuadDiff  = 0.136569f; // (1/4) * 0.546274

float3 EvalSkyAmbientSH(float4 sh[9], float3 n) {
    const float3 dc = max(sh[0].rgb * kSHAmbientDC, 0.0f);

    // Termos ja convoluidos e divididos por PI. Cada float3 carrega RGB, portanto toda a
    // avaliacao e o windowing sao vetorizados por canal.
    const float3 ly = sh[1].rgb * kSHAmbientLin;
    const float3 lz = sh[2].rgb * kSHAmbientLin;
    const float3 lx = sh[3].rgb * kSHAmbientLin;
    const float3 qXY = sh[4].rgb * kSHAmbientQuadCross;
    const float3 qYZ = sh[5].rgb * kSHAmbientQuadCross;
    const float3 qZ  = sh[6].rgb * kSHAmbientQuadZonal;
    const float3 qXZ = sh[7].rgb * kSHAmbientQuadCross;
    const float3 qD  = sh[8].rgb * kSHAmbientQuadDiff;

    const float3 linearTerm = lx * n.x + ly * n.y + lz * n.z;
    const float3 quadratic =
        qXY * (n.x * n.y) +
        qYZ * (n.y * n.z) +
        qZ  * (3.0f * n.z * n.z - 1.0f) +
        qXZ * (n.x * n.z) +
        qD  * (n.x * n.x - n.y * n.y);

    // Window DC-preserving contra ringing negativo. Para L1 o bound abaixo vira exatamente
    // |linear| e reproduz a protecao anterior. Para L2 usamos os maximos analiticos de cada
    // base: |xy|<=1/2, |3z2-1|<=2 e |x2-y2|<=1. Escalar todo o AC junto preserva a forma do
    // lobo e garante E>=0 sem o plateau grande que um clamp puro criaria.
    const float3 linearBound = sqrt(lx * lx + ly * ly + lz * lz);
    const float3 quadraticBound =
        0.5f * (abs(qXY) + abs(qYZ) + abs(qXZ)) + 2.0f * abs(qZ) + abs(qD);
    const float3 window = saturate(dc / max(linearBound + quadraticBound, 1e-6f));

    return max(dc + window * (linearTerm + quadratic), 0.0f);
}

#endif
