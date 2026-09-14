#pragma once

// O layout de regioes VIVO do main view (Fase 5 do GPU-DRIVEN-PLAN): QUANDO reconstrui-lo.
//
// O layout em si (GpuClassify.h) e imutavel e sai de `GpuCountRegionCapacities` +
// `GpuBuildIndirectLayout`. Este objeto guarda o layout publicado e a fonte de versoes, e o
// reconstroi so quando as linhas PUBLICADAS mudaram o que a capacidade conta: ocupacao, associacao,
// classe, pool, `RaytracingOnly` ou mascara de malha inutilizavel. Transform, assentamento e
// visibilidade sozinhos nao o refazem — eles nao movem o `LayoutInputsVersion` de tabela nenhuma.
//
// ⚠️ Vive pelo RENDERER, e nao pela cena. A fonte de versoes nao pode recomecar numa troca de cena: a
// tabela da cena anterior, ainda num frame slot, coincidiria com o primeiro layout da seguinte.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Scene/GpuClassify.h"

#include <span>
#include <vector>

namespace Smile {
    // As entradas de um frame, lidas DEPOIS dos RecordUpload das tres tabelas: as linhas sao as
    // PUBLICADAS (`PublishedMeta`/`PublishedRows`), as mesmas que o classify desse frame le, e as
    // versoes sao os `LayoutInputsVersion()` delas.
    struct FGpuLayoutInputs {
        std::span<const FGpuSceneInstanceMeta> Meta;
        std::span<const FGpuMaterialRecord>    Materials;
        std::span<const FGpuMeshRecord>        Meshes;
        u32 PoolCount       = 0;
        u64 MetaVersion     = 0;
        u64 MaterialVersion = 0;
        u64 MeshVersion     = 0;
    };

    // Por que o layout do frame NAO existe. Recusado, `Layout()` tem versao 0 e nenhuma regiao — e as
    // portas da gravacao e das regioes o recusam de novo, sem precisar consultar este motivo.
    enum EGpuLayoutRefresh : u32 {
        GpuLayoutRefreshOk = 0,
        // Sem pool publicado: nao ha tabela de lotes (`GpuClassifyBatchBufferSlots`).
        GpuLayoutRefreshNoPools = 1,
        // Pools demais para a tabela de lotes.
        GpuLayoutRefreshTooManyPools = 2,
        // A soma das capacidades nao cabe em u32. Inalcancavel enquanto a meta for enderecada por slot
        // u32 — cada linha conta no maximo uma vez —; propagado, e nao presumido.
        GpuLayoutRefreshTooManyCommands = 3,
        // A fonte de versoes esgotou (`FGpuIndirectLayoutVersions`). Permanente.
        GpuLayoutRefreshVersionsExhausted = 4,
    };

    class FGpuIndirectLayoutState {
    public:
        // `_LastVersion` e a ultima versao ja entregue, como no `FGpuIndirectLayoutVersions`: existe
        // para o teste alcancar a exaustao.
        explicit FGpuIndirectLayoutState(u32 _LastVersion = 0u) : Versions_(_LastVersion) {}

        // Reconstroi o layout quando alguma das tres versoes, ou o numero de pools, difere da ultima
        // TENTATIVA — bem-sucedida ou nao. Devolve o motivo da tentativa vigente.
        //
        // Uma recusa nao e tentada de novo a cada frame: com as entradas iguais, o desfecho seria o
        // mesmo, e a tentativa custa uma varredura das linhas.
        //
        // O layout anterior deixa de valer ANTES da tentativa: uma recusa nao pode deixar a gravacao
        // com regioes contadas sobre outras linhas.
        u32 Refresh(const FGpuLayoutInputs& Inputs);

        const FGpuIndirectLayout& Layout() const { return Layout_; }
        u32 LastReason() const { return LastReason_; }
        // Quantas vezes as linhas foram recontadas. Telemetria, e o numero que prova que um transform
        // nao reconstroi nada.
        u64 RebuildAttempts() const { return RebuildAttempts_; }

    private:
        FGpuIndirectLayoutVersions Versions_;
        FGpuIndirectLayout         Layout_;
        std::vector<u64>           Capacities_;

        bool Attempted_       = false;
        u64  SeenMeta_        = 0;
        u64  SeenMaterial_    = 0;
        u64  SeenMesh_        = 0;
        u32  SeenPools_       = 0;
        u32  LastReason_      = GpuLayoutRefreshOk;
        u64  RebuildAttempts_ = 0;
    };
}
