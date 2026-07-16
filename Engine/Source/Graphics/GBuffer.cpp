#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {

    // Valores de clear canonicos (precisam casar com o D3D12_CLEAR_VALUE da criacao do recurso).
    //   A: preto, AO=0     B: normal "p/ frente" (0,0,1) + rough/metal 0   C: emissive 0, model 0
    static const FLOAT kClearA[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const FLOAT kClearB[4] = { 0.5f, 0.5f, 0.0f, 0.0f }; // OctEncode((0,0,1)) = (0.5,0.5)
    static const FLOAT kClearC[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    static const FLOAT* ClearFor(u32 Index) {
        return Index == 0 ? kClearA : (Index == 1 ? kClearB : kClearC);
    }

    void FGBuffer::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, u32 _Width, u32 _Height) {
        if (!RTVHeap.Native())
            RTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTargetCount, false);
        if (SRVSlotBase == kInvalidSlot)
            SRVSlotBase = _SRVHeap.Allocate(kTargetCount + 2); // +1 = depth (4o slot contiguo);
                                                               // +2 = shadow map das nuvens (t4
                                                               // do deferred, Renderer copia)
        CreateTargets(_Device, _SRVHeap, _Width, _Height);
    }

    void FGBuffer::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, u32 _Width, u32 _Height) {
        if (_Width == 0 || _Height == 0) return;
        if (_Width == Width && _Height == Height) return;
        CreateTargets(_Device, _SRVHeap, _Width, _Height);
    }

    void FGBuffer::CreateTargets(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, u32 _Width, u32 _Height) {
        if (_Width == 0 || _Height == 0) return;
        Width  = _Width;
        Height = _Height;

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        for (u32 i = 0; i < kTargetCount; ++i) {
            const DXGI_FORMAT Fmt = Format(i);

            Targets[i].Reset();

            D3D12_RESOURCE_DESC ResourceDesc{};
            ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            ResourceDesc.Width            = Width;
            ResourceDesc.Height           = Height;
            ResourceDesc.DepthOrArraySize = 1;
            ResourceDesc.MipLevels        = 1;
            ResourceDesc.Format           = Fmt;
            ResourceDesc.SampleDesc       = { 1, 0 };
            ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE Clear{};
            Clear.Format = Fmt;
            const FLOAT* C = ClearFor(i);
            Clear.Color[0] = C[0]; Clear.Color[1] = C[1];
            Clear.Color[2] = C[2]; Clear.Color[3] = C[3];

            SMILE_HR(_Device->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &Clear, IID_PPV_ARGS(&Targets[i])));
            VramTracker::Register(Targets[i].Get(), EVramCategory::RenderTargets);
            States[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;

            D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
            RTVDesc.Format        = Fmt;
            RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            _Device->CreateRenderTargetView(Targets[i].Get(), &RTVDesc, RTVHeap.CpuHandle(i));

            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                  = Fmt;
            SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Texture2D.MipLevels     = 1;
            _SRVHeap.CreateSRV(_Device, Targets[i].Get(), SRVDesc, SRVSlotBase + i);
        }
    }

    void FGBuffer::WriteDepthSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, ID3D12Resource* _Depth) {
        if (SRVSlotBase == kInvalidSlot || !_Depth) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R32_FLOAT; // depth = R32_TYPELESS -> R32_FLOAT
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, _Depth, SRVDesc, SRVSlotBase + kTargetCount);
    }

    void FGBuffer::AppendTransitions(FBarrierBatch& _Batch, D3D12_RESOURCE_STATES _Target) {
        for (u32 i = 0; i < kTargetCount; ++i)
            _Batch.TransitionTracked(Targets[i].Get(), States[i], _Target);
    }

    void FGBuffer::TransitionToRead(ID3D12GraphicsCommandList* _Cmd, D3D12_RESOURCE_STATES _ReadState) {
        FBarrierBatch Batch;
        AppendTransitions(Batch, _ReadState);
        Batch.Flush(_Cmd);
    }

    void FGBuffer::TransitionToWrite(ID3D12GraphicsCommandList* _Cmd) {
        TransitionToRead(_Cmd, D3D12_RESOURCE_STATE_RENDER_TARGET); // mesma logica, alvo = RENDER_TARGET
    }

    void FGBuffer::Clear(ID3D12GraphicsCommandList* _Cmd) const {
        for (u32 i = 0; i < kTargetCount; ++i)
            _Cmd->ClearRenderTargetView(RTVHeap.CpuHandle(i), ClearFor(i), 0, nullptr);
    }

    void FGBuffer::Shutdown() {
        for (u32 i = 0; i < kTargetCount; ++i) Targets[i].Reset();
        SRVSlotBase = kInvalidSlot;
        Width = Height = 0;
    }
}
