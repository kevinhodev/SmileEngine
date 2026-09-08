// Vertex shader da geometria da cena: z-prepass, G-buffer e forward de translucidos.
//
// Duas permutacoes, e a diferenca e de ONDE vem o transform (Fase 4 do GPU-DRIVEN-PLAN).
//
//   classico   — quatro matrizes num CBV amarrado POR DRAW (`ObjectCB`, 256 B por instancia por
//                frame, num upload heap);
//   bindless   — o slot da instancia chega numa constante de root, as duas matrizes saem da tabela
//                canonica, e as tres de view valem para o passe inteiro.
//
// O corpo depois da composicao e o MESMO nos dois. E isso que faz o A/B atribuir qualquer
// diferenca visual ao acesso a dados, e nao a transformacao.

#include "Scene/InstanceTransform.hlsli"

#ifdef SMILE_INSTANCE_BINDLESS

#include "Scene/GpuSceneTables.hlsli"

// b2: as matrizes de VIEW do passe. Mesmo registrador que o ObjectCB ocupava no caminho classico —
// o que ele carregava de dependente do objeto passou para a tabela, e o que sobrou e por passe.
cbuffer ViewMatricesCB : register(b2) {
    GpuViewMatrices ViewMatrices;
};

// b4: o mesmo bloco de constantes de root que a Fase 3 abriu para o material.
//   [0] descritor da tabela de materiais, [1] linha do material — por passe / por grupo;
//   [2] descritor da coluna Transform, [3] descritor da coluna PrevTransform — por passe;
//   [4] slot da instancia — o UNICO estado por draw que sobra no vertex shader.
cbuffer DrawConstantsCB : register(b4) {
    uint MaterialTableSrv;
    uint MaterialRow;
    uint TransformSrv;
    uint PrevTransformSrv;
    uint InstanceSlot;
};

GpuObjectTransforms LoadTransforms() {
    StructuredBuffer<GpuInstanceTransform> Current  = ResourceDescriptorHeap[TransformSrv];
    StructuredBuffer<GpuInstanceTransform> Previous = ResourceDescriptorHeap[PrevTransformSrv];
    return ComposeObjectTransforms(ViewMatrices, Current[InstanceSlot].Value,
                                   Previous[InstanceSlot].Value);
}

#else

cbuffer ObjectCB : register(b2) {
    row_major float4x4 MVP;
    row_major float4x4 ModelMatrix;
    row_major float4x4 CurMVPNoJitter;
    row_major float4x4 PrevMVP;
};

GpuObjectTransforms LoadTransforms() {
    GpuObjectTransforms Out;
    Out.MVP            = MVP;
    Out.ModelMatrix    = ModelMatrix;
    Out.CurMVPNoJitter = CurMVPNoJitter;
    Out.PrevMVP        = PrevMVP;
    return Out;
}

#endif

struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

struct VSOutput {
    float4 pos         : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv          : TEXCOORD2;
    float4 curClip     : TEXCOORD3;
    float4 prevClip    : TEXCOORD4;
};

VSOutput main(VSInput input) {
    const GpuObjectTransforms T = LoadTransforms();

    VSOutput o;
    o.pos         = mul(float4(input.pos, 1.0f), T.MVP);
    o.worldPos    = mul(float4(input.pos, 1.0f), T.ModelMatrix).xyz;

    // Normal pela matriz de COFATORES (= det*inverse-transpose, sem o det que a normalizacao
    // do PS come): escala nao-uniforme no gizmo nao entorta mais a normal. Com escala
    // uniforme/rigida degenera exatamente no mul antigo. Convencao row-vector (mul(n, M)).
    const float3x3 M = (float3x3)T.ModelMatrix;
    const float3x3 CofM = float3x3(cross(M[1], M[2]), cross(M[2], M[0]), cross(M[0], M[1]));
    o.worldNormal = mul(input.normal, CofM);

    o.uv          = input.uv;
    o.curClip     = mul(float4(input.pos, 1.0f), T.CurMVPNoJitter);
    o.prevClip    = mul(float4(input.pos, 1.0f), T.PrevMVP);
    return o;
}
