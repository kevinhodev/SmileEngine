#include "Smile/Graphics/Material.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {

    void FMaterial::Finalize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap) {
        // --- Allocate kMaterialTextureSlots contiguous SRV slots ---
        SRVTableStart = SRVHeap.Allocate(kMaterialTextureSlots);

        // Helper: write the SRV for a slot, using the given texture or skip if null
        // (default textures should always be assigned before Finalize)
        auto WriteSRV = [&](FTexture* Tex, u32 LocalSlot) {
            if (!Tex || !Tex->IsValid()) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
            Desc.Format                        = DXGI_FORMAT_R8G8B8A8_UNORM;
            Desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
            Desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Desc.Texture2D.MipLevels           = 1;
            Desc.Texture2D.MostDetailedMip     = 0;
            Desc.Texture2D.ResourceMinLODClamp = 0.0f;
            SRVHeap.CreateSRV(Device, Tex->Resource(), Desc, SRVTableStart + LocalSlot);
        };

        WriteSRV(Albedo,            0);
        WriteSRV(Normal,            1);
        WriteSRV(MetallicRoughness, 2);
        WriteSRV(AO,                3);
        WriteSRV(Height,            4);
        WriteSRV(Emissive,          5);

        // --- Sync flags into constants ---
        Constants.HasAlbedoMap            = (Albedo            && Albedo->IsValid())            ? 1 : 0;
        Constants.HasNormalMap            = (Normal            && Normal->IsValid())            ? 1 : 0;
        Constants.HasMetallicRoughnessMap = (MetallicRoughness && MetallicRoughness->IsValid()) ? 1 : 0;
        Constants.HasAOMap                = (AO                && AO->IsValid())                ? 1 : 0;
        Constants.HasHeightMap            = (Height            && Height->IsValid())            ? 1 : 0;
        Constants.HasEmissiveMap          = (Emissive          && Emissive->IsValid())          ? 1 : 0;

        // --- Create constant buffer (256-byte aligned, persistently mapped) ---
        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC BufDesc{};
        BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufDesc.Width            = sizeof(MaterialConstants);
        BufDesc.Height           = 1;
        BufDesc.DepthOrArraySize = 1;
        BufDesc.MipLevels        = 1;
        BufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        BufDesc.SampleDesc       = { 1, 0 };
        BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BufDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&CBV)));

        SMILE_HR(CBV->Map(0, nullptr, reinterpret_cast<void**>(&MappedCBV)));
        memcpy(MappedCBV, &Constants, sizeof(MaterialConstants));
    }

    void FMaterial::UpdateConstants() {
        if (MappedCBV)
            memcpy(MappedCBV, &Constants, sizeof(MaterialConstants));
    }

    void FMaterial::UpdateTextureSlot(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                       u32 LocalSlot, FTexture* Tex) {
        if (!Tex || !Tex->IsValid() || LocalSlot >= kMaterialTextureSlots) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
        Desc.Format                        = DXGI_FORMAT_R8G8B8A8_UNORM;
        Desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        Desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Desc.Texture2D.MipLevels           = 1;
        Desc.Texture2D.MostDetailedMip     = 0;
        Desc.Texture2D.ResourceMinLODClamp = 0.0f;
        SRVHeap.CreateSRV(Device, Tex->Resource(), Desc, SRVTableStart + LocalSlot);
    }

    void FMaterial::Bind(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap) const {
        // Root parameter 1: CBV b1 — material constants
        Cmd->SetGraphicsRootConstantBufferView(1, CBV->GetGPUVirtualAddress());

        // Root parameter 2: Descriptor table t0–t5 — textures
        Cmd->SetGraphicsRootDescriptorTable(2, SRVHeap.GpuHandle(SRVTableStart));
    }

} // namespace Smile
