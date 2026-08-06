#include "Smile/Graphics/DlssRRGuides.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kGuideFormat  = DXGI_FORMAT_R16G16B16A16_FLOAT; // albedo difuso/especular + normal-rough
        constexpr DXGI_FORMAT kHitFormat    = DXGI_FORMAT_R16_FLOAT;          // specHitDist (scalar)

        ComPtr<ID3D12Resource> CreateUAVTex2D(ID3D12Device* Device, u32 W, u32 H, DXGI_FORMAT Fmt,
                                              bool AllowRenderTarget = false) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = W;
            Desc.Height           = H;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = Fmt;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                    (AllowRenderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                                       : D3D12_RESOURCE_FLAG_NONE);
            ComPtr<ID3D12Resource> Tex;
            D3D12_CLEAR_VALUE Clear{};
            Clear.Format = Fmt;
            SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, AllowRenderTarget ? &Clear : nullptr,
                     IID_PPV_ARGS(&Tex)));
            VramTracker::Register(Tex.Get(), EVramCategory::RenderTargets);
            return Tex;
        }
    }

    void FDlssRRGuides::Initialize(ID3D12Device* Device) {
        RecreatePSOs(Device);
        Initialized = true;
    }

    void FDlssRRGuides::RecreatePSOs(ID3D12Device* Device) {
        GuidesPSO.Initialize(Device, "DlssRRGuides.cs_6_0.cso", 4, 3, false);
        SpecHitPSO.Initialize(Device, "DlssRRSpecHit.cs_6_0.cso", 1, 1, false);
        WaterSpecHitPSO.Initialize(Device, "DlssRRWaterSpecHit.cs_6_0.cso", 2, 1, false);
    }

    void FDlssRRGuides::Shutdown() {
        DiffAlb.Reset(); SpecAlb.Reset(); NrmRough.Reset(); SpecHit.Reset();
        Ready = false; Initialized = false;
    }

    void FDlssRRGuides::ReleaseResize(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(MainUavTable, 3);
        Free(SpecHitUav, 1);
        DiffAlb.Reset(); SpecAlb.Reset(); NrmRough.Reset(); SpecHit.Reset();
        DiffAlbState = SpecAlbState = NrmRoughState = SpecHitState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FDlssRRGuides::SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                       u32 W, u32 H) {
        if (!Initialized) return;
        ReleaseResize(SRVHeap);
        if (W == 0 || H == 0) return;
        Width = W; Height = H;

        DiffAlb  = CreateUAVTex2D(Device, W, H, kGuideFormat);
        SpecAlb  = CreateUAVTex2D(Device, W, H, kGuideFormat);
        NrmRough = CreateUAVTex2D(Device, W, H, kGuideFormat);
        SpecHit  = CreateUAVTex2D(Device, W, H, kHitFormat, true);
        DiffAlbState = SpecAlbState = NrmRoughState = SpecHitState = D3D12_RESOURCE_STATE_COMMON;

        // 3 UAVs contiguos p/ a tabela do guides pass (u0-u2), + 1 UAV standalone p/ o specHitDist.
        MainUavTable = SRVHeap.Allocate(3);
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format = kGuideFormat;
        SRVHeap.CreateUAV(Device, DiffAlb.Get(),  Uav, MainUavTable + 0);
        SRVHeap.CreateUAV(Device, SpecAlb.Get(),  Uav, MainUavTable + 1);
        SRVHeap.CreateUAV(Device, NrmRough.Get(), Uav, MainUavTable + 2);

        SpecHitUav = SRVHeap.Allocate(1);
        Uav.Format = kHitFormat;
        SRVHeap.CreateUAV(Device, SpecHit.Get(), Uav, SpecHitUav);

        if (!SpecHitRTVHeap.Native())
            SpecHitRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        D3D12_RENDER_TARGET_VIEW_DESC Rtv{};
        Rtv.Format = kHitFormat;
        Rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device->CreateRenderTargetView(SpecHit.Get(), &Rtv, SpecHitRTVHeap.CpuHandle(0));

        Ready = true;
    }

    void FDlssRRGuides::Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                                   D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After) {
        if (!Res || State == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Res;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = State;
        B.Transition.StateAfter  = After;
        CL->ResourceBarrier(1, &B);
        State = After;
    }

    void FDlssRRGuides::RecordGuides(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                                     D3D12_GPU_DESCRIPTOR_HANDLE GBufferTable,
                                     D3D12_GPU_VIRTUAL_ADDRESS FrameCB) {
        if (!Ready) return;
        Transition(CL, DiffAlb.Get(),  DiffAlbState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, SpecAlb.Get(),  SpecAlbState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, NrmRough.Get(), NrmRoughState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        GuidesPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, FrameCB);
        CL->SetComputeRootDescriptorTable(1, GBufferTable);
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(MainUavTable));
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        CL->Dispatch(GX, GY, 1);
    }

    void FDlssRRGuides::RecordSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                                          D3D12_GPU_DESCRIPTOR_HANDLE ResolvedSrv,
                                          D3D12_GPU_VIRTUAL_ADDRESS FrameCB) {
        if (!Ready) return;
        Transition(CL, SpecHit.Get(), SpecHitState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpecHitPSO.Bind(CL);
        // O shader nao le o b0, mas o root sig o declara — deixar o slot vazio e legal em D3D12 e
        // ruidoso no GPU-based validator. Preenche com o CB do frame (custo zero).
        CL->SetComputeRootConstantBufferView(0, FrameCB);
        CL->SetComputeRootDescriptorTable(1, ResolvedSrv);
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SpecHitUav));
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        CL->Dispatch(GX, GY, 1);
    }

    void FDlssRRGuides::RecordWaterSpecHitDist(ID3D12GraphicsCommandList* CL,
                                                FTextureSRVHeap& SRVHeap,
                                                D3D12_GPU_DESCRIPTOR_HANDLE WaterTable,
                                                D3D12_GPU_VIRTUAL_ADDRESS FrameCB) {
        if (!Ready) return;
        Transition(CL, SpecHit.Get(), SpecHitState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        WaterSpecHitPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, FrameCB);
        CL->SetComputeRootDescriptorTable(1, WaterTable);
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SpecHitUav));
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        CL->Dispatch(GX, GY, 1);
    }

    void FDlssRRGuides::ClearSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!Ready) return;
        Transition(CL, SpecHit.Get(), SpecHitState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CL->ClearUnorderedAccessViewFloat(SRVHeap.GpuHandle(SpecHitUav),
                                          SRVHeap.CpuHandleStaging(SpecHitUav),
                                          SpecHit.Get(), Zero, 0, nullptr);
    }

    void FDlssRRGuides::PrepareSpecHitForWater(ID3D12GraphicsCommandList* CL) {
        if (!Ready) return;
        Transition(CL, SpecHit.Get(), SpecHitState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    void FDlssRRGuides::TransitionForRR(ID3D12GraphicsCommandList* CL) {
        Transition(CL, DiffAlb.Get(),  DiffAlbState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, SpecAlb.Get(),  SpecAlbState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, NrmRough.Get(), NrmRoughState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, SpecHit.Get(),  SpecHitState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
}
