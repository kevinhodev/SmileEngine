#include "OceanFFTCommon.hlsli"

Texture2D<float4>   DispIn     : register(t0); 
RWTexture2D<float4> OceanOut   : register(u0); 
RWTexture2D<float4> NormalMip0 : register(u1); 

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DISP_MAP_SIZE || id.y >= DISP_MAP_SIZE) return;

    int2 loc = int2(id.xy);
    int  N   = int(DISP_MAP_SIZE);
    int2 L   = int2((loc.x - 1) & (N - 1), loc.y);
    int2 R   = int2((loc.x + 1) & (N - 1), loc.y);
    int2 Dn  = int2(loc.x, (loc.y - 1) & (N - 1));
    int2 Up  = int2(loc.x, (loc.y + 1) & (N - 1));

    float4 c  = DispIn.Load(int3(loc, 0));
    float4 dL = DispIn.Load(int3(L,   0));
    float4 dR = DispIn.Load(int3(R,   0));
    float4 dD = DispIn.Load(int3(Dn,  0));
    float4 dU = DispIn.Load(int3(Up,  0));

    float3 nrm = normalize(float3(dL.z - dR.z, NormalUp, dD.z - dU.z));
    NormalMip0[loc] = float4(nrm, 1.0f);

    float dDxdx = (dR.x - dL.x) * 0.5f;
    float dDzdz = (dU.y - dD.y) * 0.5f;
    float dDxdz = (dU.x - dD.x) * 0.5f;
    float dDzdx = (dR.y - dL.y) * 0.5f;

    float Jxx = 1.0f + JacobianScale * dDxdx;
    float Jzz = 1.0f + JacobianScale * dDzdz;
    float Jxz = JacobianScale * dDxdz;
    float Jzx = JacobianScale * dDzdx;
    float J   = Jxx * Jzz - Jxz * Jzx;

    // Acumulacao temporal da espuma: .w guarda um MINIMO RELAXADO de J — onde a onda
    // dobrou (J baixo) a espuma persiste e o valor recupera rumo ao J do frame a
    // FoamRecovery/s (espuma some em ~coverage/recovery segundos). Semantica de J
    // preservada: PS/sliders/debug intactos. Ler e escrever o MESMO texel via UAV e ok.
    float prevJ = OceanOut[loc].w;
    float Jout  = (FoamReset > 0.5f) ? J
                                     : min(J, prevJ + FoamRecovery * DeltaTime);

    OceanOut[loc] = float4(c.xyz, Jout);
}
