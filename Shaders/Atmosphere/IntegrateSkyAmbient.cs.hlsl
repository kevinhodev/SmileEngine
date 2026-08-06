#include "AtmosphereCommon.hlsli"

// Ambient fisico do ceu (estilo "distant sky light" da UE): integra o SkyView LUT e devolve
// DUAS representacoes do mesmo integral:
//
//   [0] = ceu   (cos-weighted, hemisferio de cima)   \ modelo antigo de 2 cores chapadas,
//   [1] = chao  (cos-weighted, hemisferio de baixo)  / mantido como fallback e botao do A/B
//   [2..4] = SH-L1 (R, G, B), 4 coeficientes por canal, esfera INTEIRA
//
// O resultado herda tudo do LUT: multiscatter, crepusculo, transmitancia e o ceu de luar.
// Lido de volta na CPU (2 frames de latencia) e escrito no FrameConstants.
//
// POR QUE SH-L1: as 2 cores so variam com o Y da normal (o consumidor fazia
// lerp(chao, ceu, N.y*0.5+0.5)). No poente, a parede virada pro sol e a parede oposta do mesmo
// predio recebiam ambiente IDENTICO. SH-L1 acrescenta o termo linear — 3 coeficientes por canal
// — que e exatamente a informacao "de que lado o ceu esta quente".
Texture2D<float4>          SkyViewLUT : register(t0);
RWStructuredBuffer<float4> OutAmbient : register(u0);

// 64 bandas de elevacao sobre a esfera inteira = 2,8 graus por banda, contra os 22,5 graus dos
// 4 taps por hemisferio de antes. Aquela amostragem punha a amostra mais proxima do horizonte a
// 11,25 graus ACIMA dele: no poente a banda brilhante tem poucos graus de altura, entao a parte
// mais intensa e saturada do ceu simplesmente NAO entrava no integral do ambiente.
// O dispatch e de um grupo so, uma vez por frame — 64x32 = 2048 taps sai de graca.
#define NUM_EL 64
#define NUM_AZ 32

groupshared float3 gSky[NUM_EL];
groupshared float3 gGround[NUM_EL];
groupshared float3 gSH[NUM_EL][4];

[numthreads(NUM_EL, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID) {
    const uint  i      = gtid.x;
    const float dTheta = PI / (float)NUM_EL;          // esfera inteira: theta em 0..PI
    const float dPhi   = (2.0f * PI) / (float)NUM_AZ;

    // BASE DE MUNDO A PARTIR DO AZIMUTE DO SOL. O SkyView LUT e dobrado no azimute: o `phi`
    // abaixo e o angulo RELATIVO ao sol, nao um azimute de mundo. Para o integral escalar das 2
    // cores isso era irrelevante (a soma nao depende da orientacao do frame), mas o termo linear
    // da SH e um VETOR: projetado no frame errado, ele aponta para uma direcao arbitraria e o
    // ambiente ficaria quente do lado errado. Aqui o eixo X do frame do LUT e reconstruido como
    // a direcao horizontal do sol em mundo.
    float2 sunH = SunDir.xz;
    float  sunHLen = length(sunH);
    // Sol a pino: o azimute e degenerado, mas ali o ceu e quase simetrico e o termo linear
    // horizontal tende a zero de qualquer forma — qualquer base serve.
    sunH = (sunHLen > 1e-4f) ? (sunH / sunHLen) : float2(1.0f, 0.0f);
    const float3 axisX = float3(sunH.x, 0.0f, sunH.y);          // direcao do sol no plano
    const float3 axisZ = float3(-sunH.y, 0.0f, sunH.x);         // perpendicular
    const float3 axisY = float3(0.0f, 1.0f, 0.0f);

    float3 sky = float3(0.0f, 0.0f, 0.0f);
    float3 ground = float3(0.0f, 0.0f, 0.0f);
    float3 sh[4];
    [unroll] for (int c = 0; c < 4; ++c) sh[c] = float3(0.0f, 0.0f, 0.0f);

    const float theta = ((float)i + 0.5f) * dTheta;
    const float ct = cos(theta);
    const float st = sin(theta);

    for (int j = 0; j < NUM_AZ; ++j) {
        const float phi = ((float)j + 0.5f) * dPhi;
        const float cp  = cos(phi);
        const float sp  = sin(phi);

        // O LUT so quer o cosseno do azimute RELATIVO ao sol.
        const float2 uv = SkyViewParamsToUv(ct, cp, kViewHeight);
        const float3 L  = SkyViewLUT.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;

        const float dOmega = st * dTheta * dPhi;

        // Direcao em MUNDO (nao no frame do LUT) — so ela pode alimentar a SH.
        const float3 dir = axisX * (st * cp) + axisY * ct + axisZ * (st * sp);

        // Base SH real, l=0 e l=1.
        sh[0] += L * (0.282095f * dOmega);
        sh[1] += L * (0.488603f * dir.y * dOmega);
        sh[2] += L * (0.488603f * dir.z * dOmega);
        sh[3] += L * (0.488603f * dir.x * dOmega);

        // Modelo antigo, cos-weighted por hemisferio (mantido bit a bit).
        const float cosW = abs(ct) * dOmega;
        if (ct >= 0.0f) sky += L * cosW; else ground += L * cosW;
    }

    gSky[i]    = sky;
    gGround[i] = ground;
    [unroll] for (int c2 = 0; c2 < 4; ++c2) gSH[i][c2] = sh[c2];

    GroupMemoryBarrierWithGroupSync();

    // Reducao em arvore.
    [unroll] for (uint s = NUM_EL / 2; s > 0; s >>= 1) {
        if (i < s) {
            gSky[i]    += gSky[i + s];
            gGround[i] += gGround[i + s];
            [unroll] for (int c3 = 0; c3 < 4; ++c3) gSH[i][c3] += gSH[i + s][c3];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (i != 0) return;

    OutAmbient[0] = float4(gSky[0]    / PI, 1.0f);
    OutAmbient[1] = float4(gGround[0] / PI, 1.0f);
    // Empacotado por CANAL: [2] = R, [3] = G, [4] = B, cada um com (c0, c1, c2, c3).
    OutAmbient[2] = float4(gSH[0][0].r, gSH[0][1].r, gSH[0][2].r, gSH[0][3].r);
    OutAmbient[3] = float4(gSH[0][0].g, gSH[0][1].g, gSH[0][2].g, gSH[0][3].g);
    OutAmbient[4] = float4(gSH[0][0].b, gSH[0][1].b, gSH[0][2].b, gSH[0][3].b);
}
