#include "OceanFFTCommon.hlsli"

// Normal (mip 0) + Jacobiano (foam) a partir do mapa de deslocamento.
// - Normal: derivada central da altura (.z = -h), igual ao BuildNormalMipChain antigo.
// - Jacobiano: J = det(I + scale*grad(D_horizontal)); J<1 = cristas dobrando -> espuma.
//   (porte de creategradients.comp do Asylum / folding map da Cry).
// O .xyz do deslocamento e preservado; so o .w (J) e novo -> escreve OceanTex completo
// num resource separado do DispIn (evita aliasing SRV/UAV no mesmo recurso).
Texture2D<float4>   DispIn     : register(t0); // (Dx, Dz, -h, 0)
RWTexture2D<float4> OceanOut   : register(u0); // (Dx, Dz, -h, J)
RWTexture2D<float4> NormalMip0 : register(u1); // (nx, ny, nz, ToksvigT=1)

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

    // Normal da altura (.z). Derivada central; "up" tunavel (NormalUp).
    float3 nrm = normalize(float3(dL.z - dR.z, NormalUp, dD.z - dU.z));
    NormalMip0[loc] = float4(nrm, 1.0f);

    // Jacobiano da deformacao horizontal (.x = Dx, .y = Dz).
    float dDxdx = (dR.x - dL.x) * 0.5f;
    float dDzdz = (dU.y - dD.y) * 0.5f;
    float dDxdz = (dU.x - dD.x) * 0.5f;
    float dDzdx = (dR.y - dL.y) * 0.5f;

    float Jxx = 1.0f + JacobianScale * dDxdx;
    float Jzz = 1.0f + JacobianScale * dDzdz;
    float Jxz = JacobianScale * dDxdz;
    float Jzx = JacobianScale * dDzdx;
    float J   = Jxx * Jzz - Jxz * Jzx;

    OceanOut[loc] = float4(c.xyz, J);
}
