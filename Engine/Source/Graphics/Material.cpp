#include "Smile/Graphics/Material.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    void FMaterial::Finalize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        SRVTableStart = _SRVHeap.Allocate(kMaterialTextureSlots);

        auto WriteSRV = [&](FTexture* _Texture, u32 _LocalSlot) {
            if (!_Texture || !_Texture->IsValid()) return;
            D3D12_SHADER_RESOURCE_VIEW_DESC ResourceDesc{};
            ResourceDesc.Format                        = DXGI_FORMAT_R8G8B8A8_UNORM;
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

        Constants.HasAlbedoMap            = (Albedo            && Albedo->IsValid())            ? 1 : 0;
        Constants.HasNormalMap            = (Normal            && Normal->IsValid())            ? 1 : 0;
        Constants.HasMetallicRoughnessMap = (MetallicRoughness && MetallicRoughness->IsValid()) ? 1 : 0;
        Constants.HasAOMap                = (AO                && AO->IsValid())                ? 1 : 0;
        Constants.HasEmissiveMap          = (Emissive          && Emissive->IsValid())          ? 1 : 0;
        Constants.HasHeightMap            = (Height            && Height->IsValid())            ? 1 : 0;

        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC BufferDesc{};
        BufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufferDesc.Width            = sizeof(MaterialConstants);
        BufferDesc.Height           = 1;
        BufferDesc.DepthOrArraySize = 1;
        BufferDesc.MipLevels        = 1;
        BufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        BufferDesc.SampleDesc       = { 1, 0 };
        BufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BufferDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(_Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&CBV)));

        SMILE_HR(CBV->Map(0, nullptr, reinterpret_cast<void**>(&MappedCBV)));
        memcpy(MappedCBV, &Constants, sizeof(MaterialConstants));
    }

    void FMaterial::UpdateConstants() {
        if (MappedCBV)
            memcpy(MappedCBV, &Constants, sizeof(MaterialConstants));
    }

    void FMaterial::UpdateTextureSlot(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                       u32 _LocalSlot, FTexture* _Texture) {
        if (!_Texture || !_Texture->IsValid() || _LocalSlot >= kMaterialTextureSlots) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
        Desc.Format                        = DXGI_FORMAT_R8G8B8A8_UNORM;
        Desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        Desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Desc.Texture2D.MipLevels           = _Texture->MipCount();
        Desc.Texture2D.MostDetailedMip     = 0;
        Desc.Texture2D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, _Texture->Resource(), Desc, SRVTableStart + _LocalSlot);
    }

    void FMaterial::Bind(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap) const {
        _CommandList->SetGraphicsRootConstantBufferView(1, CBV->GetGPUVirtualAddress());
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(SRVTableStart));
    }
} 
