#pragma once

#include <d3d12.h>
#include <memory>
#include <vector>
#include "Smile/Core/Types.h"

namespace Smile {
    // Fila COPY dedicada p/ upload de texturas/meshes: Begin -> gravar copias -> Submit,
    // que NAO bloqueia (fence propria; os stagings ficam retidos ate a GPU passar por
    // eles) — o CPU prepara o proximo batch enquanto a GPU copia o anterior, e a fila
    // direta nunca para. Chame WaitIdle() antes do primeiro consumo dos recursos na
    // fila direta (BLAS/frame). Substitui os ExecuteAndSync de upload (stall total).
    //
    // Estados: recurso usado na fila COPY decai pra COMMON quando o ExecuteCommandLists
    // termina, e PROMOVE implicitamente pro estado de leitura no primeiro uso na fila
    // direta — por isso os uploads daqui nao gravam transition barrier nenhuma (a COPY
    // queue nem aceita transicao pra estado de shader).
    // Uma fatia do ring de staging. NAO e dona: o buffer pertence ao ring, e o conteudo so
    // pode ser considerado vivo ate a fence do batch em que foi usada completar — que e
    // exatamente o contrato que a fila ja cumpria retendo o ComPtr do staging avulso.
    struct FStagingSlice {
        ID3D12Resource* Resource = nullptr; // origem do CopyTextureRegion/CopyBufferRegion
        u64             Offset   = 0;       // offset da fatia dentro do Resource
        u8*             Mapped   = nullptr; // ja apontando para Offset

        explicit operator bool() const { return Mapped != nullptr; }
    };

    class FUploadQueue {
    public:
        static constexpr u32 kSlots = 2; // batches em voo (Begin espera o slot mais velho)

        // Tamanho do chunk do ring. 64 MB cobre folgado o lote tipico de texturas de uma
        // cena (a Bistro sobe ~135 texturas num budget de 256 MB por batch) sem reservar
        // VRAM de sistema demais numa cena pequena — chunk novo so nasce sob demanda.
        static constexpr u64 kRingChunkBytes = 64ull * 1024 * 1024;

        // Chunks mantidos apos o WaitIdle. Um basta para o regime de repouso (upload avulso:
        // lua, weather map, textura trocada no editor de materiais); o pico do load volta
        // para o sistema em vez de virar residente. Ver TrimRing.
        static constexpr size_t kRingRetainChunks = 1;

        void Initialize(ID3D12Device* Device);
        void Shutdown();

        ID3D12GraphicsCommandList* Begin();

        // Fecha e submete o batch. Staging e retido ate a fence do batch completar
        // (liberado em Begin/WaitIdle futuros). Retorna o valor de fence do batch.
        u64  Submit(std::vector<ComPtr<ID3D12Resource>>&& Staging);
        u64  Submit() { return Submit({}); }

        void WaitIdle();

        // Sub-aloca staging do ring em vez de criar um committed UPLOAD por upload.
        //
        // Existe porque o caminho antigo criava UM recurso committed POR TEXTURA: numa cena
        // como a Emerald Square isso e um heap do driver por textura, criado e destruido no
        // load. Aqui o custo vira um bump de ponteiro, e o recurso subjacente e reciclado
        // assim que a fence do batch que o usou passa.
        //
        // Alignment default = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT porque o offset de um
        // PLACED_FOOTPRINT em CopyTextureRegion TEM que ser multiplo de 512; passar 4 (ou
        // qualquer coisa menor) faz a copia falhar na validacao, nao no resultado.
        //
        // A fatia vale ate o proximo Submit(): ela e emitida para o batch ABERTO, e o ring
        // so recicla a regiao depois que a fence daquele batch completa.
        FStagingSlice AllocateStaging(u64 Bytes,
                                      u64 Alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        // Chunks vivos e bytes que eles somam. So p/ log/diagnostico do load.
        u32 RingChunkCount() const { return static_cast<u32>(Chunks.size()); }
        u64 RingBytes() const;

    private:
        void CpuWait(u64 Value);
        void Retire();   // libera stagings cuja fence ja completou
        void TrimRing(); // devolve o pico de chunks do load (so com a GPU parada)

        // Fence que o Submit ainda vai sinalizar. Toda fatia entregue agora pertence a ele.
        u64 PendingFence() const { return FenceValue + 1; }

        struct FRetired {
            u64 FenceValue;
            std::vector<ComPtr<ID3D12Resource>> Staging;
        };

        // Um buffer UPLOAD persistente mapeado, consumido por bump. LastFence = o batch mais
        // recente que leu dele; enquanto ele nao completar, o chunk nao volta a ser usado do
        // inicio. Cursor so anda para a frente — reciclar e zerar o Cursor, e isso so
        // acontece depois do Retire.
        struct FRingChunk {
            ComPtr<ID3D12Resource> Resource;
            u8*                    Mapped    = nullptr;
            u64                    Capacity  = 0;
            u64                    Cursor    = 0;
            u64                    LastFence = 0;
        };

        FRingChunk* AcquireChunk(u64 MinBytes);

        ComPtr<ID3D12CommandQueue>        Queue;
        ComPtr<ID3D12CommandAllocator>    Allocators[kSlots];
        u64                               SlotFence[kSlots] = {};
        ComPtr<ID3D12GraphicsCommandList> List;
        ComPtr<ID3D12Fence>               Fence;
        HANDLE                            Event      = nullptr;
        u64                               FenceValue = 0;
        u32                               Slot       = 0;
        std::vector<FRetired>             Pending;

        ID3D12Device*                        Device_ = nullptr;
        std::vector<std::unique_ptr<FRingChunk>> Chunks;  // ponteiro estavel: Current aponta p/ um
        FRingChunk*                          Current = nullptr;
    };
}
