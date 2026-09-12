// Classificacao e contagem por lote (Fase 5 do GPU-DRIVEN-PLAN, passo 4 da secao 6.2).
//
// Le `ExpandedSlots` (saida do ExpandInstanceRanges) e escreve `BatchCounts` + `Counters`. Esta
// fatia so CONTA: o prefixo, a compactacao e a escrita dos argumentos vem depois, e nada consome
// estas saidas ainda.
//
// UMA THREAD POR CANDIDATO. A saida nao e posicional — dois candidatos do mesmo lote somam no MESMO
// slot —, entao aqui o InterlockedAdd e a operacao certa, e nao um contorno. A ordem nao importa
// porque o que se produz e um HISTOGRAMA.
//
// A regra do arquivo, espelhada do Smile/Graphics/Scene/GpuClassify.h: todo candidato incrementa
// EXATAMENTE um contador de desfecho. Quem vira comando incrementa tambem o lote. A soma dos
// desfechos tem de dar o numero de candidatos, e e isso que o teste WARP verifica.

#include "../Scene/GpuSceneTables.hlsli"

// Desfechos. Indices fixos, espelhando EGpuClassifyOutcome; o teste WARP le os nove de volta.
#define SMILE_CLASSIFY_INDIRECT            0u
#define SMILE_CLASSIFY_NO_SLOT             1u
#define SMILE_CLASSIFY_INVALID_SLOT        2u
#define SMILE_CLASSIFY_INVALID_MATERIAL_ID 3u
#define SMILE_CLASSIFY_INVALID_MESH_ID     4u
#define SMILE_CLASSIFY_INVALID_POOL_ID     5u
#define SMILE_CLASSIFY_NOT_ELIGIBLE        6u
#define SMILE_CLASSIFY_MESH_UNUSABLE     7u
#define SMILE_CLASSIFY_OVERFLOW            8u
#define SMILE_CLASSIFY_OUTCOME_COUNT       9u

// Posicao sem candidato. TEM de valer o mesmo que o SMILE_EXPAND_NO_SLOT do produtor; se divergirem,
// o buraco cai em INVALID_SLOT (0xFFFFFFFF nao e slot valido) em vez de NO_SLOT — nenhum comando
// errado sai dai, mas o contador mente. O teste alimenta um buraco de verdade justamente por isso.
#define SMILE_CLASSIFY_NO_SLOT_ID 0xFFFFFFFFu

cbuffer ClassifyCB : register(b0) {
    uint4 ClassifyParams;  // x=candidatos, y=linhas de meta, z=linhas de material, w=linhas de malha
    uint4 ClassifyParams2; // x=pools, y=capacidade de BatchCounts; zw reservados
};

StructuredBuffer<uint>              ExpandedSlots : register(t0);
StructuredBuffer<GpuInstanceMeta>   InstanceMeta  : register(t1);
StructuredBuffer<GpuMaterialRecord> Materials     : register(t2);
StructuredBuffer<GpuMeshRecord>     Meshes        : register(t3);

RWStructuredBuffer<uint> BatchCounts : register(u0);
RWStructuredBuffer<uint> Counters    : register(u1);

// Indice denso do lote. CLASS-MAJOR, espelhando GpuBatchIndex do C++.
uint BatchIndex(uint _PipelineClass, uint _GeometryPoolId, uint _PoolCount) {
    return _PipelineClass * _PoolCount + _GeometryPoolId;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    const uint Total         = ClassifyParams.x;
    const uint MetaCount     = ClassifyParams.y;
    const uint MaterialCount = ClassifyParams.z;
    const uint MeshCount     = ClassifyParams.w;
    const uint PoolCount     = ClassifyParams2.x;
    const uint BatchCapacity = ClassifyParams2.y;

    const uint Index = dtid.x;
    if (Index >= Total) return;

    const uint Slot = ExpandedSlots[Index];
    if (Slot == SMILE_CLASSIFY_NO_SLOT_ID) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_NO_SLOT], 1u);
        return;
    }
    if (Slot >= MetaCount) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_INVALID_SLOT], 1u);
        return;
    }

    const GpuInstanceMeta Meta = InstanceMeta[Slot];
    // Generation zero e linha VAZIA. O expand ja comparou a generation ESPERADA do candidato; o que
    // sobra aqui e perguntar se a linha tem ocupante.
    if (Meta.Generation == 0u) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_INVALID_SLOT], 1u);
        return;
    }

    // Os dois ids sao validados ANTES de qualquer leitura de linha. Sem isto o D3D12 devolve zero
    // para o Load fora do buffer, e o candidato passaria a usar o material 0 e a malha 0 — que sao
    // linhas PLAUSIVEIS (o default cinza e a malha ausente), nao lixo obvio.
    if (Meta.MaterialId >= MaterialCount) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_INVALID_MATERIAL_ID], 1u);
        return;
    }
    if (Meta.MeshId >= MeshCount) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_INVALID_MESH_ID], 1u);
        return;
    }

    const GpuMaterialRecord Material = Materials[Meta.MaterialId];
    const GpuMeshRecord     Mesh     = Meshes[Meta.MeshId];

    // Pool antes da elegibilidade: pool fora da lista e defeito de tabela, e reporta-lo como "nao
    // elegivel" o esconderia numa contagem que cresce por bons motivos.
    if (Mesh.GeometryPoolId >= PoolCount) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_INVALID_POOL_ID], 1u);
        return;
    }

    // A PORTA e o predicado COMPLETO, e so ele decide. A decomposicao logo abaixo nunca decide nada:
    // ela apenas atribui o motivo do que ja foi recusado. Partir a decisao em dois testes faria com
    // que, no dia em que um deles sumisse, a malha irrepresentavel recebesse comando com o offset
    // poison — e o command processor leria bytes de vertice como indices.
    if (!GpuIndirectEligible(Material.PipelineClass, Mesh.Flags)) {
        // Com os dois termos fechados, a CLASSE tem precedencia no relato: o desfecho e o mesmo
        // (fallback CPU), e isto so torna o motivo deterministico.
        const uint Motivo = GpuPipelineClassIndirectEligible(Material.PipelineClass)
                                ? SMILE_CLASSIFY_MESH_UNUSABLE
                                : SMILE_CLASSIFY_NOT_ELIGIBLE;
        InterlockedAdd(Counters[Motivo], 1u);
        return;
    }

    // Defesa de ultima instancia, e NAO politica. O `GpuClassifyPreflight` exige
    // `BatchCapacity >= kGpuPipelineClassCount * PoolCount` antes do dispatch, e com o `PoolCount`
    // validado logo acima o indice ja esta limitado — sob preflight aprovado este ramo e
    // inalcancavel. Ele fica porque a capacidade viaja separada do PoolCount ate aqui e os dois
    // podem discordar; descobrir falta de espaco NESTE ponto ja seria geometria perdida no frame, e
    // por isso quem impede e o preflight, nao este contador.
    const uint Batch = BatchIndex(Material.PipelineClass, Mesh.GeometryPoolId, PoolCount);
    if (Batch >= BatchCapacity) {
        InterlockedAdd(Counters[SMILE_CLASSIFY_OVERFLOW], 1u);
        return;
    }

    InterlockedAdd(BatchCounts[Batch], 1u);
    InterlockedAdd(Counters[SMILE_CLASSIFY_INDIRECT], 1u);
}
