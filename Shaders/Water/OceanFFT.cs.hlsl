#include "OceanFFTCommon.hlsli"

// FFT 1D em memoria compartilhada com transpose-on-write (porte de fourier_fft.comp
// do Asylum). Um grupo por linha/coluna, 256 threads. Le em (z,x), escreve em (x,z)
// -> transpoe. Chamado 2x por campo (horizontal depois vertical), reusando o scratch.
// Sem normalizacao por N (quirk fiel da Cry; createdisplacement aplica o sign-flip).
Texture2D<float2>   ReadBuf  : register(t0);
RWTexture2D<float2> WriteBuf : register(u0);

groupshared float2 PingPong[2][256];

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const float N = float(DISP_MAP_SIZE);

    int z = int(gid.x);
    int x = int(tid.x);

    // STEP 1: carrega linha/coluna com bit-reversal.
    uint nj = (reversebits(uint(x)) >> (32u - LOG2_DISP_MAP_SIZE)) & (DISP_MAP_SIZE - 1u);
    PingPong[0][nj] = ReadBuf.Load(int3(z, x, 0));

    GroupMemoryBarrierWithGroupSync();

    // STEP 2: butterflies.
    int src = 0;
    [loop] for (int s = 1; s <= int(LOG2_DISP_MAP_SIZE); ++s) {
        int m  = 1 << s;            // altura do grupo butterfly
        int mh = m >> 1;            // meia-altura
        int k  = (x * (int(DISP_MAP_SIZE) / m)) & (int(DISP_MAP_SIZE) - 1);
        int i  = (x & ~(m - 1));    // inicio do grupo butterfly
        int j  = (x & (mh - 1));    // indice dentro do grupo

        float  theta = (OCEAN_TWOPI * float(k)) / N;
        float2 W_N_k = float2(cos(theta), sin(theta));

        float2 input1 = PingPong[src][i + j + mh];
        float2 input2 = PingPong[src][i + j];

        src = 1 - src;
        PingPong[src][x] = input2 + OceanComplexMul(W_N_k, input1);

        GroupMemoryBarrierWithGroupSync();
    }

    // STEP 3: escreve transposto.
    WriteBuf[int2(x, z)] = PingPong[src][x];
}
