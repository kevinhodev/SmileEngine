#include "Smile/Graphics/UploadQueue.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <string>

namespace Smile {
    void FUploadQueue::Initialize(ID3D12Device* _Device) {
        Device_ = _Device;

        D3D12_COMMAND_QUEUE_DESC Desc{};
        Desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        SMILE_HR(_Device->CreateCommandQueue(&Desc, IID_PPV_ARGS(&Queue)));

        for (u32 i = 0; i < kSlots; ++i)
            SMILE_HR(_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                     IID_PPV_ARGS(&Allocators[i])));
        SMILE_HR(_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                            Allocators[0].Get(), nullptr, IID_PPV_ARGS(&List)));
        SMILE_HR(List->Close());

        SMILE_HR(_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!Event) {
            SMILE_HR(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void FUploadQueue::Shutdown() {
        if (Fence) WaitIdle();
        Pending.clear();
        // Unmap explicito antes de soltar o ComPtr: o mapeamento do ring e persistente, e
        // liberar o recurso mapeado deixaria o range aberto ate o driver recolher.
        for (auto& Chunk : Chunks)
            if (Chunk->Mapped) { Chunk->Resource->Unmap(0, nullptr); Chunk->Mapped = nullptr; }
        Chunks.clear();
        Current = nullptr;
        if (Event) { CloseHandle(Event); Event = nullptr; }
    }

    u64 FUploadQueue::RingBytes() const {
        u64 Total = 0;
        for (const auto& Chunk : Chunks) Total += Chunk->Capacity;
        return Total;
    }

    FUploadQueue::FRingChunk* FUploadQueue::AcquireChunk(u64 _MinBytes) {
        const u64 Done = Fence ? Fence->GetCompletedValue() : 0;

        // Reciclagem: chunk cuja ultima fence ja completou volta com o cursor zerado. So o
        // Cursor volta, nao o recurso — o mapeamento e a alocacao continuam de pe.
        for (auto& Chunk : Chunks) {
            if (Chunk.get() == Current) continue;
            if (Chunk->LastFence > Done) continue;   // GPU ainda pode estar lendo
            if (Chunk->Capacity < _MinBytes) continue;
            Chunk->Cursor = 0;
            return Chunk.get();
        }

        // Nenhum servia: nasce um chunk novo. Uma alocacao maior que o chunk padrao ganha um
        // chunk sob medida em vez de estourar — e o equivalente do "stand alone" da Unreal
        // para alocacao acima do maximo do pool.
        //
        // O ponteiro mapeado vem do proprio CreateUploadBuffer, que ja entrega o recurso
        // mapeado (map persistente e o ponto dele). Mapear de novo aqui seria legal — Map e
        // contado por referencia em D3D12 — mas deixaria uma referencia de mapeamento viva
        // que o unico Unmap do teardown nao fecha.
        auto New      = std::make_unique<FRingChunk>();
        New->Capacity = std::max(_MinBytes, kRingChunkBytes);
        const GpuResources::FUploadBuffer Buffer =
            GpuResources::CreateUploadBuffer(Device_, New->Capacity, 1, false);
        New->Resource = Buffer.Resource;
        New->Mapped   = Buffer.Mapped;

        FRingChunk* Out = New.get();
        Chunks.push_back(std::move(New));
        LogDebug("[Upload] chunk novo no ring de staging (" + std::to_string(Chunks.size()) +
                 " no total, " + std::to_string(RingBytes() >> 20) + " MB)");
        return Out;
    }

    FStagingSlice FUploadQueue::AllocateStaging(u64 _Bytes, u64 _Alignment) {
        if (_Bytes == 0 || !Device_) return {};
        if (_Alignment == 0) _Alignment = 1;

        if (Current) {
            const u64 Aligned = (Current->Cursor + _Alignment - 1) & ~(_Alignment - 1);
            if (Aligned + _Bytes <= Current->Capacity) {
                Current->Cursor    = Aligned + _Bytes;
                Current->LastFence = PendingFence();
                return { Current->Resource.Get(), Aligned, Current->Mapped + Aligned };
            }
        }

        // Nao coube: o chunk atual vai inteiro para o batch aberto e pegamos outro. O
        // alinhamento entra no pedido porque o chunk novo comeca no cursor 0, que ja e
        // 64 KB-alinhado — mas pedir Bytes+Alignment mantem a conta valida se um dia o
        // inicio deixar de ser.
        Current = AcquireChunk(_Bytes + _Alignment);
        const u64 Aligned  = (Current->Cursor + _Alignment - 1) & ~(_Alignment - 1);
        Current->Cursor    = Aligned + _Bytes;
        Current->LastFence = PendingFence();
        return { Current->Resource.Get(), Aligned, Current->Mapped + Aligned };
    }

    ID3D12GraphicsCommandList* FUploadQueue::Begin() {
        CpuWait(SlotFence[Slot]); // allocator do slot so pode resetar com a GPU alem dele
        Retire();
        SMILE_HR(Allocators[Slot]->Reset());
        SMILE_HR(List->Reset(Allocators[Slot].Get(), nullptr));
        return List.Get();
    }

    u64 FUploadQueue::Submit(std::vector<ComPtr<ID3D12Resource>>&& _Staging) {
        SMILE_HR(List->Close());
        ID3D12CommandList* Lists[] = { List.Get() };
        Queue->ExecuteCommandLists(1, Lists);
        ++FenceValue;
        SMILE_HR(Queue->Signal(Fence.Get(), FenceValue));
        SlotFence[Slot] = FenceValue;
        Slot = (Slot + 1) % kSlots;
        if (!_Staging.empty())
            Pending.push_back({ FenceValue, std::move(_Staging) });
        return FenceValue;
    }

    void FUploadQueue::WaitIdle() {
        CpuWait(FenceValue);
        Retire();
        TrimRing();
    }

    void FUploadQueue::TrimRing() {
        // O staging antigo era um committed POR TEXTURA e morria junto com o batch; o ring,
        // se ninguem podar, guardaria o PICO do load para sempre. Um lote de 256 MB sobre
        // chunks de 64 MB deixaria ~4 chunks (mais os do batch anterior em voo) presos em
        // memoria de sistema depois que a cena terminou de carregar.
        //
        // So roda no WaitIdle, que e onde a GPU esta comprovadamente parada — e onde o load
        // termina (o SceneLoader espera antes do primeiro consumo). Durante o laco de batches
        // do CreateBatchFromCPU nao ha WaitIdle, entao a poda nao interfere no overlap.
        if (Chunks.size() <= kRingRetainChunks) return;

        const u64 BytesBefore = RingBytes();
        for (size_t i = Chunks.size(); i-- > kRingRetainChunks; ) {
            if (Chunks[i]->Mapped) Chunks[i]->Resource->Unmap(0, nullptr);
            Chunks.erase(Chunks.begin() + static_cast<ptrdiff_t>(i));
        }
        // O Current quase certamente foi podado (era o ultimo em uso). Reapontar aqui e nao
        // deixar pendurado e o que impede um ponteiro morto na proxima alocacao.
        Current = Chunks.empty() ? nullptr : Chunks.front().get();
        if (Current) Current->Cursor = 0;

        LogDebug("[Upload] ring podado para " + std::to_string(Chunks.size()) + " chunk(s), " +
                 std::to_string((BytesBefore - RingBytes()) >> 20) + " MB devolvidos");
    }

    void FUploadQueue::CpuWait(u64 _Value) {
        if (_Value == 0 || Fence->GetCompletedValue() >= _Value) return;
        SMILE_HR(Fence->SetEventOnCompletion(_Value, Event));
        WaitForSingleObject(Event, INFINITE);
    }

    void FUploadQueue::Retire() {
        const u64 Done = Fence->GetCompletedValue();
        Pending.erase(std::remove_if(Pending.begin(), Pending.end(),
                                     [Done](const FRetired& R) { return R.FenceValue <= Done; }),
                      Pending.end());

        // Chunk cujo ultimo batch completou volta ao cursor 0 — inclusive o Current, que e o
        // caso comum: o load faz WaitIdle entre fases, e sem isto o chunk aberto seguiria
        // sangrando capacidade batch a batch ate forcar um chunk novo sem necessidade.
        // Seguro aqui e so aqui: Retire() so roda em Begin() (batch anterior ja submetido) e
        // em WaitIdle(), nunca no meio da gravacao de um batch.
        for (auto& Chunk : Chunks)
            if (Chunk->LastFence <= Done) Chunk->Cursor = 0;
    }
}
