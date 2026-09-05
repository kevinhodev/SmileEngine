#ifndef SMILE_SCENE_MATERIALACCESS_HLSLI
#define SMILE_SCENE_MATERIALACCESS_HLSLI

// ================================================================================================
// Acesso do RASTER a linha canonica do material (Fase 3 do GPU-DRIVEN-PLAN).
//
// O que ele substitui: ate aqui todo draw da cena amarrava DOIS parametros de root — um CBV de
// 256 B com as constantes do material e uma tabela de oito descritores de textura. Aqui os dois
// viram uma LINHA lida da tabela canonica, e a unica coisa que muda por draw e uma constante de
// root de 4 B com o numero dessa linha.
//
// PUREZA (mesma regra do GpuSceneTablesAccess.hlsli e do RTTriangleAccess.hlsli): este header usa
// `ResourceDescriptorHeap`, que exige Shader Model 6.6 e root signature heap-directly-indexed. O
// DXC valida o corpo de toda funcao mesmo sem chamada, entao inclui-lo de um shader compilado em
// 6.0 quebra a compilacao. Ele so pode ser alcancado pela permutacao BINDLESS do MaterialCB.hlsli.
//
// Contrato de bindings, e as tres partes sao obrigatorias:
//   - root signature com CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
//   - `SetDescriptorHeaps` chamado ANTES do `SetGraphicsRootSignature` (spec de Dynamic
//     Resources; ver FPipelineState::BindGraphicsRoot);
//   - os dois valores de b4 escritos como constantes de root pelo passe.
// ================================================================================================

#include "GpuSceneTables.hlsli"

// Constantes de ROOT, nao um constant buffer de verdade: os dois valores viajam dentro da root
// signature. `MaterialTableSrv` e escrito uma vez por passe e `MaterialRow` por draw — 4 B por
// item, contra os 2 DWORDs de um root CBV mais a troca de tabela de descritores.
cbuffer SmileMaterialIndexCB : register(b4) {
    uint MaterialTableSrv; // descritor da StructuredBuffer<GpuMaterialRecord> da cena
    uint MaterialRow;      // linha deste draw; 0 = material default
};

GpuMaterialRecord Smile_LoadRasterMaterial() {
    StructuredBuffer<GpuMaterialRecord> Materials = ResourceDescriptorHeap[MaterialTableSrv];
    return Materials[MaterialRow];
}

// Indice de textura SEGURO para indexar o heap.
//
// Diferente do `MaterialTextureIndex`, que devolve a propria base invalida de proposito para que
// um bit de flag errado apareca como descritor absurdo. Aqui o indice e usado para CRIAR o handle,
// e isso acontece incondicionalmente no topo do shader — antes de qualquer `if (HasXMap)`. Um
// `ResourceDescriptorHeap[0xFFFFFFFF]` trava a GPU; ler o descritor 0 nao. Mesmo raciocinio ja
// escrito no `FGpuMeshTable::GeometrySlotOf`.
//
// O valor lido nunca chega a ser amostrado: sem tabela de descritores nenhum bit de mapa liga, e
// todo uso de textura no raster esta sob o bit correspondente.
uint Smile_RasterTextureSlot(GpuMaterialRecord _M, uint _Offset) {
    return (_M.TextureBase == 0xFFFFFFFFu) ? 0u : (_M.TextureBase + _Offset);
}

#endif
