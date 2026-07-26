#include "Smile/Graphics/VolumeTexture.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    void FVolumeTexture::Create(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                DXGI_FORMAT _Format, u32 _Width, u32 _Height, u32 _Depth,
                                u32 _MipLevels, bool _AllowUAV) {
        TexFormat = _Format;
        TexWidth  = _Width;
        TexHeight = _Height;
        TexDepth  = _Depth;
        MipLevels = _MipLevels;

        D3D12_RESOURCE_DESC TextureDesc{};
        TextureDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        TextureDesc.Width            = _Width;
        TextureDesc.Height           = _Height;
        TextureDesc.DepthOrArraySize = static_cast<UINT16>(_Depth);
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
        MipStates.assign(_MipLevels, InitialState);

        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
            InitialState, nullptr, IID_PPV_ARGS(&GpuResource)));
        VramTracker::Register(GpuResource.Get(), EVramCategory::Sky);

        VolumeSRVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                        = _Format;
        SRVDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE3D;
        SRVDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture3D.MipLevels           = _MipLevels;
        SRVDesc.Texture3D.MostDetailedMip     = 0;
        SRVDesc.Texture3D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, GpuResource.Get(), SRVDesc, VolumeSRVSlot);

        if (_AllowUAV) {
            UAVSlots.resize(_MipLevels);
            for (u32 Mip = 0; Mip < _MipLevels; ++Mip) {
                u32 Slot = _SRVHeap.Allocate(1);
                UAVSlots[Mip] = Slot;

                const u32 MipDepth = (_Depth >> Mip) ? (_Depth >> Mip) : 1u;

                D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
                UAVDesc.Format                = _Format;
                UAVDesc.ViewDimension         = D3D12_UAV_DIMENSION_TEXTURE3D;
                UAVDesc.Texture3D.MipSlice    = Mip;
                UAVDesc.Texture3D.FirstWSlice = 0;
                UAVDesc.Texture3D.WSize       = MipDepth;
                _SRVHeap.CreateUAV(_Device, GpuResource.Get(), UAVDesc, Slot);
            }
        }
    }

    void FVolumeTexture::Transition(ID3D12GraphicsCommandList* _CommandList,
                                    D3D12_RESOURCE_STATES _After) {
        bool AllMatch = true;
        for (auto s : MipStates) if (s != _After) { AllMatch = false; break; }
        if (AllMatch) return;

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

    void FVolumeTexture::TransitionMip(ID3D12GraphicsCommandList* _CommandList,
                                       u32 _MipSlice, D3D12_RESOURCE_STATES _After) {
        if (_MipSlice >= MipStates.size()) return;
        if (MipStates[_MipSlice] == _After) return;

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = GpuResource.Get();
        B.Transition.StateBefore = MipStates[_MipSlice];
        B.Transition.StateAfter  = _After;
        B.Transition.Subresource = _MipSlice;
        _CommandList->ResourceBarrier(1, &B);
        MipStates[_MipSlice] = _After;
    }
}
