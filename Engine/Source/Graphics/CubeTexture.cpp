#include "Smile/Graphics/CubeTexture.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    void FCubeTexture::Create(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                              DXGI_FORMAT _Format, u32 _Size, u32 _MipLevels, bool _AllowUAV) {
        TexFormat = _Format;
        TexSize   = _Size;
        MipLevels = _MipLevels;

        D3D12_RESOURCE_DESC TextureDesc{};
        TextureDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TextureDesc.Width            = _Size;
        TextureDesc.Height           = _Size;
        TextureDesc.DepthOrArraySize = 6;
        TextureDesc.MipLevels        = static_cast<UINT16>(_MipLevels);
        TextureDesc.Format           = _Format;
        TextureDesc.SampleDesc       = { 1, 0 };
        TextureDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        TextureDesc.Flags            = _AllowUAV
                                     ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                     : D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES DefaultHeap{};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        const D3D12_RESOURCE_STATES InitialState = _AllowUAV
            ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            : D3D12_RESOURCE_STATE_COPY_DEST;
        MipStates.assign(static_cast<size_t>(_MipLevels) * 6, InitialState);

        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
            InitialState, nullptr, IID_PPV_ARGS(&GpuResource)));

        // SRV TextureCube
        FaceSRVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                          = _Format;
        SRVDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
        SRVDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.TextureCube.MipLevels           = _MipLevels;
        SRVDesc.TextureCube.MostDetailedMip     = 0;
        SRVDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, GpuResource.Get(), SRVDesc, FaceSRVSlot);

        // UAVs (Texture2DArray over the 6 faces, one per mip)
        if (_AllowUAV) {
            UAVSlots.resize(_MipLevels);
            for (u32 Mip = 0; Mip < _MipLevels; ++Mip) {
                u32 Slot = _SRVHeap.Allocate(1);
                UAVSlots[Mip] = Slot;

                D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
                UAVDesc.Format                         = _Format;
                UAVDesc.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                UAVDesc.Texture2DArray.MipSlice        = Mip;
                UAVDesc.Texture2DArray.FirstArraySlice = 0;
                UAVDesc.Texture2DArray.ArraySize       = 6;
                UAVDesc.Texture2DArray.PlaneSlice      = 0;
                _SRVHeap.CreateUAV(_Device, GpuResource.Get(), UAVDesc, Slot);
            }
        }
    }

    void FCubeTexture::Transition(ID3D12GraphicsCommandList* _CommandList,
                                  D3D12_RESOURCE_STATES _After) {
        // Fast path: if every subresource already matches, no-op. Otherwise
        // we emit per-subresource barriers (only for subresources that differ),
        // then update all states.
        bool AllMatch = true;
        for (auto s : MipStates) if (s != _After) { AllMatch = false; break; }
        if (AllMatch) return;

        // Per-subresource transition. Subresource index uses the D3D12 layout:
        // subIdx = ArraySlice * MipLevels + MipSlice, which is exactly our
        // MipStates index — emit a barrier wherever the state differs.
        std::vector<D3D12_RESOURCE_BARRIER> Barriers;
        Barriers.reserve(MipStates.size());
        for (u32 SubIdx = 0; SubIdx < MipStates.size(); ++SubIdx) {
            if (MipStates[SubIdx] == _After) continue;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = GpuResource.Get();
            B.Transition.StateBefore = MipStates[SubIdx];
            B.Transition.StateAfter  = _After;
            B.Transition.Subresource = SubIdx;
            Barriers.push_back(B);
            MipStates[SubIdx] = _After;
        }
        if (!Barriers.empty())
            _CommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
    }

    void FCubeTexture::TransitionMip(ID3D12GraphicsCommandList* _CommandList,
                                     u32 _MipSlice, D3D12_RESOURCE_STATES _After) {
        // Subresource index for cube (single plane): face * MipLevels + mip.
        D3D12_RESOURCE_BARRIER Barriers[6];
        UINT Count = 0;
        for (u32 Face = 0; Face < 6; ++Face) {
            const u32 SubIdx = Face * MipLevels + _MipSlice;
            if (MipStates[SubIdx] == _After) continue;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = GpuResource.Get();
            B.Transition.StateBefore = MipStates[SubIdx];
            B.Transition.StateAfter  = _After;
            B.Transition.Subresource = SubIdx;
            Barriers[Count++] = B;
            MipStates[SubIdx] = _After;
        }
        if (Count > 0) _CommandList->ResourceBarrier(Count, Barriers);
    }
}
