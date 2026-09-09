// Vertex shader da geometria de SELECAO. Dois consumidores, e eles nao compartilham buffer:
//
//   - o PICKING (ObjectID offscreen) le o ObjectCB do frame, escrito pelo laco de montagem;
//   - o CONTORNO de selecao tem um ObjectCB PROPRIO (FSelectionOutline), justamente para nao
//     herdar o jitter do compartilhado.
//
// A permutacao de transform da Fase 4 vale so para o primeiro. O contorno continua compilando e
// usando o `.cso` classico, e por isso o corpo classico nao pode mudar.

#ifdef SMILE_INSTANCE_BINDLESS

#include "../Scene/GpuSceneTables.hlsli"
#include "../Scene/InstanceTransform.hlsli"

// b2: as matrizes de view do passe, no mesmo registrador que o ObjectCB ocupava.
cbuffer ViewMatricesCB : register(b2) {
    GpuViewMatrices ViewMatrices;
};

// b4: o bloco de constantes de draw. O picking so le duas — o descritor da coluna, por passe, e
// o slot, por draw.
cbuffer DrawConstantsCB : register(b4) {
    uint MaterialTableSrv;
    uint MaterialRow;
    uint TransformSrv;
    uint PrevTransformSrv;
    uint InstanceSlot;
};

float4x4 LoadMVP() {
    StructuredBuffer<GpuInstanceTransform> Current = ResourceDescriptorHeap[TransformSrv];
    // Pela composicao COMPARTILHADA, e nao por um `mul` local: o picking tem de concordar com o
    // raster ate o ultimo bit, senao o objeto sob o cursor deixa de ser o objeto desenhado ali.
    // A coluna PrevTransform nao e lida — o picking nao tem motion vector — e passar `Current`
    // no lugar dela e inofensivo porque so o MVP e consumido abaixo.
    return ComposeObjectTransforms(ViewMatrices, Current[InstanceSlot].Value,
                                   Current[InstanceSlot].Value).MVP;
}

#else

cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
};

float4x4 LoadMVP() { return MVP; }

#endif

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = mul(float4(input.pos, 1.0f), LoadMVP());
    output.uv  = input.uv;
    return output;
}
