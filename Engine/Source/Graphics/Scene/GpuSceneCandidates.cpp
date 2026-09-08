#include "Smile/Graphics/Scene/GpuSceneCandidates.h"

#include "Smile/Scene/Scene.h"

namespace Smile {
    void FGpuSceneCandidates::SetCapacity(u32 _Capacity) {
        if (Capacity_ == _Capacity) return;
        Capacity_ = _Capacity;
        // A lista foi montada sob o teto ANTIGO. Nenhuma versao da cena mudou, entao o
        // RefreshIfStale nao teria como perceber — e aumentar o teto depois de um overflow
        // deixaria a lista truncada para sempre.
        Built_ = false;
        BuiltStructureVersion_     = ~0ull;
        BuiltStaticCastersVersion_ = ~0ull;
    }

    bool FGpuSceneCandidates::RefreshIfStale(const FScene& _Scene) {
        // Duas versoes, e nao tres. Estrutura muda quem existe; static casters muda visibilidade e
        // mobilidade, e visibilidade decide membresia. Transform nao muda quem e candidato — se
        // entrasse aqui, um arraste de gizmo reconstruiria a lista inteira por frame, que e
        // exatamente o custo que a lista existe para nao ter.
        if (Built_ && _Scene.StructureVersion() == BuiltStructureVersion_ &&
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

            // Conta ANTES do teto: `Candidates` responde "de quanto a cena precisa", e nao
            // "quanto coube". Sem isso, a capacidade necessaria nunca seria observavel — e ela e
            // o unico numero com que se conserta um overflow.
            ++Stats_.Candidates;
            if (Capacity_ != 0 && CandidateIds_.size() >= Capacity_) {
                ++Stats_.Overflow;
                continue;
            }
            CandidateIds_.push_back({ R.GpuHandle.Slot, R.GpuHandle.Generation });
        }

        if (Stats_.Candidates > HighWater_) HighWater_ = Stats_.Candidates;
        Built_ = true;

        // OVERFLOW NAO PRODUZ LISTA PARCIAL.
        //
        // Contar o descarte nao o torna conservador: uma lista truncada e utilizavel e geometria
        // sumindo em silencio — o modo de falha que a regra 6 do plano proibe. Ou a lista descreve
        // a cena inteira, ou ela nao existe, e `IsUsable()` manda o consumidor para o caminho CPU
        // (decisao D9). Uma lista vazia com a flag em falso falha ALTO; uma lista truncada falha
        // baixo, e e a baixa que passa despercebida.
        if (Stats_.Overflow > 0) {
            CandidateIds_.clear();
        } else if (Stats_.Candidates > 0) {
            // Cena plana: UM range cobrindo tudo. Lista vazia nao produz range vazio — um
            // consumidor que receba `{0,0}` teria de tratar o caso, e nao ter range nenhum ja e o
            // mesmo fato dito sem excecao.
            Ranges_.push_back({ 0u, Stats_.Candidates });
        }

        BuiltStructureVersion_     = _Scene.StructureVersion();
        BuiltStaticCastersVersion_ = _Scene.StaticCastersVersion();
    }

    void FGpuSceneCandidates::Reset() {
        CandidateIds_.clear();
        Ranges_.clear();
        Stats_ = {};
        HighWater_ = 0;
        Built_     = false;
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
                              std::span<const FGpuCandidateId> _CandidateIds,
                              std::vector<u32>& _OutSlots) {
        _OutSlots.clear();
        u32 Ignored = 0;
        for (const FInstanceRange& Range : _Ranges) {
            // Aritmetica em u64 de proposito: `FirstCandidate + CandidateCount` em u32 pode dar a
            // volta e transformar um range absurdo num range que cabe.
            const u64 Last = static_cast<u64>(Range.FirstCandidate) + Range.CandidateCount;
            if (Last > _CandidateIds.size()) { ++Ignored; continue; }
            for (u32 I = 0; I < Range.CandidateCount; ++I)
                _OutSlots.push_back(_CandidateIds[Range.FirstCandidate + I].Slot);
        }
        return Ignored;
    }
}
