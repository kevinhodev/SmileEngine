#include "VolumetricFogCommon.hlsli"

// MaterialSetup (UE): voxeliza a densidade do meio no grid froxel.
// VBufferA = (scattering.rgb, extincao) — scattering = albedo * extincao.

RWTexture3D<float4> VBufferA : register(u0);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)kVolFogW || id.y >= (uint)kVolFogH || id.z >= (uint)kVolFogZ)
        return;

    float viewZ;
    float3 wp = VolFog_WorldPos(id, float3(0.5f, 0.5f, 0.5f), viewZ);

    float  ext = VolFog_Extinction(wp);
    float3 scattering = AlbedoAmb.rgb * ext;

    VBufferA[id] = float4(scattering, ext);
}
