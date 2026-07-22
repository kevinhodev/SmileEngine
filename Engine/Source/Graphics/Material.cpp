#include "Smile/Graphics/Material.h"
#include "Smile/Core/HResultCheck.h"
#include <vector>

namespace Smile {

    // ---- Pool de CBVs de material -------------------------------------------------------------
    // Paginas upload de 64KB (256 slots x 256B) persistentemente mapeadas. 64KB = granularidade
    // real de alocacao do D3D12: um committed buffer de 256B por material custava uma pagina
    // INTEIRA cada (Bistro 132 materiais ~ 8MB + 132 allocs; agora 1 pagina). Free list reusa
    // slots em troca de cena; quando o ultimo material solta (UsedCount=0), as paginas morrem.
    // Sem locks: Finalize/Release rodam na thread principal (como o resto do SRV heap).
    namespace {
        constexpr u32 kSlotsPerPage = 256; // x 256B = 64KB por pagina

        struct FMaterialCBPage {
            Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
            u8* Mapped = nullptr;
        };

        std::vector<FMaterialCBPage> GPages;
        std::vector<u32>             GFreeSlots;
        u32                          GUsedCount = 0;

        void AddPage(ID3D12Device* _Device) {
            D3D12_HEAP_PROPERTIES UploadHeap{};
            UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC BufferDesc{};
            BufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            BufferDesc.Width            = static_cast<u64>(kSlotsPerPage) * sizeof(MaterialConstants);
            BufferDesc.Height           = 1;
            BufferDesc.DepthOrArraySize = 1;
            BufferDesc.MipLevels        = 1;
            BufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
            BufferDesc.SampleDesc       = { 1, 0 };
            BufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            BufferDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

            FMaterialCBPage Page;
            SMILE_HR(_Device->CreateCommittedResource(
                &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Page.Buffer)));
            SMILE_HR(Page.Buffer->Map(0, nullptr, reinterpret_cast<void**>(&Page.Mapped)));

            const u32 PageIndex = static_cast<u32>(GPages.size());
            GPages.push_back(std::move(Page));
            // Empilha em ordem reversa p/ o pop_back entregar os slots da pagina em ordem crescente.
            for (u32 i = kSlotsPerPage; i > 0; --i)
                GFreeSlots.push_back(PageIndex * kSlotsPerPage + (i - 1));
        }

        u32 AllocSlot(ID3D12Device* _Device) {
            if (GFreeSlots.empty()) AddPage(_Device);
            const u32 Slot = GFreeSlots.back();
            GFreeSlots.pop_back();
            ++GUsedCount;
            return Slot;
        }

        void FreeSlot(u32 _Slot) {
            GFreeSlots.push_back(_Slot);
            if (--GUsedCount == 0) { // ultima referencia: solta as paginas (paridade com o
                GPages.clear();      // lifetime antigo, em que cada material tinha o proprio CBV)
                GFreeSlots.clear();
            }
        }

        u8* SlotPtr(u32 _Slot) {
            return GPages[_Slot / kSlotsPerPage].Mapped +
                   static_cast<size_t>(_Slot % kSlotsPerPage) * sizeof(MaterialConstants);
        }

        D3D12_GPU_VIRTUAL_ADDRESS SlotGpuVA(u32 _Slot) {
            return GPages[_Slot / kSlotsPerPage].Buffer->GetGPUVirtualAddress() +
                   static_cast<u64>(_Slot % kSlotsPerPage) * sizeof(MaterialConstants);
        }
    }

    void FMaterial::Finalize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (SRVTableStart != kInvalidTable)
            _SRVHeap.Free(SRVTableStart, kMaterialTextureSlots);
        SRVTableStart = _SRVHeap.Allocate(kMaterialTextureSlots);

        auto WriteSRV = [&](FTexture* _Texture, u32 _LocalSlot) {
            if (!_Texture || !_Texture->IsValid()) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC ResourceDesc{};
            ResourceDesc.Format                        = _Texture->Format();
            ResourceDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
            ResourceDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ResourceDesc.Texture2D.MipLevels           = _Texture->MipCount();
            ResourceDesc.Texture2D.MostDetailedMip     = 0;
            ResourceDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            _SRVHeap.CreateSRV(_Device, _Texture->Resource(), ResourceDesc, SRVTableStart + _LocalSlot);
        };

        WriteSRV(Albedo,            0);
        WriteSRV(Normal,            1);
        WriteSRV(MetallicRoughness, 2);
        WriteSRV(AO,                3);
        WriteSRV(Emissive,          4);
        WriteSRV(Height,            5);
        WriteSRV(Metalness,         6);
        WriteSRV(Roughness,         7);

        Constants.HasAlbedoMap            = (Albedo            && Albedo->IsValid())            ? 1 : 0;
        Constants.HasNormalMap            = (Normal            && Normal->IsValid())            ? 1 : 0;
        Constants.HasMetallicRoughnessMap = (MetallicRoughness && MetallicRoughness->IsValid()) ? 1 : 0;
        Constants.HasAOMap                = (AO                && AO->IsValid())                ? 1 : 0;
        Constants.HasEmissiveMap          = (Emissive          && Emissive->IsValid())          ? 1 : 0;
        Constants.HasHeightMap            = (Height            && Height->IsValid())            ? 1 : 0;
        Constants.HasMetalnessMap         = (Metalness         && Metalness->IsValid())         ? 1 : 0;
        Constants.HasRoughnessMap         = (Roughness         && Roughness->IsValid())         ? 1 : 0;

        // Re-finalize reusa o slot do pool (so os SRVs sao remontados acima).
        if (CBSlot == kInvalidTable) {
            CBSlot  = AllocSlot(_Device);
            CBGpuVA = SlotGpuVA(CBSlot);
        }
        MappedCBV = reinterpret_cast<MaterialConstants*>(SlotPtr(CBSlot));
        memcpy(MappedCBV, &Constants, sizeof(MaterialConstants));
    }

    void FMaterial::Release(FTextureSRVHeap& _SRVHeap) {
        if (SRVTableStart != kInvalidTable) {
            _SRVHeap.Free(SRVTableStart, kMaterialTextureSlots);
            SRVTableStart = kInvalidTable;
        }
        if (CBSlot != kInvalidTable) {
            FreeSlot(CBSlot);
            CBSlot    = kInvalidTable;
            CBGpuVA   = 0;
            MappedCBV = nullptr;
        }
    }

    void FMaterial::UpdateConstants() {
        if (MappedCBV)
            memcpy(MappedCBV, &Constants, sizeof(MaterialConstants));
    }

    void FMaterial::UpdateTextureSlot(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                       u32 _LocalSlot, FTexture* _Texture) {
        if (!_Texture || !_Texture->IsValid() || _LocalSlot >= kMaterialTextureSlots) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
        Desc.Format                        = _Texture->Format();
        Desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        Desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Desc.Texture2D.MipLevels           = _Texture->MipCount();
        Desc.Texture2D.MostDetailedMip     = 0;
        Desc.Texture2D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, _Texture->Resource(), Desc, SRVTableStart + _LocalSlot);
    }

    void FMaterial::Bind(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap) const {
        _CommandList->SetGraphicsRootConstantBufferView(1, CBGpuVA);
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(SRVTableStart));
    }
}
