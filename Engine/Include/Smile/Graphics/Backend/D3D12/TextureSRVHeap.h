#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Scene/GpuSceneHandle.h"
#include <d3d12.h>
#include <initializer_list>
#include <span>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FTextureSRVHeap {
    public:
        static constexpr u32 kCapacity    = 16384;
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        void Initialize(ID3D12Device* Device);

        u32  Allocate(u32 Count = 1);
        void Free(u32 Slot, u32 Count = 1);
        void Release(u32& Slot, u32 Count = 1);

        // Libera um range SO DEPOIS que as duas filas passarem de Required.
        //
        // Por que existe (Fase 3 do GPU-DRIVEN-PLAN): descritor nao e dado, e escrever num slot
        // que uma command list ja submetida referencia e comportamento indefinido — nao "textura
        // errada por um frame". O `Free` normal devolve o range ao free list na hora, e o proximo
        // `Allocate` o reescreve; enquanto o unico caminho que soltava descritor era troca de
        // cena (com as filas drenadas), isso passava. A partir do momento em que o editor rotaciona
        // a tabela de um material com a GPU em voo, nao passa mais.
        //
        // Mesmo contrato do `FGpuSceneSlots::Retire`: Required tem de ser capturado numa
        // FRONTEIRA DE SUBMISSAO, com os ultimos valores realmente submetidos nas duas filas.
        void FreeDeferred(u32 Slot, u32 Count, const FGpuFenceSet& Required);

        // Devolve ao free list todo range aposentado cujas DUAS fences ja passaram. Retorna
        // quantos ranges voltaram. Custa O(aposentados) — no caso comum a lista esta vazia.
        u32 CollectRetired(const FGpuFenceSet& Completed);

        void CopyTable(ID3D12Device* Device, u32 DestinationSlot,
                       std::span<const u32> SourceSlots) const;
        void CopyTable(ID3D12Device* Device, u32 DestinationSlot,
                       std::initializer_list<u32> SourceSlots) const {
            CopyTable(Device, DestinationSlot,
                      std::span<const u32>(SourceSlots.begin(), SourceSlots.size()));
        }

        void CreateSRV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, u32 Slot);

        void CreateUAV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc, u32 Slot);

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(u32 Slot) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(u32 Slot) const;

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleStaging(u32 Slot) const;

        ID3D12DescriptorHeap* Native() const { return Heap.Get(); }

        // Ocupacao. High-water e por SESSAO (nunca decresce): o pico e o que decide se cabe
        // outro consumidor, e a instantanea sozinha esconde um pico entre trocas de cena. O
        // bindless da Fase 3 vai consumir deste mesmo orcamento, entao a folga precisa ser
        // observavel ANTES de se contar com ela.
        u32 UsedSlots()      const { return UsedSlots_; }
        u32 HighWaterSlots() const { return HighWaterSlots_; }
        u32 CapacitySlots()  const { return kCapacity; }
        // Slots presos em ranges aposentados esperando fence. Modo de falha e crescimento
        // silencioso: um Collect que deixasse de rodar consumiria o heap sem nada quebrar.
        u32 RetiredSlots()   const { return RetiredSlots_; }

    private:
        struct FFreeRange { u32 Offset; u32 Count; };
        // Range aposentado: as mesmas coordenadas do FFreeRange mais ate onde cada fila precisa
        // chegar antes de ele poder voltar ao free list.
        struct FRetiredRange { u32 Offset; u32 Count; FGpuFenceSet Required; };

        // So mexe no free list (insere ordenado e coalesce). O Free desconta de UsedSlots_ antes
        // de chamar; o CollectRetired nao, porque o aposentado ja saiu daquela conta.
        void InsertFreeRange(u32 Slot, u32 Count);

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Heap;        
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> StagingHeap; 
        u32 HandleSize = 0;
        std::vector<FFreeRange>    FreeList;
        std::vector<FRetiredRange> RetiredList;
        u32 UsedSlots_      = 0;
        u32 HighWaterSlots_ = 0;
        u32 RetiredSlots_   = 0;
    };
} 
