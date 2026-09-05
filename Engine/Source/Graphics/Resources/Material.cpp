#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
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
            const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
                _Device, sizeof(MaterialConstants), kSlotsPerPage);

            FMaterialCBPage Page;
            Page.Buffer = Upload.Resource;
            Page.Mapped = Upload.Mapped;

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
        // Re-finalize REUSA o range e reescreve os descritores no lugar. Isso e seguro AQUI e so
        // aqui: os tres chamadores do Finalize (default, import de cena e proxy do terreno) rodam
        // com as duas filas ja drenadas, entao nao ha command list em voo referenciando o range.
        //
        // O caminho que NAO tem essa garantia — troca de textura no editor — nao passa por aqui:
        // ele usa o RepublishTextureTable, que aloca outro range e devolve este para o chamador
        // aposentar atras das fences.
        //
        // A base tambem nao e privada do raster: ela e o `TextureBase` da linha canonica do
        // material, de onde os shaders de RT tiram os indices bindless. Reusar o range mantem a
        // linha inalterada; mudar a base suja a linha, que e como o RepublishTextureTable
        // propaga.
        if (SRVTableStart == kInvalidTable)
            SRVTableStart = _SRVHeap.Allocate(kMaterialTextureSlots);

        WriteTextureTable(_Device, _SRVHeap, SRVTableStart);

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

    // As oito posicoes, SEMPRE. Slot sem textura recebe um descritor NULO (recurso nulo com desc
    // valida), que e leitura definida devolvendo zero — e nao um slot deixado como estava.
    //
    // Antes o slot vazio simplesmente nao era escrito. Passava porque o range nunca se movia e o
    // shader consultava o bit `HasXMap` antes de amostrar; nao passa mais: o
    // RepublishTextureTable aloca range NOVO, e ali "como estava" e lixo de quem usou aqueles
    // slots por ultimo. O preview de material, alem disso, copia os oito descritores do staging
    // sem consultar flag nenhuma (MaterialPreview.cpp).
    void FMaterial::WriteTextureTable(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                      u32 _Base) const {
        FTexture* const Slots[kMaterialTextureSlots] = {
            Albedo, Normal, MetallicRoughness, AO, Emissive, Height, Metalness, Roughness
        };

        for (u32 i = 0; i < kMaterialTextureSlots; ++i) {
            FTexture* Texture = Slots[i];
            const bool Valid  = Texture && Texture->IsValid();

            D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
            // Formato do descritor nulo: uma forma qualquer VALIDA e obrigatoria (typeless nao
            // serve). RGBA8 unorm porque nao ha recurso por tras — o formato so descreve como a
            // leitura seria decodificada, e ela devolve zero.
            Desc.Format                        = Valid ? Texture->Format()
                                                       : DXGI_FORMAT_R8G8B8A8_UNORM;
            Desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
            Desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Desc.Texture2D.MipLevels           = Valid ? Texture->MipCount() : 1;
            Desc.Texture2D.MostDetailedMip     = 0;
            Desc.Texture2D.ResourceMinLODClamp = 0.0f;
            _SRVHeap.CreateSRV(_Device, Valid ? Texture->Resource() : nullptr, Desc, _Base + i);
        }
    }

    bool FMaterial::RepublishTextureTable(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                          u32& _OutRetiredBase) {
        _OutRetiredBase = kInvalidDescriptorTable;
        if (SRVTableStart == kInvalidTable) return false;

        // Aloca ANTES de publicar qualquer coisa, e RECUSANDO em vez de lancar: sem range novo o
        // material continua valido com o antigo. Uma excecao daqui subiria por um callback de UI
        // com o material ja mutado pelo chamador — material CPU e tabela GPU divergentes, ou o
        // processo terminado.
        const u32 NewBase = _SRVHeap.TryAllocate(kMaterialTextureSlots);
        if (NewBase == FTextureSRVHeap::kInvalidSlot) return false;

        WriteTextureTable(_Device, _SRVHeap, NewBase);

        // Publicacao: so aqui o estado muda, e as duas linhas nao podem falhar.
        _OutRetiredBase = SRVTableStart;
        SRVTableStart   = NewBase;
        return true;
    }

    void FMaterial::Bind(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap) const {
        _CommandList->SetGraphicsRootConstantBufferView(1, CBGpuVA);
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(SRVTableStart));
    }
}
