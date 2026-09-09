// Vertex shader dos casters de sombra (CSM do sol e sombras locais).
//
// Duas permutacoes, mesma diferenca do Triangle.vs (Fase 4): de onde vem o `ModelMatrix`.
//
// Aqui e MAIS BARATO que no main view, e vale dizer por que: o caster nao precisa de motion
// vector nem de MVP composta — o `LightViewProj` ja chega por b0, por CASCATA/face. Entao a
// permutacao le UMA coluna da tabela e nao usa nem a de PrevTransform nem o CBV de view. As duas
// matrizes que o ObjectCB carregava para ca sempre foram uma so, usada, e uma ignorada.

#ifdef SMILE_INSTANCE_BINDLESS

#include "../Scene/GpuSceneTables.hlsli"

// b4: o mesmo bloco de constantes de root do main view. O caster so le duas — o descritor da
// coluna, publicado por passe, e o slot, escrito por draw.
cbuffer DrawConstantsCB : register(b4) {
    uint MaterialTableSrv;
    uint MaterialRow;
    uint TransformSrv;
    uint PrevTransformSrv;
    uint InstanceSlot;
};

float4x4 LoadModelMatrix() {
    StructuredBuffer<GpuInstanceTransform> Current = ResourceDescriptorHeap[TransformSrv];
    return Current[InstanceSlot].Value;
}

#else

cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;         // nao usado aqui: o caster projeta pela LightViewProj
    row_major float4x4 ModelMatrix;
};

float4x4 LoadModelMatrix() { return ModelMatrix; }

#endif

cbuffer ShadowCascadeCB : register(b0) {
    row_major float4x4 LightViewProj;
};

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
    VSOutput o;
    const float4x4 Model = LoadModelMatrix();
    float3 world = mul(float4(input.pos, 1.0f), Model).xyz;
    o.pos        = mul(float4(world, 1.0f), LightViewProj);
    o.uv         = input.uv;
    return o;
}
