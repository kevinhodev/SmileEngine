#include "Smile/Graphics/AmbientOcclusion.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kAOFormat = DXGI_FORMAT_R8_UNORM;

        ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* _Device, u32 _Width, u32 _Height,
                                           DXGI_FORMAT _Format) {
            return GpuResources::CreateTex2D(_Device, _Width, _Height, _Format,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_COMMON,
                                             EVramCategory::RenderTargets);
        }
    }

    void FAmbientOcclusion::Initialize(ID3D12Device* _Device) {
        MainPSO.Initialize(_Device, "GTAO.cs_6_0.cso", 2, 1);
        BlurPSO.Initialize(_Device, "GTAOBlur.cs_6_0.cso", 2, 1);
        UpsamplePSO.Initialize(_Device, "GTAOUpsample.cs_6_0.cso", 2, 1);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FAmbientOcclusion::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * 2 *
                            sizeof(GTAOConstants);
        // Bloco unico: o Execute fatia por (FrameSlot*2 + passe) na mao, entao o alinhamento
        // de CB nao se aplica aqui.
        GpuResources::FUploadBuffer Upload =
            GpuResources::CreateUploadBuffer(_Device, Size, 1, /*ForConstantBuffer*/ false);
        CB       = Upload.Resource;
        MappedCB = Upload.Mapped;
    }

    void FAmbientOcclusion::ReleaseSizedResources(FTextureSRVHeap& _SRVHeap) {
        auto FreeSlot = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        FreeSlot(AO0SRVSlot, 1);
        FreeSlot(AO0UAVSlot, 1);
        FreeSlot(AO1SRVSlot, 1);
        FreeSlot(AO1UAVSlot, 1);
        FreeSlot(AOH0SRVSlot, 1);
        FreeSlot(AOH0UAVSlot, 1);
        FreeSlot(AOH1SRVSlot, 1);
        FreeSlot(AOH1UAVSlot, 1);
        FreeSlot(MainTable, 2);
        FreeSlot(BlurTable0, 2);
        FreeSlot(BlurTable1, 2);
        FreeSlot(BlurTableH0, 2);
        FreeSlot(BlurTableH1, 2);
        AO0.Reset();
        AO1.Reset();
        AOH0.Reset();
        AOH1.Reset();
        AO0State  = D3D12_RESOURCE_STATE_COMMON;
        AO1State  = D3D12_RESOURCE_STATE_COMMON;
        AOH0State = D3D12_RESOURCE_STATE_COMMON;
        AOH1State = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FAmbientOcclusion::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                           u32 _DepthSRVSlot, u32 _NormalSRVSlot,
                                           u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) return;
        ReleaseSizedResources(_SRVHeap);
        Width  = _Width;
        Height = _Height;

        const u32 HalfW = (Width  + 1) / 2;
        const u32 HalfH = (Height + 1) / 2;
        AO0  = CreateTex2D(_Device, Width, Height, kAOFormat);
        AO1  = CreateTex2D(_Device, Width, Height, kAOFormat);
        AOH0 = CreateTex2D(_Device, HalfW, HalfH, kAOFormat);
        AOH1 = CreateTex2D(_Device, HalfW, HalfH, kAOFormat);

        AO0SRVSlot  = _SRVHeap.Allocate(1);
        AO0UAVSlot  = _SRVHeap.Allocate(1);
        AO1SRVSlot  = _SRVHeap.Allocate(1);
        AO1UAVSlot  = _SRVHeap.Allocate(1);
        AOH0SRVSlot = _SRVHeap.Allocate(1);
        AOH0UAVSlot = _SRVHeap.Allocate(1);
        AOH1SRVSlot = _SRVHeap.Allocate(1);
        AOH1UAVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kAOFormat;
        Srv.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, AO0.Get(),  Srv, AO0SRVSlot);
        _SRVHeap.CreateSRV(_Device, AO1.Get(),  Srv, AO1SRVSlot);
        _SRVHeap.CreateSRV(_Device, AOH0.Get(), Srv, AOH0SRVSlot);
        _SRVHeap.CreateSRV(_Device, AOH1.Get(), Srv, AOH1SRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kAOFormat;
        _SRVHeap.CreateUAV(_Device, AO0.Get(),  Uav, AO0UAVSlot);
        _SRVHeap.CreateUAV(_Device, AO1.Get(),  Uav, AO1UAVSlot);
        _SRVHeap.CreateUAV(_Device, AOH0.Get(), Uav, AOH0UAVSlot);
        _SRVHeap.CreateUAV(_Device, AOH1.Get(), Uav, AOH1UAVSlot);

        MainTable = _SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(MainTable);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[2] = {
                _SRVHeap.CpuHandleStaging(_DepthSRVSlot),
                _SRVHeap.CpuHandleStaging(_NormalSRVSlot),
            };
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Src, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        BlurTable0 = _SRVHeap.Allocate(2);
        BlurTable1 = _SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst0 = _SRVHeap.CpuHandle(BlurTable0);
            D3D12_CPU_DESCRIPTOR_HANDLE Src0[2] = {
                _SRVHeap.CpuHandleStaging(_DepthSRVSlot),
                _SRVHeap.CpuHandleStaging(AO0SRVSlot),
            };
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &Dst0, &DstCount, 2, Src0, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            D3D12_CPU_DESCRIPTOR_HANDLE Dst1 = _SRVHeap.CpuHandle(BlurTable1);
            D3D12_CPU_DESCRIPTOR_HANDLE Src1[2] = {
                _SRVHeap.CpuHandleStaging(_DepthSRVSlot),
                _SRVHeap.CpuHandleStaging(AO1SRVSlot),
            };
            _Device->CopyDescriptors(1, &Dst1, &DstCount, 2, Src1, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        // Cadeia meia-res: depth + AOH0 (blur H e input do upsample) / depth + AOH1 (blur V)
        BlurTableH0 = _SRVHeap.Allocate(2);
        BlurTableH1 = _SRVHeap.Allocate(2);
        {
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            D3D12_CPU_DESCRIPTOR_HANDLE DstH0 = _SRVHeap.CpuHandle(BlurTableH0);
            D3D12_CPU_DESCRIPTOR_HANDLE SrcH0[2] = {
                _SRVHeap.CpuHandleStaging(_DepthSRVSlot),
                _SRVHeap.CpuHandleStaging(AOH0SRVSlot),
            };
            _Device->CopyDescriptors(1, &DstH0, &DstCount, 2, SrcH0, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            D3D12_CPU_DESCRIPTOR_HANDLE DstH1 = _SRVHeap.CpuHandle(BlurTableH1);
            D3D12_CPU_DESCRIPTOR_HANDLE SrcH1[2] = {
                _SRVHeap.CpuHandleStaging(_DepthSRVSlot),
                _SRVHeap.CpuHandleStaging(AOH1SRVSlot),
            };
            _Device->CopyDescriptors(1, &DstH1, &DstCount, 2, SrcH1, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        Ready = true;
    }

    void FAmbientOcclusion::UpdatePerFrame(u32 _FrameSlot, f32 _M00, f32 _M11, f32 _M22,
                                           f32 _M32, const Mat44& _View, u32 _Width, u32 _Height,
                                           u32 _FrameIndex) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.ProjA        = { _M00, _M11, _M22, _M32 };
        CPU.ScreenParams = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.Params       = { Radius, Intensity, Power, Radius };
        // % 12 = periodo do ciclo temporal do shader (rot 6 frames x offset 4 frames);
        // mantem o f32 exato independente do tempo de sessao.
        CPU.Params2      = { (f32)NumDir, (f32)NumSteps, (f32)(_FrameIndex % 12u), 0.0f };
        CPU.Params3      = { FadeStart, FadeEnd, HalfRes ? 2.0f : 1.0f, 0.0f };
        CPU.ViewMatrix   = _View;

        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot * 2 + 0) * sizeof(GTAOConstants),
                    &CPU, sizeof(GTAOConstants));
        CPU.Params2.W = 1.0f;
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot * 2 + 1) * sizeof(GTAOConstants),
                    &CPU, sizeof(GTAOConstants));
    }

    void FAmbientOcclusion::Transition(ID3D12GraphicsCommandList* _CommandList, ID3D12Resource* _Resource,
                                       D3D12_RESOURCE_STATES& _State, D3D12_RESOURCE_STATES _After) {
        if (_State == _After) return;
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = _Resource;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = _State;
        Barrier.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &Barrier);
        _State = _After;
    }

    void FAmbientOcclusion::Execute(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                                    u32 _DepthSRVSlot) {
        if (!Ready) return;
        const UINT GX = (Width  + 7) / 8;
        const UINT GY = (Height + 7) / 8;

        if (HalfRes) {
            // main -> AOH0, blur H -> AOH1, blur V -> AOH0, upsample bilateral -> AO0 (final).
            // O AO0 full-res segue sendo a unica textura que o lighting le (t14).
            const u32  HalfW = (Width  + 1) / 2;
            const u32  HalfH = (Height + 1) / 2;
            const UINT HGX = (HalfW + 7) / 8;
            const UINT HGY = (HalfH + 7) / 8;

            Transition(_CommandList, AOH0.Get(), AOH0State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            MainPSO.Bind(_CommandList);
            _CommandList->SetComputeRootConstantBufferView(0, CBAddr(0));
            _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(MainTable));
            _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AOH0UAVSlot));
            _CommandList->Dispatch(HGX, HGY, 1);

            Transition(_CommandList, AOH0.Get(), AOH0State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CommandList, AOH1.Get(), AOH1State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            BlurPSO.Bind(_CommandList);
            _CommandList->SetComputeRootConstantBufferView(0, CBAddr(0));
            _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(BlurTableH0));
            _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AOH1UAVSlot));
            _CommandList->Dispatch(HGX, HGY, 1);

            Transition(_CommandList, AOH1.Get(), AOH1State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CommandList, AOH0.Get(), AOH0State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            _CommandList->SetComputeRootConstantBufferView(0, CBAddr(1));
            _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(BlurTableH1));
            _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AOH0UAVSlot));
            _CommandList->Dispatch(HGX, HGY, 1);

            Transition(_CommandList, AOH0.Get(), AOH0State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CommandList, AO0.Get(), AO0State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            UpsamplePSO.Bind(_CommandList);
            _CommandList->SetComputeRootConstantBufferView(0, CBAddr(0));
            _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(BlurTableH0));
            _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AO0UAVSlot));
            _CommandList->Dispatch(GX, GY, 1);

            Transition(_CommandList, AO0.Get(), AO0State, kAORead);
            return;
        }

        Transition(_CommandList, AO0.Get(), AO0State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        MainPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr(0));
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(MainTable));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AO0UAVSlot));
        _CommandList->Dispatch(GX, GY, 1);

        Transition(_CommandList, AO0.Get(), AO0State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CommandList, AO1.Get(), AO1State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        BlurPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr(0)); 
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(BlurTable0));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AO1UAVSlot));
        _CommandList->Dispatch(GX, GY, 1);

        Transition(_CommandList, AO1.Get(), AO1State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CommandList, AO0.Get(), AO0State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr(1)); 
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(BlurTable1));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AO0UAVSlot));
        _CommandList->Dispatch(GX, GY, 1);

        Transition(_CommandList, AO0.Get(), AO0State, kAORead);
    }

    void FAmbientOcclusion::ClearToWhite(ID3D12GraphicsCommandList* _CommandList,
                                         FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;
        Transition(_CommandList, AO0.Get(), AO0State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float White[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        _CommandList->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(AO0UAVSlot),
                                           _SRVHeap.CpuHandleStaging(AO0UAVSlot),
                                           AO0.Get(), White, 0, nullptr);
        Transition(_CommandList, AO0.Get(), AO0State, kAORead);
    }
}
