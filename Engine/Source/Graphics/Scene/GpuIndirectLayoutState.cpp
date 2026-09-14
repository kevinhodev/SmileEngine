#include "Smile/Graphics/Scene/GpuIndirectLayoutState.h"

namespace Smile {
    u32 FGpuIndirectLayoutState::Refresh(const FGpuLayoutInputs& _In) {
        if (Attempted_ && _In.MetaVersion == SeenMeta_ && _In.MaterialVersion == SeenMaterial_ &&
            _In.MeshVersion == SeenMesh_ && _In.PoolCount == SeenPools_)
            return LastReason_;

        Attempted_    = true;
        SeenMeta_     = _In.MetaVersion;
        SeenMaterial_ = _In.MaterialVersion;
        SeenMesh_     = _In.MeshVersion;
        SeenPools_    = _In.PoolCount;
        ++RebuildAttempts_;

        Layout_ = FGpuIndirectLayout{};

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
