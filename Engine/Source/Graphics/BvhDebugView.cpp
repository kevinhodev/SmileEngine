#include "Smile/Graphics/BvhDebugView.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        // RGBA8 e nao RGBA16F: este passe ja resolve a cor final (paleta de categoria, rampa de
        // falsa-cor), entao nao ha faixa HDR p/ preservar — o alvo e registrado como Raw e vai
        // direto p/ o visualizador. Metade da banda e da VRAM de um RGBA16F de tela cheia.
        constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    void FBvhDebugView::Initialize(ID3D12Device* _Device) {
        CreatePipelines(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FBvhDebugView::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = sizeof(BvhDebugConstants) * FCommandQueue::kFramesInFlight;
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

    void FBvhDebugView::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                               u32 _Width, u32 _Height) {
        if (SRVSlot  != kInvalidSlot) { _SRVHeap.Free(SRVSlot);     SRVSlot  = kInvalidSlot; }
        if (UAVSlot  != kInvalidSlot) { _SRVHeap.Free(UAVSlot);     UAVSlot  = kInvalidSlot; }
        if (UAVTable != kInvalidSlot) { _SRVHeap.Free(UAVTable, 1); UAVTable = kInvalidSlot; }
        Target.Reset();
        State  = D3D12_RESOURCE_STATE_COMMON;
        Width_ = Height_ = 0;
        if (!Initialized || _Width == 0 || _Height == 0) return;

        Target = GpuResources::CreateTex2D(
            _Device, _Width, _Height, kFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, EVramCategory::Misc, nullptr, 1, 1,
            "Visualizador de BVH");

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kFormat;
        Srv.Texture2D.MipLevels     = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kFormat;

        SRVSlot  = _SRVHeap.Allocate(1);
        UAVSlot  = _SRVHeap.Allocate(1);
        UAVTable = _SRVHeap.Allocate(1); // u0 do dispatch (o UAVSlot solto e p/ o clear/visualizador)
        _SRVHeap.CreateSRV(_Device, Target.Get(), Srv, SRVSlot);
        _SRVHeap.CreateUAV(_Device, Target.Get(), Uav, UAVSlot);
        _SRVHeap.CreateUAV(_Device, Target.Get(), Uav, UAVTable);

        Width_  = _Width;
        Height_ = _Height;
    }

    void FBvhDebugView::SetSceneSRVs(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _TlasSRVSlot, u32 _InstanceSRVSlot) {
        SceneTableReady = false;
        if (!Initialized) return;
        if (_TlasSRVSlot == kInvalidSlot || _InstanceSRVSlot == kInvalidSlot) return;

        if (SceneTable == kInvalidSlot) SceneTable = _SRVHeap.Allocate(2);

        D3D12_CPU_DESCRIPTOR_HANDLE Src[2] = {
            _SRVHeap.CpuHandleStaging(_TlasSRVSlot),
            _SRVHeap.CpuHandleStaging(_InstanceSRVSlot),
        };
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(SceneTable);
        UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
        _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Src, SrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        SceneTableReady = true;
    }

    void FBvhDebugView::Release(FTextureSRVHeap& _SRVHeap) {
        if (SRVSlot    != kInvalidSlot) { _SRVHeap.Free(SRVSlot);       SRVSlot    = kInvalidSlot; }
        if (UAVSlot    != kInvalidSlot) { _SRVHeap.Free(UAVSlot);       UAVSlot    = kInvalidSlot; }
        if (UAVTable   != kInvalidSlot) { _SRVHeap.Free(UAVTable, 1);   UAVTable   = kInvalidSlot; }
        if (SceneTable != kInvalidSlot) { _SRVHeap.Free(SceneTable, 2); SceneTable = kInvalidSlot; }
        SceneTableReady = false;
        Target.Reset();
        State = D3D12_RESOURCE_STATE_COMMON;
        if (CB && MappedCB) { CB->Unmap(0, nullptr); MappedCB = nullptr; }
        CB.Reset();
        Width_ = Height_ = 0;
        Initialized = false;
    }

    void FBvhDebugView::Transition(ID3D12GraphicsCommandList* _CL, D3D12_RESOURCE_STATES _After) {
        if (!Target || State == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Target.Get();
        B.Transition.StateBefore = State;
        B.Transition.StateAfter  = _After;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _CL->ResourceBarrier(1, &B);
        State = _After;
    }

    void FBvhDebugView::Render(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap,
                               const Mat44& _InvViewProj, const Vec3& _CameraPos,
                               EMode _Mode, f32 _MaxRayDist, f32 _ComplexityMax, u32 _FrameSlot) {
        if (!IsReady() || !MappedCB) return;

        const u32 Slot = _FrameSlot % FCommandQueue::kFramesInFlight;
        BvhDebugConstants C{};
        C.InvViewProj  = _InvViewProj;
        C.CameraPos    = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 0.0f };
        C.ScreenParams = { static_cast<f32>(Width_), static_cast<f32>(Height_),
                           1.0f / static_cast<f32>(Width_), 1.0f / static_cast<f32>(Height_) };
        C.Params       = { static_cast<f32>(_Mode), _MaxRayDist,
                           _ComplexityMax > 1.0f ? _ComplexityMax : kDefaultComplexityMax, 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(Slot) * sizeof(BvhDebugConstants),
                    &C, sizeof(C));

        Transition(_CL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        PSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(
            0, CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(Slot) * sizeof(BvhDebugConstants));
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SceneTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(UAVTable));
        _CL->Dispatch((Width_ + 7) / 8, (Height_ + 7) / 8, 1);
    }

    void FBvhDebugView::ToRead(ID3D12GraphicsCommandList* _CL) {
        // Mesmo estado combinado dos outros alvos publicados no DebugTargets: o modo grade do
        // visualizador pode ler varios de uma vez, e ele e um passe grafico.
        Transition(_CL, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void FBvhDebugView::CreatePipelines(ID3D12Device* _Device) {
        PSO.Initialize(_Device, "BvhDebug.cs_6_6.cso", 2, 1, /*HeapDirectlyIndexed=*/true);
    }


    FPassShaderStems FBvhDebugView::ShaderStems() const {
        static const char* const kStems[] = { "BvhDebug.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FBvhDebugView::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
