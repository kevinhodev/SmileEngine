#ifndef SMILE_SKY_AMBIENT_SH_HLSLI
#define SMILE_SKY_AMBIENT_SH_HLSLI

// Avaliacao da irradiancia difusa de uma SH-L1 do ceu, projetada pelo IntegrateSkyAmbient.cs.
// Sem cbuffer e sem recurso: o shader hospedeiro passa os 3 float4 (um por canal), cada um com
// (c0, c1, c2, c3) na base real l=0/l=1.
//
// Convolucao de Ramamoorthi & Hanrahan 2001: irradiancia = radiancia convoluida com o cosseno,
// cujos fatores por banda sao A0 = PI e A1 = 2*PI/3. Devolvemos E/PI porque e essa a convencao
// que o consumidor ja usava com as 2 cores chapadas (multiplica direto pelo DiffuseColor):
//
//   E/PI = 0.282095*c0 + (2/3)*0.488603*(c1*n.y + c2*n.z + c3*n.x)
static const float kSHAmbientDC  = 0.282095f;
static const float kSHAmbientLin = 0.325735f; // (2/3) * 0.488603

float3 EvalSkyAmbientSH(float4 shR, float4 shG, float4 shB, float3 n) {
    float3 dc = float3(shR.x, shG.x, shB.x) * kSHAmbientDC;

    // Termo linear por canal, ja no espaco da normal (x,y,z).
    float3 lx = float3(shR.w, shG.w, shB.w) * kSHAmbientLin;
    float3 ly = float3(shR.y, shG.y, shB.y) * kSHAmbientLin;
    float3 lz = float3(shR.z, shG.z, shB.z) * kSHAmbientLin;

    // CLAMP DO LOBO NEGATIVO. E(n) = dc + dot(linear, n), entao o minimo sobre a esfera e
    // dc - |linear|: com um sinal muito direcional (e um poente e exatamente isso) a L1 fica
    // NEGATIVA nas direcoes opostas ao sol e aparecem manchas pretas em superficies viradas
    // para o lado contrario. Limitar o comprimento do vetor linear a dc garante E >= 0 em
    // qualquer direcao PRESERVANDO a direcao do lobo — melhor que um max(0) no fim, que
    // achataria o gradiente inteiro perto do zero. E o mesmo espirito do windowing padrao.
    float3 e = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int c = 0; c < 3; ++c) {
        float3 lin = float3(lx[c], ly[c], lz[c]);
        float  len = length(lin);
        float  cap = max(dc[c], 0.0f);
        if (len > cap) lin *= cap / max(len, 1e-6f);
        e[c] = cap + dot(lin, n);
    }
    return max(e, 0.0f);
}

#endif
