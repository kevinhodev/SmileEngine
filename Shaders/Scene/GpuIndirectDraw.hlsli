#ifndef SMILE_SCENE_GPUINDIRECTDRAW_HLSLI
#define SMILE_SCENE_GPUINDIRECTDRAW_HLSLI

// ================================================================================================
// Espelho HLSL de Smile::FGpuIndirectDrawArgs
// (Engine/Include/Smile/Graphics/Scene/GpuIndirectDraw.h), Fase 5 do GPU-DRIVEN-PLAN.
//
// O que escreve esta struct e o scatter; quem a le e o COMMAND PROCESSOR, e nao um shader. Isso
// muda o que pode dar errado: nao ha leitura do outro lado para o DXC validar, e um campo fora de
// lugar nao da erro nenhum — da o draw errado, com a malha errada, sem mensagem.
//
// Os dois primeiros campos viram constantes de root; os cinco seguintes sao
// D3D12_DRAW_INDEXED_ARGUMENTS. A ordem e ditada pela descricao da command signature, do lado C++.
// ================================================================================================

struct GpuIndirectDrawArgs {
    uint MaterialRow;
    uint InstanceSlot;

    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    // INT tambem aqui: o valor e somado a cada indice, e `uint` faria um offset negativo — legal no
    // contrato do D3D12 — virar um numero enorme sem nenhum aviso.
    int  BaseVertexLocation;
    uint StartInstanceLocation;
};

// O bloco de constantes de root do main view, com os MESMOS offsets do
// Smile::GpuDrawConstants. Declarado como cbuffer para os shaders que o consomem; a command
// signature escreve nos offsets 1 e 4 por comando e os outros tres sobrevivem ao ExecuteIndirect.
#define SMILE_DRAW_CONSTANTS_BLOCK \
    uint MaterialTableSrv;         \
    uint MaterialRow;              \
    uint TransformSrv;             \
    uint PrevTransformSrv;         \
    uint InstanceSlot;

#endif // SMILE_SCENE_GPUINDIRECTDRAW_HLSLI
