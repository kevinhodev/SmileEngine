#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <string>
#include <stdexcept>

namespace Smile {
    void FTextureSRVHeap::Initialize(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc{};
        DescriptorHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        DescriptorHeapDesc.NumDescriptors = kCapacity;
        DescriptorHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        DescriptorHeapDesc.NodeMask       = 0;
        SMILE_HR(_Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(&Heap)));

        DescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        SMILE_HR(_Device->CreateDescriptorHeap(&DescriptorHeapDesc, IID_PPV_ARGS(&StagingHeap)));

        HandleSize = _Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        FreeList.clear();
        FreeList.push_back({ 0u, kCapacity });
    }

    u32 FTextureSRVHeap::Allocate(u32 _Count) {
        const u32 Slot = TryAllocate(_Count);
        if (Slot != kInvalidSlot) return Slot;
        throw std::runtime_error("TextureSRVHeap capacity exceeded");
    }

    u32 FTextureSRVHeap::TryAllocate(u32 _Count) {
        if (_Count == 0) _Count = 1;

        for (size_t i = 0; i < FreeList.size(); ++i) {
            if (FreeList[i].Count >= _Count) {
                const u32 Slot = FreeList[i].Offset;
                FreeList[i].Offset += _Count;
                FreeList[i].Count  -= _Count;
                if (FreeList[i].Count == 0)
                    FreeList.erase(FreeList.begin() + i);
                UsedSlots_ += _Count;
                if (UsedSlots_ > HighWaterSlots_) HighWaterSlots_ = UsedSlots_;
                return Slot;
            }
        }
        // Conta e DIZ. Quem chamou pelo Allocate lanca em seguida, e a excecao so carrega a
        // frase; sem isto, "acabou o heap" nunca dizia quanto faltava nem quanto estava preso
        // esperando fence — que e a diferenca entre subir o teto e caçar um consumidor que nao
        // devolve descritor.
        ++AllocationFailures_;
        LogError("TextureSRVHeap sem espaco: pedido de " + std::to_string(_Count) +
                 " descritor(es); em uso " + std::to_string(UsedSlots_) + ", aposentados " +
                 std::to_string(RetiredSlots_) + ", capacidade " + std::to_string(kCapacity));
        return kInvalidSlot;
    }

    // So o free list. Separado do Free porque o CollectRetired tambem devolve range, e la o slot
    // ja saiu de UsedSlots_ quando foi aposentado — descontar de novo faria a ocupacao cair
    // abaixo do real e acabaria em zero com o heap cheio.
    void FTextureSRVHeap::InsertFreeRange(u32 _Slot, u32 _Count) {
        size_t i = 0;
        while (i < FreeList.size() && FreeList[i].Offset < _Slot) ++i;
        FreeList.insert(FreeList.begin() + i, { _Slot, _Count });

        if (i + 1 < FreeList.size() &&
            FreeList[i].Offset + FreeList[i].Count == FreeList[i + 1].Offset) {
            FreeList[i].Count += FreeList[i + 1].Count;
            FreeList.erase(FreeList.begin() + i + 1);
        }
        if (i > 0 &&
            FreeList[i - 1].Offset + FreeList[i - 1].Count == FreeList[i].Offset) {
            FreeList[i - 1].Count += FreeList[i].Count;
            FreeList.erase(FreeList.begin() + i);
        }
    }

    void FTextureSRVHeap::Free(u32 _Slot, u32 _Count) {
        if (_Count == 0) return;
        UsedSlots_ -= (_Count <= UsedSlots_) ? _Count : UsedSlots_;
        InsertFreeRange(_Slot, _Count);
    }

    void FTextureSRVHeap::FreeDeferred(u32 _Slot, u32 _Count, const FGpuFenceSet& _Required) {
        if (_Count == 0 || _Slot == kInvalidSlot) return;
        // Sai de UsedSlots_ AGORA e entra em RetiredSlots_: as duas grandezas nao se sobrepoem, e
        // "quanto do heap esta indisponivel" e a soma das duas. Contar o aposentado como usado
        // esconderia a fila de espera exatamente quando ela e o problema.
        UsedSlots_ -= (_Count <= UsedSlots_) ? _Count : UsedSlots_;
        RetiredSlots_ += _Count;
        RetiredList.push_back({ _Slot, _Count, _Required });
    }

    u32 FTextureSRVHeap::CollectRetired(const FGpuFenceSet& _Completed) {
        u32 Collected = 0;
        for (size_t i = 0; i < RetiredList.size();) {
            if (!_Completed.Covers(RetiredList[i].Required)) { ++i; continue; }
            const FRetiredRange Range = RetiredList[i];
            // swap-and-pop: a ordem da lista de aposentados nao significa nada — cada entrada
            // carrega a propria fence exigida —, e assim o custo continua O(aposentados) mesmo
            // quando os que vencem estao no meio.
            RetiredList[i] = RetiredList.back();
            RetiredList.pop_back();
            RetiredSlots_ -= (Range.Count <= RetiredSlots_) ? Range.Count : RetiredSlots_;
            InsertFreeRange(Range.Offset, Range.Count);
            ++Collected;
        }
        return Collected;
    }

    void FTextureSRVHeap::Release(u32& _Slot, u32 _Count) {
        if (_Slot == kInvalidSlot || _Count == 0) return;
        Free(_Slot, _Count);
        _Slot = kInvalidSlot;
    }

    void FTextureSRVHeap::CopyTable(ID3D12Device* _Device, u32 _DestinationSlot,
                                    std::span<const u32> _SourceSlots) const {
        if (_SourceSlots.empty()) return;

        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> SourceHandles;
        SourceHandles.reserve(_SourceSlots.size());
        for (const u32 Slot : _SourceSlots)
            SourceHandles.push_back(CpuHandleStaging(Slot));

        std::vector<UINT> SourceCounts(_SourceSlots.size(), 1u);
        D3D12_CPU_DESCRIPTOR_HANDLE Destination = CpuHandle(_DestinationSlot);
        UINT DestinationCount = static_cast<UINT>(_SourceSlots.size());
        _Device->CopyDescriptors(
            1, &Destination, &DestinationCount,
            static_cast<UINT>(SourceHandles.size()), SourceHandles.data(), SourceCounts.data(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FTextureSRVHeap::CreateSRV(ID3D12Device* _Device, ID3D12Resource* _Resource,
                                     const D3D12_SHADER_RESOURCE_VIEW_DESC& _SRVDesc, u32 _Slot) {
        _Device->CreateShaderResourceView(_Resource, &_SRVDesc, CpuHandle(_Slot));
        _Device->CreateShaderResourceView(_Resource, &_SRVDesc, CpuHandleStaging(_Slot));
    }

    void FTextureSRVHeap::CreateUAV(ID3D12Device* _Device, ID3D12Resource* _Resource,
                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC& _UAVDesc, u32 _Slot) {
        _Device->CreateUnorderedAccessView(_Resource, nullptr, &_UAVDesc, CpuHandle(_Slot));
        _Device->CreateUnorderedAccessView(_Resource, nullptr, &_UAVDesc, CpuHandleStaging(_Slot));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE FTextureSRVHeap::CpuHandle(u32 _Slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle =
            Heap->GetCPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<SIZE_T>(_Slot) * HandleSize;
        return Handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE FTextureSRVHeap::GpuHandle(u32 _Slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE Handle =
            Heap->GetGPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<UINT64>(_Slot) * HandleSize;
        return Handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE FTextureSRVHeap::CpuHandleStaging(u32 _Slot) const {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle =
            StagingHeap->GetCPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<SIZE_T>(_Slot) * HandleSize;
        return Handle;
    }
} 
