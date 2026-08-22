#include "DDGICommon.hlsli"

Buffer<uint>   ActiveProbeCount : register(t0);
RWBuffer<uint> DispatchArgs     : register(u0);

// D3D12_DISPATCH_ARGUMENTS = { ThreadGroupCountX, Y, Z }. Um item zero ainda emite um grupo para
// evitar dimensao zero; os tres consumidores veem count=0 e retornam uniformemente antes de
// qualquer barrier do grupo.
[numthreads(1, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    const int work = max((int)ActiveProbeCount[0], 1);
    const int groupsX = DDGI_DispatchGroupsX(work);
    DispatchArgs[0] = (uint)groupsX;
    DispatchArgs[1] = (uint)((work + groupsX - 1) / groupsX);
    DispatchArgs[2] = 1u;
}
