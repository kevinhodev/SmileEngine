#include "Smile/Graphics/DILite.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        // FP16: o alvo guarda radiancia direta ja com BRDF aplicada, na mesma faixa do HDR.
        constexpr DXGI_FORMAT kDirectFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr u32 kTableSize = 7; // t0..t2 gbuffer, t3 depth, t4 TLAS, t5 InstanceGeo, t6 luzes
    }

    void FDILite::Initialize(ID3D12Device* _Device) {
        // heap-directly-indexed: o alpha-test do RTAlphaTest.hlsli busca VB/IB/albedo por
        // ResourceDescriptorHeap a partir do InstanceGeo (bindless), como no ReSTIR e no DDGI.
        PSO.Initialize(_Device, "DILite.cs_6_6.cso", kTableSize, 1, true);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FDILite::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(DILiteConstants);
        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = Size;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));
    }

    D3D12_GPU_VIRTUAL_ADDRESS FDILite::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<u64>(FrameSlot) * sizeof(DILiteConstants);
    }

    void FDILite::ReleaseResize(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(OutSRV, 1);
        Free(OutUAV, 1);
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) Free(Table[i], kTableSize);
        Output = nullptr;
        OutputState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FDILite::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                 u32 _Width, u32 _Height,
                                 u32 _GBufferASlot, u32 _GBufferBSlot, u32 _GBufferCSlot,
                                 u32 _DepthSlot, u32 _TlasSlot, u32 _InstanceSlot,
                                 const u32 _LightSlots[FCommandQueue::kFramesInFlight]) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        if (_Width == 0 || _Height == 0) return;
        if (_TlasSlot == kInvalidSlot || _InstanceSlot == kInvalidSlot) return; // sem DXR: no-op

        Width = _Width; Height = _Height;

        {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = Width;
            Desc.Height           = Height;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = kDirectFormat;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Output)));
            VramTracker::Register(Output.Get(), EVramCategory::RenderTargets);
            OutputState = D3D12_RESOURCE_STATE_COMMON;
        }

        OutSRV = _SRVHeap.Allocate(1);
        OutUAV = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                  = kDirectFormat;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, Output.Get(), Srv, OutSRV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format        = kDirectFormat;
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _SRVHeap.CreateUAV(_Device, Output.Get(), Uav, OutUAV);

        // Uma tabela por frame em voo. Os 6 primeiros descritores sao iguais em todas; so o das
        // luzes (t6) aponta p/ a fatia daquele frame. Montadas UMA vez aqui, entao nada e reescrito
        // com frames em voo — o hazard de descriptor versioning nao existe neste passe.
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) {
            Table[i] = _SRVHeap.Allocate(kTableSize);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(Table[i]);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[kTableSize] = {
                _SRVHeap.CpuHandleStaging(_GBufferASlot),
                _SRVHeap.CpuHandleStaging(_GBufferBSlot),
                _SRVHeap.CpuHandleStaging(_GBufferCSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_LightSlots[i]),
            };
            UINT DstCount = kTableSize;
            UINT SrcCounts[kTableSize] = { 1, 1, 1, 1, 1, 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, kTableSize, Src, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        Ready = true;
    }

    void FDILite::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj, const Vec3& _CameraPos,
                                 u32 _Width, u32 _Height, u32 _FrameIndex, u32 _LightCount,
                                 u32 _ShadowRayMask) {
        FrameSlot = _FrameSlot;
        if (!MappedCB) return;

        CPU.InvViewProj  = _InvViewProj;
        CPU.CameraPos    = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 0.0f };
        CPU.ScreenParams = { static_cast<f32>(_Width), static_cast<f32>(_Height),
                             1.0f / static_cast<f32>(_Width), 1.0f / static_cast<f32>(_Height) };
        CPU.Params       = { static_cast<f32>(_LightCount), static_cast<f32>(_FrameIndex),
                             static_cast<f32>(_ShadowRayMask), RayEps.VisRayEndMargin };
        CPU.RayEpsA      = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                             RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB      = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                             FRayEpsilonProfile::kOriginAngularMinRatio };

        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(DILiteConstants),
                    &CPU, sizeof(DILiteConstants));
    }

    void FDILite::RecordDispatch(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;

        if (OutputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = Output.Get();
            B.Transition.StateBefore = OutputState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            _CL->ResourceBarrier(1, &B);
            OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        PSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(Table[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(OutUAV));
        _CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
    }

    void FDILite::TransitionForRead(ID3D12GraphicsCommandList* _CL) {
        if (!Ready) return;
        const D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (OutputState == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Output.Get();
        B.Transition.StateBefore = OutputState;
        B.Transition.StateAfter  = After;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _CL->ResourceBarrier(1, &B);
        OutputState = After;
    }
}
