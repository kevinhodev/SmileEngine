#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
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

    // Alocador LRU de slices por IDENTIDADE de luz (FLight::Id), nao por posicao no ranking.
    // Modelo do Flax (ShadowsPass mantem Dictionary<Guid, ShadowAtlasLight> com LastFrameUsed
    // e reciclagem por frame) e do shadow pool da Cry. Antes o slice era o indice do sort por
    // distancia: a camera andar um metro reordenava o ranking e a mesma luz caia noutro slice.
    // Isso (a) impede cachear o depth entre frames — o conteudo do slice nao pertence a
    // ninguem — e (b) impede histerese/fade, porque nao da pra perguntar "esta luz tinha
    // sombra no frame passado?". Slot estavel e pre-requisito das duas coisas.
    template <u32 N>
    struct TShadowSlotCache {
        static constexpr u32 kNoSlot = 0xFFFFFFFFu;

        // Resolve a selecao INTEIRA de uma vez, em DUAS FASES. A API e em lote de proposito:
        // resolver luz a luz remapeia em cascata e destroi a estabilidade que este cache
        // existe pra dar. Com slots [A,B,C,D] e selecao [E,A,B,C], o novato E entra primeiro,
        // nao acha slot livre e rouba o LRU — que e o de A. Ai A tambem virou novato, rouba o
        // de B, B rouba o de C, C rouba o de D: uma entrada remapeou os quatro. Reservando
        // todos os donos ANTES de alocar qualquer novato, a evicao so alcanca luz que nao
        // esta selecionada neste frame, e quem sobrevive fica no mesmo slice.
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
            // Epoch PROPRIO em vez do FrameIndex do renderer: nao acopla o cache ao contador de
            // frames (que e u32 e daria wrap) e a unica coisa que o LRU precisa e uma ordem
            // monotonica das chamadas. 0 fica reservado p/ "slot nunca usado".
            const u64 Stamp = ++Epoch;

            // Fase 1 — donos atuais reservam o proprio slice e ficam protegidos (LastUse = Stamp).
            for (u32 c = 0; c < _Count; ++c) {
                _OutSlots[c] = kNoSlot;
                for (u32 i = 0; i < N; ++i)
                    if (Owner[i] == _LightIds[c]) { LastUse[i] = Stamp; _OutSlots[c] = i; break; }
            }

            // Fase 2 — novatos pegam slot virgem ou o LRU entre os NAO reservados.
            for (u32 c = 0; c < _Count; ++c) {
                if (_OutSlots[c] != kNoSlot) continue;
                u32 Best   = kNoSlot;
                u64 Oldest = ~0ull;
                for (u32 i = 0; i < N; ++i) {
                    if (Owner[i] == 0) { Best = i; break; }   // slot virgem: pega direto
                    if (LastUse[i] == Stamp) continue;        // reservado neste frame: intocavel
                    if (LastUse[i] < Oldest) { Oldest = LastUse[i]; Best = i; }
                }
                if (Best == kNoSlot) continue;                // orcamento cheio: fica sem sombra
                Owner[Best]   = _LightIds[c];
                LastUse[Best] = Stamp;
                _OutSlots[c]  = Best;
            }
        }

        // Troca de cena: os Ids antigos morreram junto com as luzes.
        void Reset() {
            for (u32 i = 0; i < N; ++i) { Owner[i] = 0; LastUse[i] = 0; }
        }

        u64 Owner[N]   = {}; // FLight::Id do dono (0 = livre)
        u64 LastUse[N] = {};
        u64 Epoch      = 0;
    };

    class FLocalShadows {
    public:
        static constexpr u32 kMaxShadows     = 8;
        static constexpr u32 kResolution     = 1024;
        static constexpr u32 kMaxCubeShadows = 4;
        static constexpr u32 kCubeResolution = 512;
        static constexpr u32 kNoSlot         = 0xFFFFFFFFu; // Acquire*: orcamento cheio
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
