#include "Smile/Graphics/Scene/GpuIndirectLayoutState.h"

namespace Smile {
    void FGpuIndirectLayoutState::Reserve(u32 _PoolCount) {
        u32 Slots = 0;
        if (GpuClassifyBatchBufferSlots(_PoolCount, Slots) != GpuClassifyPreflightOk) return;
        Capacities_.reserve(Slots);
        Layout_.Reserve(Slots);
    }

    u32 FGpuIndirectLayoutState::Refresh(const FGpuLayoutInputs& _In) noexcept {
        if (Attempted_ && _In.MetaVersion == SeenMeta_ && _In.MaterialVersion == SeenMaterial_ &&
            _In.MeshVersion == SeenMesh_ && _In.PoolCount == SeenPools_)
            return LastReason_;

        // NO LUGAR: atribuir um layout vazio soltaria a reserva das regioes.
        Layout_.Invalidate();

        // A reserva ANTES de contar. Com ela, `GpuCountRegionCapacities` e `GpuBuildIndirectLayout`
        // escrevem dentro da capacidade que ja existe; sem ela, alocariam aqui dentro. A tentativa nao
        // e registrada: com as mesmas entradas, a chamada seguinte, depois do `Reserve`, reconta.
        u32 Slots = 0;
        if (GpuClassifyBatchBufferSlots(_In.PoolCount, Slots) == GpuClassifyPreflightOk &&
            (Slots > Capacities_.capacity() || Slots > Layout_.ReservedRegions())) {
            Attempted_ = false;
            return LastReason_ = GpuLayoutRefreshNotReserved;
        }

        Attempted_    = true;
        SeenMeta_     = _In.MetaVersion;
        SeenMaterial_ = _In.MaterialVersion;
        SeenMesh_     = _In.MeshVersion;
        SeenPools_    = _In.PoolCount;
        ++RebuildAttempts_;

        const u32 Contagem = GpuCountRegionCapacities(_In.Meta, _In.Materials, _In.Meshes,
                                                      _In.PoolCount, Capacities_);
        if (Contagem == GpuClassifyPreflightNoPools) return LastReason_ = GpuLayoutRefreshNoPools;
        if (Contagem != GpuClassifyPreflightOk) return LastReason_ = GpuLayoutRefreshTooManyPools;

        const u32 Montagem = GpuBuildIndirectLayout(Capacities_, Versions_.Next(), Layout_);
        if (Montagem == GpuRegionLayoutInvalidVersion)
            return LastReason_ = GpuLayoutRefreshVersionsExhausted;
        if (Montagem != GpuRegionLayoutOk) return LastReason_ = GpuLayoutRefreshTooManyCommands;
        return LastReason_ = GpuLayoutRefreshOk;
    }
}
