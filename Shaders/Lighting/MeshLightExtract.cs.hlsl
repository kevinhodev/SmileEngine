// Extracao de triangulos emissivos para o pool de mesh lights (fase 3).
//
// Uma thread por triangulo emissivo da cena. A thread descobre a qual malha pertence por busca
// binaria nas tasks, le VB/IB bindless pelo InstanceGeo, leva as posicoes para mundo e grava um
// FTriangleLightGPU. Estrutura tirada do PrepareLights.hlsl do RTXDI.
//
// Diferenca deliberada em relacao a referencia: o RTXDI reconstroi TUDO todo frame. Aqui o
// dispatch so acontece quando a cena esta suja (ver FMeshLights::MarkDirty), porque a geometria
// emissiva do projeto e estatica — reextrair 26 mil triangulos por frame seria banda jogada fora,
// e em escala de jogo (1M de triangulos = 32 MB por frame) deixaria de ser detalhe.

#include "../GI/DDGICommon.hlsli"   // InstanceGeo, DDGIVertex, INSTGEO_FLAG_EMISSIVE
#include "MeshLightCommon.hlsli"

cbuffer MeshLightCB : register(b0) {
    uint4 Counts; // x = numero de tasks, y = total de triangulos, zw livres
};

StructuredBuffer<FMeshLightTask> Tasks     : register(t0);
StructuredBuffer<InstanceGeo>    Instances : register(t1);

RWStructuredBuffer<FTriangleLightGPU> OutLights : register(u0);

SamplerState LinearWrap : register(s1);

// Busca binaria pelo intervalo [LightOffset, LightOffset + TriangleCount) que contem o indice.
// As tasks sao montadas em ordem crescente de LightOffset pela CPU, sem buracos.
bool FindTask(uint index, out FMeshLightTask task, out uint localTri) {
    task = (FMeshLightTask)0;
    localTri = 0;

    int lo = 0;
    int hi = (int)Counts.x - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const FMeshLightTask t = Tasks[mid];
        if (index < t.LightOffset) {
            hi = mid - 1;
        } else if (index >= t.LightOffset + t.TriangleCount) {
            lo = mid + 1;
        } else {
            task = t;
            localTri = index - t.LightOffset;
            return true;
        }
    }
    return false;
}

float3 ToWorld(FMeshLightTask task, float3 p) {
    const float4 p4 = float4(p, 1.0f);
    return float3(dot(task.Row0, p4), dot(task.Row1, p4), dot(task.Row2, p4));
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint index = dtid.x;
    if (index >= Counts.y) return;

    FMeshLightTask task;
    uint tri;
    if (!FindTask(index, task, tri)) return;

    const InstanceGeo geo = Instances[task.InstanceIndex];
    StructuredBuffer<DDGIVertex> Verts = ResourceDescriptorHeap[geo.VertexSrv];
    Buffer<uint>                 Idx   = ResourceDescriptorHeap[geo.IndexSrv];

    const uint i0 = Idx[tri * 3 + 0];
    const uint i1 = Idx[tri * 3 + 1];
    const uint i2 = Idx[tri * 3 + 2];

    const float3 p0 = ToWorld(task, Verts[i0].Position);
    const float3 p1 = ToWorld(task, Verts[i1].Position);
    const float3 p2 = ToWorld(task, Verts[i2].Position);

    const float3 e0 = p1 - p0;
    const float3 e1 = p2 - p0;

    // EmissiveFactor.rgb ja vem escalado pelo RTEmissiveScale (ver FillInstanceGeo): uma malha
    // marcada como "brilha mas nao ilumina" chega aqui com radiancia zero e sai com fluxo zero,
    // entao a alias table da fase 4 nunca a escolhe.
    float3 radiance = geo.EmissiveFactor.rgb;

    if ((geo.Flags & INSTGEO_FLAG_EMISSIVE) != 0u) {
        // Radiancia MEDIA do triangulo, nao um point sample do centro. Elipse inscrita no
        // triangulo em UV, com os eixos paralelos a aresta mais curta e a mediana oposta a ela:
        // triangulo comprido/fino vira amostragem bem anisotropica, triangulo normal vira quase
        // redonda. Mesma construcao do PrepareLights.hlsl do RTXDI.
        const float2 uv0 = Verts[i0].TexCoord;
        const float2 uv1 = Verts[i1].TexCoord;
        const float2 uv2 = Verts[i2].TexCoord;

        const float2 ed[3] = { uv1 - uv0, uv2 - uv1, uv0 - uv2 };
        const float  el[3] = { length(ed[0]), length(ed[1]), length(ed[2]) };

        float2 shortEdge, longEdge1, longEdge2;
        if (el[0] < el[1] && el[0] < el[2])      { shortEdge = ed[0]; longEdge1 = ed[1]; longEdge2 = ed[2]; }
        else if (el[1] < el[2])                  { shortEdge = ed[1]; longEdge1 = ed[2]; longEdge2 = ed[0]; }
        else                                     { shortEdge = ed[2]; longEdge1 = ed[0]; longEdge2 = ed[1]; }

        const float2 shortGrad = shortEdge * (2.0f / 3.0f);
        const float2 longGrad  = (longEdge1 + longEdge2) / 3.0f;
        const float2 centerUV  = (uv0 + uv1 + uv2) / 3.0f;

        Texture2D<float4> emissiveTex = ResourceDescriptorHeap[geo.EmissiveMapIndex];
        radiance *= emissiveTex.SampleGrad(LinearWrap, centerUV, shortGrad, longGrad).rgb;
    }

    radiance = max(radiance, 0.0f);

    const float3 cr   = cross(e0, e1);
    const float  area = 0.5f * length(cr);

    FTriangleLightGPU outLight;
    outLight.Base     = p0;
    outLight.Edges0   = f32tof16(e0.x) | (f32tof16(e1.x) << 16);
    outLight.Edges1   = f32tof16(e0.y) | (f32tof16(e1.y) << 16);
    outLight.Edges2   = f32tof16(e0.z) | (f32tof16(e1.z) << 16);
    outLight.Radiance = MeshLight_PackRGB9E5(radiance);
    // Potencia do triangulo, igual ao getPower() do RTXDI. Triangulo degenerado (area 0) sai com
    // fluxo 0 e some da amostragem por potencia sem precisar de caso especial.
    outLight.Flux     = area * SMILE_PI * MeshLight_Luminance(radiance);

    OutLights[index] = outLight;
}
