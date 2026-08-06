#include "OceanFFTCommon.hlsli"

Texture2D<float2>   ReadBuf  : register(t0);
RWTexture2D<float2> WriteBuf : register(u0);

groupshared float2 PingPong[2][256];

// First octant of exp(+i*2*pi*k/256). The other roots are reconstructed by
// exact axis swaps/sign changes, avoiding per-thread transcendental work.
static const float2 OceanFFTFirstOctant[33] = {
    float2(1.0000000000f, 0.0000000000f),
    float2(0.9996988187f, 0.0245412285f),
    float2(0.9987954562f, 0.0490676743f),
    float2(0.9972904567f, 0.0735645636f),
    float2(0.9951847267f, 0.0980171403f),
    float2(0.9924795346f, 0.1224106752f),
    float2(0.9891765100f, 0.1467304745f),
    float2(0.9852776424f, 0.1709618888f),
    float2(0.9807852804f, 0.1950903220f),
    float2(0.9757021300f, 0.2191012402f),
    float2(0.9700312532f, 0.2429801799f),
    float2(0.9637760658f, 0.2667127575f),
    float2(0.9569403357f, 0.2902846773f),
    float2(0.9495281806f, 0.3136817404f),
    float2(0.9415440652f, 0.3368898534f),
    float2(0.9329927988f, 0.3598950365f),
    float2(0.9238795325f, 0.3826834324f),
    float2(0.9142097557f, 0.4052413140f),
    float2(0.9039892931f, 0.4275550934f),
    float2(0.8932243012f, 0.4496113297f),
    float2(0.8819212643f, 0.4713967368f),
    float2(0.8700869911f, 0.4928981922f),
    float2(0.8577286100f, 0.5141027442f),
    float2(0.8448535652f, 0.5349976199f),
    float2(0.8314696123f, 0.5555702330f),
    float2(0.8175848132f, 0.5758081914f),
    float2(0.8032075315f, 0.5956993045f),
    float2(0.7883464276f, 0.6152315906f),
    float2(0.7730104534f, 0.6343932842f),
    float2(0.7572088465f, 0.6531728430f),
    float2(0.7409511254f, 0.6715589548f),
    float2(0.7242470830f, 0.6895405447f),
    float2(0.7071067812f, 0.7071067812f)
};

float2 OceanFFTPositiveTwiddle(uint K) {
    K &= DISP_MAP_SIZE - 1u;
    const uint Quadrant = K >> 6u;
    const uint Offset = K & 63u;
    const uint OctantIndex = (Offset <= 32u) ? Offset : 64u - Offset;

    float2 W = OceanFFTFirstOctant[OctantIndex];
    if (Offset > 32u) W = W.yx;
    if ((Quadrant & 1u) != 0u) W = W.yx;
    if (Quadrant == 1u || Quadrant == 2u) W.x = -W.x;
    if (Quadrant >= 2u) W.y = -W.y;
    return W;
}

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    int z = int(gid.x);
    int x = int(tid.x);

    uint nj = (reversebits(uint(x)) >> (32u - LOG2_DISP_MAP_SIZE)) & (DISP_MAP_SIZE - 1u);
    PingPong[0][nj] = ReadBuf.Load(int3(z, x, 0));

    GroupMemoryBarrierWithGroupSync();

    int src = 0;
    [loop] for (int s = 1; s <= int(LOG2_DISP_MAP_SIZE); ++s) {
        int m  = int(1u << uint(s)); 
        int mh = m >> 1;           
        int k  = (x * (int(DISP_MAP_SIZE) / m)) & (int(DISP_MAP_SIZE) - 1);
        int i  = (x & ~(m - 1));    
        int j  = (x & (mh - 1));    

        float2 W_N_k = OceanFFTPositiveTwiddle(uint(k));

        float2 input1 = PingPong[src][i + j + mh];
        float2 input2 = PingPong[src][i + j];

        src = 1 - src;
        PingPong[src][x] = input2 + OceanComplexMul(W_N_k, input1);

        GroupMemoryBarrierWithGroupSync();
    }

    WriteBuf[int2(x, z)] = PingPong[src][x];
}
