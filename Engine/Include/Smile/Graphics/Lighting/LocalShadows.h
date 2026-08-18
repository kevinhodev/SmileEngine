#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <functional>
#include <vector>
#include <cassert>

namespace Smile {
    class FTextureSRVHeap;
    class FGpuMesh;
    class FMaterial;

    // Sombras das luzes LOCAIS. Modelado no FSunShadows; mesmos shaders de depth do CSM
    // (ShadowDepth.vs/ps: alpha-test de folhagem de graca). Dois caminhos:
    //  - SPOT (F3a): Texture2DArray D32 1024^2, um slice por luz (budget kMaxShadows/frame),
    //    projecao fov = 2*coneExterno / far = raio; o deferred lighting le t18 e faz PCF 3x3
    //    com a matriz world->UVZ que viaja no FGPULight (slice em SpotParams.y; -1 = sem).
    //  - POINT (F3b): TextureCubeArray D32 512^2 x kMaxCubeShadows (6 faces de 90 graus por
    //    luz, convencao de faces D3D). O lookup nao usa matriz: o vetor luz->pixel escolhe a
    //    face no hardware e a profundidade de referencia sai do EIXO DOMINANTE (mesma
    //    projecao das faces) — t19, indice do cubo em SpotParams.y.

    // Tabela de slices chaveada pela IDENTIDADE da luz (FLight::Id), nao pela posicao no ranking.
    // Modelo do Flax (ShadowsPass mantem Dictionary<Guid, ShadowAtlasLight> com LastFrameUsed
    // e reciclagem por frame) e do shadow pool da Cry. Antes o slice era o indice do sort por
    // distancia: a camera andar um metro reordenava o ranking e a mesma luz caia noutro slice.
    // Isso (a) impede cachear o depth entre frames — o conteudo do slice nao pertence a
    // ninguem — e (b) impede histerese/fade, porque nao da pra perguntar "esta luz tinha
    // sombra no frame passado?". Slot estavel e pre-requisito das duas coisas.
    template <u32 N>
    struct TShadowSlotCache {
        static constexpr u32 kNoSlot = 0xFFFFFFFFu;

        // Resolve a selecao INTEIRA de uma vez, em DUAS FASES: donos atuais reservam o proprio
        // slice, e so entao os novatos ocupam o que estiver LIVRE. A API e em lote de proposito
        // — resolvendo luz a luz, o novato processado primeiro rouba o slot de um selecionado,
        // que vira novato e rouba o do seguinte, remapeando todos em cascata.
        //
        // A liberacao e por FADE, nunca por evicao: ver a fase 2 e o UpdateFades.
        void AcquireBatch(const u64* _LightIds, u32 _Count, u32* _OutSlots) {
            // Id 0 = "sem identidade" (colide com slot livre) e Id repetido faz duas luzes
            // convergirem pro mesmo slice — uma sobrescreveria a sombra da outra em silencio.
            // Os caminhos de criacao de hoje garantem os dois, mas FScene::Lights() devolve o
            // vetor mutavel, entao uma copia direta futura reintroduziria Id duplicado sem
            // aviso. O(n^2) sobre <= 8 entradas, so em Debug.
#ifndef NDEBUG
            for (u32 a = 0; a < _Count; ++a) {
                assert(_LightIds[a] != 0 && "FLight::Id nao atribuido antes do AcquireBatch");
                for (u32 b = a + 1; b < _Count; ++b)
                    assert(_LightIds[a] != _LightIds[b] && "FLight::Id duplicado no lote");
            }
#endif
            // Fase 1 — dono atual mantem o proprio slice, independente da ordem no lote.
            for (u32 c = 0; c < _Count; ++c) {
                _OutSlots[c] = kNoSlot;
                for (u32 i = 0; i < N; ++i)
                    if (Owner[i] == _LightIds[c]) { _OutSlots[c] = i; break; }
            }

            // Fase 2 — novato so ocupa slot LIVRE. NAO existe evicao direta: quem perde a
            // selecao apenas sai de `active`, o UpdateFades derruba o fade dele e o slot se
            // libera sozinho ao chegar em zero. Sobrescrever o perdedor aqui apagava o dono
            // ANTES de qualquer fade — dava fade-in no novato e sumico instantaneo na luz
            // removida, que e exatamente o pop que isto existe pra matar, no caso mais comum
            // (orcamento cheio). O preco e o novato esperar a transicao pra ganhar sombra.
            for (u32 c = 0; c < _Count; ++c) {
                if (_OutSlots[c] != kNoSlot) continue;
                for (u32 i = 0; i < N; ++i) {
                    if (Owner[i] != 0) continue;
                    Owner[i]     = _LightIds[c];
                    Fade[i]      = 0.0f; // dono novo entra fazendo fade-in
                    _OutSlots[c] = i;
                    break;
                }
            }
        }

        bool Owns(u64 _LightId) const {
            for (u32 i = 0; i < N; ++i) if (Owner[i] == _LightId) return true;
            return false;
        }

        // Avanca o fade de cada slot: sobe pra 1 se o dono esta na lista ativa, desce pra 0 se
        // nao (a luz saiu da selecao). Slot que chega a 0 e liberado — e assim que uma luz
        // "aposentada" devolve o slice sem que ninguem precise expulsa-la.
        // Chamar DEPOIS do AcquireBatch do frame.
        void UpdateFades(const u64* _ActiveIds, u32 _Count, f32 _Dt, f32 _Rate) {
            const f32 Step = _Dt * _Rate;
            for (u32 i = 0; i < N; ++i) {
                if (Owner[i] == 0) { Fade[i] = 0.0f; continue; }
                bool Active = false;
                for (u32 c = 0; c < _Count && !Active; ++c) Active = (_ActiveIds[c] == Owner[i]);
                if (Active) {
                    Fade[i] = Fade[i] + Step >= 1.0f ? 1.0f : Fade[i] + Step;
                } else {
                    Fade[i] = Fade[i] - Step <= 0.0f ? 0.0f : Fade[i] - Step;
                    if (Fade[i] <= 0.0f) Owner[i] = 0; // terminou de sumir: slot livre
                }
            }
        }

        u64 OwnerAt(u32 _Slot) const { return Owner[_Slot]; }
        f32 FadeAt(u32 _Slot)  const { return Fade[_Slot]; }

        // Troca de cena: os Ids antigos morreram junto com as luzes.
        void Reset() {
            for (u32 i = 0; i < N; ++i) { Owner[i] = 0; Fade[i] = 0.0f; }
        }

        u64 Owner[N] = {}; // FLight::Id do dono (0 = livre)
        f32 Fade[N]  = {}; // 0 = sem sombra, 1 = cheia. Vive no SLOT: e ele que guarda o depth
    };

    class FLocalShadows {
    public:
        // Slices FISICOS vs teto de SELECAO: sempre sobra um slice de cada tipo como espaco de
        // manobra. Sem ele a troca e sequencial — o perdedor precisa terminar de sumir pra
        // liberar o slot, e a luz promovida fica ~250 ms sem sombra (vazando parede) justo
        // quando a camera chegou perto dela. Com o extra, o novato entra no slice livre e faz
        // fade-in enquanto o perdedor faz fade-out no dele: cross-fade de verdade.
        // O extra atende UMA transicao por tipo. Com duas ou mais substituicoes no mesmo frame
        // TODOS os perdedores comecam o fade-out juntos, mas so o primeiro novato acha slice
        // livre pra cruzar com eles; os outros entram conforme os slices se liberam e portanto
        // fazem fade-in SEM o fade-out correspondente (o perdedor deles ja terminou de sumir).
        // E o fallback sequencial esperado com uma reserva so — mantem VRAM e custo previsiveis.
        static constexpr u32 kMaxShadows     = 9; // 8 ativos + 1 de transicao  (36 MiB)
        static constexpr u32 kActiveShadows  = 8;
        static constexpr u32 kResolution     = 1024;
        static constexpr u32 kMaxCubeShadows = 5; // 4 ativos + 1 de transicao  (30 MiB)
        static constexpr u32 kActiveCubes    = 4;
        static constexpr u32 kCubeResolution = 512;
        static constexpr u32 kNoSlot         = 0xFFFFFFFFu; // Acquire*: orcamento cheio

        // Politica de selecao (item 4). Incumbente vale kHysteresis a mais no ranking: sem isso
        // duas luzes de importancia parecida trocam de slot todo frame e a sombra pisca — o
        // oposto do que a estabilidade de slot veio dar. kFadeRate = 1/segundos da transicao;
        // 4 = 250 ms, curto o bastante pra nao parecer bug e longo pra matar o pop.
        static constexpr f32 kHysteresis     = 1.5f;
        static constexpr f32 kFadeRate       = 4.0f;
        static constexpr f32 kPointNear      = 0.05f; // near de TODAS as projecoes locais (spot e
                                                      // faces do cubo) — o shader reconstroi o refZ
                                                      // linear com ele (LightParams2.y)

        struct FShadowDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
        };

        // Um slice de SPOT a renderizar neste frame: matriz da luz + esfera de influencia
        // (broad phase dos casters; o frustum do cone refina em cima da lista curta).
        // Slice vem do TShadowSlotCache — e a identidade da luz, nao a posicao no ranking.
        struct FShadowJob {
            Mat44 ViewProj;
            Vec3  LightPos;
            f32   Radius;
            u32   Slice;
        };

        // Um cubo de POINT a renderizar (6 faces): so posicao + raio; as matrizes das faces
        // sao fixas por convencao (D3D cube faces) e construidas aqui dentro.
        struct FCubeShadowJob {
            Vec3 LightPos;
            f32  Radius;
            u32  CubeIndex;
        };

        // Caster extra por slice/face (terreno) — chamado depois dos itens, com o CB e a
        // matriz daquela view. Pode trocar root signature/PSO: o loop re-liga os desta
        // classe no inicio do proximo slice (mesmo contrato do FSunShadows).
        // Recebe TAMBEM a esfera de influencia da luz porque o frustum sozinho nao basta como
        // filtro: num spot de 89 graus ele abre ~57R lateralmente no far plane, enquanto a luz
        // morre radialmente em R. Sem a esfera o caster extra desenharia geometria a dezenas
        // de raios de distancia, que nunca vai iluminar nada.
        using FExtraLocalDraw = std::function<void(ID3D12GraphicsCommandList*,
                                                   D3D12_GPU_VIRTUAL_ADDRESS SliceCB,
                                                   const Mat44& SliceViewProj,
                                                   const Vec3& LightPos, f32 Radius)>;

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        bool IsInitialized() const { return Initialized; }

        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             u32 FrameSlot, const FShadowDrawItem* Items, size_t Count,
                             const FShadowJob* Jobs, u32 JobCount,
                             const FCubeShadowJob* CubeJobs, u32 CubeJobCount,
                             const FExtraLocalDraw& ExtraDraw = {});

        // Slot persistente por luz (ver TShadowSlotCache). Em LOTE: a selecao inteira do frame
        // de uma vez, senao a alocacao remapeia em cascata. Slot kNoSlot = ficou sem sombra.
        void AcquireSpotSlots(const u64* LightIds, u32 Count, u32* OutSlots) {
            SpotSlots.AcquireBatch(LightIds, Count, OutSlots);
        }
        void AcquireCubeSlots(const u64* LightIds, u32 Count, u32* OutSlots) {
            CubeSlots.AcquireBatch(LightIds, Count, OutSlots);
        }
        void ResetSlots() { SpotSlots.Reset(); CubeSlots.Reset(); }

        // Consulta p/ a selecao (histerese) e p/ varrer os slots em fade-out.
        bool SpotOwns(u64 LightId) const { return SpotSlots.Owns(LightId); }
        bool CubeOwns(u64 LightId) const { return CubeSlots.Owns(LightId); }
        u64  SpotOwnerAt(u32 Slot) const { return SpotSlots.OwnerAt(Slot); }
        u64  CubeOwnerAt(u32 Slot) const { return CubeSlots.OwnerAt(Slot); }
        f32  SpotFadeAt(u32 Slot)  const { return SpotSlots.FadeAt(Slot); }
        f32  CubeFadeAt(u32 Slot)  const { return CubeSlots.FadeAt(Slot); }

        // O slice em fade-out CONTINUA sendo redesenhado enquanto a luz estiver visivel (o
        // Renderer emite job pra ele tambem). E o que mantem o mapa valido se a luz ou os
        // casters se moverem durante a transicao — sem isso seria preciso congelar a matriz e a
        // pose de captura e abortar o fade quando divergissem, o que ainda deixava o caster
        // movido errado. O custo e um slice a mais (dai o extra em kMaxShadows).
        void UpdateSpotFades(const u64* ActiveIds, u32 Count, f32 Dt) {
            SpotSlots.UpdateFades(ActiveIds, Count, Dt, kFadeRate);
        }
        void UpdateCubeFades(const u64* ActiveIds, u32 Count, f32 Dt) {
            CubeSlots.UpdateFades(ActiveIds, Count, Dt, kFadeRate);
        }

        // Garante os arrays legiveis (PIXEL_SHADER_RESOURCE) mesmo em frame sem job novo.
        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);
        // Leitura tambem em compute (volumetric fog): PIXEL | NON_PIXEL.
        void EnsureReadableCompute(ID3D12GraphicsCommandList* CommandList);

        // Slot do SRV do atlas de spot (t18); o SRV do cube array (t19) vive em Slot+1 —
        // a tabela do root param cobre os dois de uma vez (descritores contiguos).
        u32 ShadowSRVSlot() const { return ShadowSRVSlot_; }

        f32  GetDepthBias() const { return DepthBias; }
        void SetDepthBias(f32 B)  { DepthBias = B; }

    private:
        void CreateResources(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);
        void TransitionArray(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);
        void TransitionCube(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

        Microsoft::WRL::ComPtr<ID3D12Resource>      DepthArray;
        FDescriptorHeap                             DSVHeap;
        u32                                         ShadowSRVSlot_ = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES                       ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12Resource>      CubeArray;   // 6*kMaxCubeShadows slices
        FDescriptorHeap                             CubeDSVHeap;
        D3D12_RESOURCE_STATES                       CubeState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OpaquePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskedPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource>      SliceCB; // Mat44 por slice por frame em voo
        u8*                                         MappedSlice = nullptr;

        TShadowSlotCache<kMaxShadows>               SpotSlots;
        TShadowSlotCache<kMaxCubeShadows>           CubeSlots;
        // Broad phase por luz: indices em Items dentro da esfera de influencia. Membro (e nao
        // local) so pra nao realocar por slice — as 6 faces do point reusam a MESMA lista.
        std::vector<u32>                            CullScratch;

        f32  DepthBias   = 0.02f; // bias constante em METROS (o shader converte pra NDC pelo
                                  // caminho linear; + termo relativo por distancia no shader).
                                  // Bias NDC constante em perspectiva = peter-panning na borda.
        bool Initialized = false;
    };
}
