#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    // Heap de descritores CBV/SRV/UAV shader-visible com um allocator de free-list:
    // Allocate carve blocos contiguos a partir de ranges livres (first-fit) e Free
    // devolve + coalesce ranges adjacentes. Permite reuso de slots quando texturas/
    // materiais sao recarregados em runtime (antes era bump-only e vazava slots).
    class FTextureSRVHeap {
    public:
        static constexpr u32 kCapacity = 512;

        void Initialize(ID3D12Device* Device);

        // Reserva um bloco contiguo de Count slots. Lanca se nao houver espaco.
        u32  Allocate(u32 Count = 1);

        // Devolve um bloco previamente alocado (mesmo Count usado no Allocate).
        void Free(u32 Slot, u32 Count = 1);

        void CreateSRV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, u32 Slot);

        void CreateUAV(ID3D12Device* Device, ID3D12Resource* Resource,
                       const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc, u32 Slot);

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(u32 Slot) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(u32 Slot) const;

        // Handle no heap "staging" (NAO shader-visible), espelho do mesmo slot. Usar como
        // FONTE de CopyDescriptors — ler do heap shader-visible (CPU-write-only) e invalido.
        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleStaging(u32 Slot) const;

        ID3D12DescriptorHeap* Native() const { return Heap.Get(); }

    private:
        // Range contiguo de slots livres [Offset, Offset+Count).
        struct FFreeRange { u32 Offset; u32 Count; };

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Heap;        // shader-visible (GPU)
        // Espelho CPU-only: cada CreateSRV/CreateUAV escreve nos dois. Serve de fonte
        // valida para CopyDescriptors (o shader-visible nao pode ser lido como fonte).
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> StagingHeap; // non-shader-visible (CPU)
        u32 HandleSize = 0;
        // Mantida ordenada por Offset e sem ranges adjacentes (coalescidos).
        std::vector<FFreeRange> FreeList;
    };
} 
