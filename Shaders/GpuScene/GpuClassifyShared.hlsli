#ifndef SMILE_GPUSCENE_GPUCLASSIFYSHARED_HLSLI
#define SMILE_GPUSCENE_GPUCLASSIFYSHARED_HLSLI

// A DECISAO de classificacao, compartilhada pelo classify e pelo scatter (Fase 5).
//
// Por que ela nao mora dentro do classify: a contagem por lote RESERVA a posicao do comando, e o
// scatter e quem a preenche. Se o scatter avaliasse a mesma pergunta por conta propria, bastaria um
// termo a mais — ou a menos — para que um candidato contado nao recebesse argumento, e o
// `ExecuteIndirect` executaria ali o comando do frame passado. Duas copias da regra sao duas
// oportunidades de divergir; uma funcao so nao tem como.
//
// Espelha `GpuClassifyCandidate` (Smile/Graphics/Scene/GpuClassify.h) passo a passo, na MESMA ordem:
// slot -> meta -> ids -> linhas -> pool -> elegibilidade. Cada passo valida antes de usar o
// resultado do anterior.
//
// ⚠️ Este header DECLARA as tres tabelas nos registradores t1..t3. Quem o inclui herda esse layout —
// e e proposital: os dois passes leem as mesmas tabelas, e deixar cada um escolher o seu
// registrador seria abrir a porta para lerem tabelas diferentes.

#include "../Scene/GpuIndirectDraw.hlsli"
#include "../Scene/GpuSceneTables.hlsli"

// Desfechos. Indices fixos, espelhando EGpuClassifyOutcome; o teste WARP le os nove de volta.
#define SMILE_CLASSIFY_INDIRECT            0u
#define SMILE_CLASSIFY_NO_SLOT             1u
#define SMILE_CLASSIFY_INVALID_SLOT        2u
#define SMILE_CLASSIFY_INVALID_MATERIAL_ID 3u
#define SMILE_CLASSIFY_INVALID_MESH_ID     4u
#define SMILE_CLASSIFY_INVALID_POOL_ID     5u
#define SMILE_CLASSIFY_NOT_ELIGIBLE        6u
#define SMILE_CLASSIFY_MESH_UNUSABLE       7u
#define SMILE_CLASSIFY_OVERFLOW            8u
#define SMILE_CLASSIFY_OUTCOME_COUNT       9u

// Posicao sem candidato. TEM de valer o mesmo que o SMILE_EXPAND_NO_SLOT do produtor.
#define SMILE_CLASSIFY_NO_SLOT_ID 0xFFFFFFFFu

StructuredBuffer<GpuInstanceMeta>   InstanceMeta : register(t1);
StructuredBuffer<GpuMaterialRecord> Materials    : register(t2);
StructuredBuffer<GpuMeshRecord>     Meshes       : register(t3);

// O resultado de UM candidato: o desfecho e, sob INDIRECT, o COMANDO INTEIRO pronto para escrita.
//
// Inteiro, e nao ids. A primeira versao carregava `MeshId` e `MaterialId`, e o scatter precisaria
// ler `Meshes[MeshId]` para achar `IndexCount`, `IndexOffset` e `VertexOffset` — reabrindo
// exatamente a tabela onde uma validacao a mais caberia sem ninguem notar. Com o argumento completo
// aqui, o scatter nao tem o que ler: sob INDIRECT ele reserva a posicao e copia `Args`.
struct GpuClassifyResult {
    uint                Outcome;
    uint                Batch; // indice denso do lote; valido so sob INDIRECT
    GpuIndirectDrawArgs Args;  // o comando; valido so sob INDIRECT
};

// Indice denso do lote. CLASS-MAJOR, espelhando GpuBatchIndex do C++.
//
// ⚠️ Aritmetica u32, segura so depois do preflight: ele garante que
// `kGpuPipelineClassCount * PoolCount` cabe em u32, e o maior indice possivel e esse total menos um.
uint GpuBatchIndexFor(uint _PipelineClass, uint _GeometryPoolId, uint _PoolCount) {
    return _PipelineClass * _PoolCount + _GeometryPoolId;
}

GpuClassifyResult GpuClassifyOne(uint _Slot, uint _MetaCount, uint _MaterialCount,
                                 uint _MeshCount, uint _PoolCount) {
    GpuClassifyResult R;
    R.Outcome = SMILE_CLASSIFY_INDIRECT;
    R.Batch   = 0u;
    // Zerado por inteiro antes de qualquer saida antecipada. So e lido sob INDIRECT, mas um campo
    // nao inicializado numa struct HLSL e indefinido, e indefinido nao aparece em teste nenhum.
    R.Args.MaterialRow           = 0u;
    R.Args.InstanceSlot          = 0u;
    R.Args.IndexCountPerInstance = 0u;
    R.Args.InstanceCount         = 0u;
    R.Args.StartIndexLocation    = 0u;
    R.Args.BaseVertexLocation    = 0;
    R.Args.StartInstanceLocation = 0u;

    if (_Slot == SMILE_CLASSIFY_NO_SLOT_ID) { R.Outcome = SMILE_CLASSIFY_NO_SLOT;      return R; }
    if (_Slot >= _MetaCount)                { R.Outcome = SMILE_CLASSIFY_INVALID_SLOT; return R; }

    const GpuInstanceMeta Meta = InstanceMeta[_Slot];
    // Generation zero e linha VAZIA. O expand ja comparou a generation ESPERADA do candidato; o que
    // sobra aqui e perguntar se a linha tem ocupante.
    if (Meta.Generation == 0u) { R.Outcome = SMILE_CLASSIFY_INVALID_SLOT; return R; }

    // Os dois ids sao validados ANTES de qualquer leitura de linha. Sem isto o D3D12 devolve zero
    // para o Load fora do buffer, e o candidato passaria a usar o material 0 e a malha 0 — que sao
    // linhas PLAUSIVEIS (o default cinza e a malha ausente), nao lixo obvio.
    if (Meta.MaterialId >= _MaterialCount) {
        R.Outcome = SMILE_CLASSIFY_INVALID_MATERIAL_ID;
        return R;
    }
    if (Meta.MeshId >= _MeshCount) {
        R.Outcome = SMILE_CLASSIFY_INVALID_MESH_ID;
        return R;
    }

    const GpuMaterialRecord Material = Materials[Meta.MaterialId];
    const GpuMeshRecord     Mesh     = Meshes[Meta.MeshId];

    // Pool antes da elegibilidade: pool fora da lista e defeito de tabela, e reporta-lo como "nao
    // elegivel" o esconderia numa contagem que cresce por bons motivos.
    if (Mesh.GeometryPoolId >= _PoolCount) {
        R.Outcome = SMILE_CLASSIFY_INVALID_POOL_ID;
        return R;
    }

    // A PORTA e o predicado COMPLETO, e so ele decide. A decomposicao logo abaixo nunca decide nada:
    // ela apenas atribui o motivo do que ja foi recusado.
    if (!GpuIndirectEligible(Material.PipelineClass, Mesh.Flags)) {
        // Com os dois termos fechados, a CLASSE tem precedencia no relato: o desfecho e o mesmo
        // (fallback CPU), e isto so torna o motivo deterministico.
        R.Outcome = GpuPipelineClassIndirectEligible(Material.PipelineClass)
                        ? SMILE_CLASSIFY_MESH_UNUSABLE
                        : SMILE_CLASSIFY_NOT_ELIGIBLE;
        return R;
    }

    // O comando, campo a campo. Cada fonte e a UNICA possivel: a linha de material e o id que a meta
    // aponta, o slot e o proprio candidato, e os tres numeros de geometria vem da linha de malha —
    // cujos offsets ja foram garantidos representaveis e cuja contagem ja foi garantida inteira pela
    // mascara que a porta acabou de consultar.
    R.Batch = GpuBatchIndexFor(Material.PipelineClass, Mesh.GeometryPoolId, _PoolCount);
    R.Args.MaterialRow           = Meta.MaterialId;
    R.Args.InstanceSlot          = _Slot;
    R.Args.IndexCountPerInstance = Mesh.IndexCount;
    R.Args.InstanceCount         = 1u;
    R.Args.StartIndexLocation    = Mesh.IndexOffset;
    R.Args.BaseVertexLocation    = Mesh.VertexOffset;
    R.Args.StartInstanceLocation = 0u;
    return R;
}

#endif
