#include "Smile/Graphics/Scene/GpuSceneCandidates.h"

#include "Smile/Scene/Scene.h"

namespace Smile {
    bool FGpuSceneCandidates::RefreshIfStale(const FScene& _Scene) {
        // Duas versoes, e nao tres. Estrutura muda quem existe; static casters muda visibilidade e
        // mobilidade, e visibilidade decide membresia. Transform nao muda quem e candidato — se
        // entrasse aqui, um arraste de gizmo reconstruiria a lista inteira por frame, que e
        // exatamente o custo que a lista existe para nao ter.
        if (_Scene.StructureVersion() == BuiltStructureVersion_ &&
            _Scene.StaticCastersVersion() == BuiltStaticCastersVersion_)
            return false;
        Rebuild(_Scene);
        return true;
    }

    void FGpuSceneCandidates::Rebuild(const FScene& _Scene) {
        CandidateIds_.clear();
        Ranges_.clear();
        Stats_ = {};

        const std::vector<FRenderable>& List = _Scene.Renderables();
        CandidateIds_.reserve(List.size());
        for (const FRenderable& R : List) {
            // A ordem dos testes e a ordem em que o contador precisa ser lido. `RaytracingOnly`
            // antes de residencia porque o proxy do terreno TEM residencia valida: contado como
            // slot invalido, ele apareceria como defeito de alocacao.
            if (R.RaytracingOnly)                  { ++Stats_.RaytracingOnly; continue; }
            if (!R.Visible)                        { ++Stats_.Hidden;         continue; }
            if (!R.Mesh || !R.Mesh->IsValid())     { ++Stats_.NoGeometry;     continue; }
            if (!_Scene.GpuSlots().IsAlive(R.GpuHandle)) { ++Stats_.InvalidSlot; continue; }

            if (Capacity_ != 0 && CandidateIds_.size() >= Capacity_) {
                ++Stats_.Overflow;
                continue;
            }
            CandidateIds_.push_back(R.GpuHandle.Slot);
        }

        Stats_.Candidates = static_cast<u32>(CandidateIds_.size());
        if (Stats_.Candidates > HighWater_) HighWater_ = Stats_.Candidates;

        // Cena plana: UM range cobrindo tudo. Lista vazia nao produz range vazio — um consumidor
        // que receba `{0,0}` teria de tratar o caso, e nao ter range nenhum ja e o mesmo fato dito
        // sem excecao.
        if (Stats_.Candidates > 0) Ranges_.push_back({ 0u, Stats_.Candidates });

        BuiltStructureVersion_     = _Scene.StructureVersion();
        BuiltStaticCastersVersion_ = _Scene.StaticCastersVersion();
    }

    void FGpuSceneCandidates::Reset() {
        CandidateIds_.clear();
        Ranges_.clear();
        Stats_ = {};
        HighWater_ = 0;
        BuiltStructureVersion_     = ~0ull;
        BuiltStaticCastersVersion_ = ~0ull;
    }

    u32 BuildRangeOutputBases(std::span<const FInstanceRange> _Ranges,
                              std::vector<u32>& _OutBases) {
        _OutBases.clear();
        _OutBases.reserve(_Ranges.size());
        u64 Total = 0;
        for (const FInstanceRange& Range : _Ranges) {
            _OutBases.push_back(static_cast<u32>(Total));
            Total += Range.CandidateCount;
        }
        // Saturado, e nao truncado: com mais de 2^32 saidas o total daria a volta e o dispatch
        // ficaria PEQUENO, o que faz geometria sumir. Nao acontece com as escalas do plano (1M),
        // e continua sendo a direcao certa do erro.
        return Total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(Total);
    }

    u32 ExpandCandidateRanges(std::span<const FInstanceRange> _Ranges,
                              std::span<const u32> _CandidateIds, std::vector<u32>& _OutSlots) {
        _OutSlots.clear();
        u32 Ignored = 0;
        for (const FInstanceRange& Range : _Ranges) {
            // Aritmetica em u64 de proposito: `FirstCandidate + CandidateCount` em u32 pode dar a
            // volta e transformar um range absurdo num range que cabe.
            const u64 Last = static_cast<u64>(Range.FirstCandidate) + Range.CandidateCount;
            if (Last > _CandidateIds.size()) { ++Ignored; continue; }
            for (u32 I = 0; I < Range.CandidateCount; ++I)
                _OutSlots.push_back(_CandidateIds[Range.FirstCandidate + I]);
        }
        return Ignored;
    }
}
