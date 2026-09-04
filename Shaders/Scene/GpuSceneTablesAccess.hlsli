#ifndef SMILE_SCENE_GPUSCENETABLESACCESS_HLSLI
#define SMILE_SCENE_GPUSCENETABLESACCESS_HLSLI

// ================================================================================================
// ACESSO BINDLESS as tabelas canonicas. UMA funcao, e ela mora separada pelo mesmo motivo que o
// RTTriangleAccess.hlsli: `ResourceDescriptorHeap` exige SM 6.6 e root signature
// heap-directly-indexed, e o DXC valida o corpo de qualquer funcao mesmo sem chamada. Posta no
// GpuSceneTables.hlsli — que o DDGICommon inclui, e que shaders de RASTER incluem — ela quebraria
// a compilacao do DebugView.ps.hlsl e do DeferredLighting.ps.hlsl.
//
// Contrato de bindings: `InstanceGeo`/`GpuMaterialRecord` (GpuSceneTables.hlsli, incluido antes) e
// root signature heap-directly-indexed.
// ================================================================================================

#include "GpuSceneTables.hlsli"

// Linha do material do hit.
//
// Duas leituras dependentes em vez de uma (instancia -> material) e o preco da Fase 2, e ele nao
// e obviamente uma perda: o snapshot por instancia vive num UPLOAD heap — memoria do host, lida
// por PCIe em placa discreta — enquanto a tabela de materiais vive em DEFAULT heap. Nas cenas
// atuais ela inteira cabe em poucos KB (214 materiais na Emerald = ~14 KB) e tende a ficar
// residente em cache; o que sai do upload heap por hit cai de 100 B para 20 B. A hipotese e essa;
// medi-la exige GPU e nao foi feita nesta fase.
//
// Chamada varias vezes por hit em alguns caminhos (PT_LoadHitSurface, PT_LoadHitMaterial e
// PT_LoadHitEmissive carregam cada um a sua). Sao leituras de SRV somente-leitura com o mesmo
// indice, entao o DXC as funde — e o codigo ja dependia disso antes, porque o
// RadianceCacheUpdate.cs.hlsl reindexa `Instances[instId]` quatro vezes seguidas.
GpuMaterialRecord RT_LoadMaterial(InstanceGeo _Geo) {
    StructuredBuffer<GpuMaterialRecord> Materials = ResourceDescriptorHeap[_Geo.MaterialSrv];
    return Materials[_Geo.MaterialId];
}

// Linha da malha do hit: bounds locais, contagens e os descritores de VB/IB.
//
// Nao ha chamador no caminho quente, e e de proposito: o payload por triangulo, que TODO hit le,
// continua desnormalizado no InstanceGeo. Quem passa por aqui e caminho frio — o MeshLightExtract,
// uma vez por cena — e o culling da Fase 6, que le bounds e nao geometria.
GpuMeshRecord RT_LoadMesh(InstanceGeo _Geo) {
    StructuredBuffer<GpuMeshRecord> Meshes = ResourceDescriptorHeap[_Geo.MeshSrv];
    return Meshes[_Geo.MeshId];
}

#endif
