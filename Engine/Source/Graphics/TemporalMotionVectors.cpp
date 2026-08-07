#include "Smile/Graphics/TemporalMotionVectors.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/HResultCheck.h"
#include <algorithm>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        ComPtr<ID3D12Resource> CreateTexture2D(ID3D12Device* Device, u32 Width, u32 Height,
                                               DXGI_FORMAT Format, const char* Label = nullptr) {
            return GpuResources::CreateTex2D(Device, Width, Height, Format,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI,
                                             nullptr, 1, 1, Label);
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* Device, UINT64 Size, u8** Mapped) {
            // Alinhamento de CB desligado: aqui o chamador ja passa o tamanho total e fatia
            // o mapeado na mao pelo FrameSlot.
            GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(Device, Size, 1, /*ForConstantBuffer*/ false);
            *Mapped = Upload.Mapped;
            return Upload.Resource;
        }
    }

    void FTemporalMotionVectors::Initialize(ID3D12Device* Device) {
        CreatePipelines(Device);
        CB = CreateUploadBuffer(Device,
            static_cast<UINT64>(kFrames) * sizeof(FTemporalMotionConstants), &MappedCB);
        Initialized = true;
    }

    void FTemporalMotionVectors::ReleaseResize(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        for (u32 i = 0; i < kFrames; ++i) {
            Free(SurfaceSRVSlot[i], 1);
            Free(SurfaceUAVSlot[i], 1);
            Free(SurfaceTable[i], 3);
            Free(DualTable[i], 4);
            Surface[i].Reset();
            SurfaceState[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        Free(ReliableMotionSRV, 1);
        Free(ReliableMotionUAV, 1);
        ReliableMotion.Reset();
        ReliableMotionState = D3D12_RESOURCE_STATE_COMMON;
        Width = Height = 0;
        Ready = false;
        HistoryValid = false;
    }

    void FTemporalMotionVectors::ReleaseScene(FTextureSRVHeap& SRVHeap) {
        ReleaseResize(SRVHeap);
        for (u32& Slot : TransformSRVSlot) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, 1); Slot = kInvalidSlot; }
        }
        TransformBuffer.Reset();
        MappedTransforms = nullptr;
        PreviousModels.clear();
        InstanceCount_ = 0;
        TlasSRVSlot = InstanceGeoSRVSlot = kInvalidSlot;
        SceneReady = false;
    }

    void FTemporalMotionVectors::SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                                const FScene& Scene, u32 TlasSlot,
                                                u32 InstanceGeoSlot) {
        if (!Initialized) return;
        ReleaseScene(SRVHeap);
        const u32 Count = static_cast<u32>(Scene.Renderables().size());
        if (Count == 0 || TlasSlot == kInvalidSlot || InstanceGeoSlot == kInvalidSlot) return;

        // Capacidade com folga (SceneCapacityFor), nao o tamanho exato da cena: assim um objeto
        // criado no editor cabe sem refazer buffer e SRVs. InstanceCount_ passa a ser a
        // CAPACIDADE — e ele que o UpdatePerFrame usa como stride do slice por frame e no gate
        // `PreviousModels.size() == InstanceCount_`, entao o PreviousModels abaixo tem de ser
        // dimensionado igual, senao o gate falha para sempre e o motion vector nunca reusa
        // historico (todo frame sairia com movimento zero).
        const u32 Capacity = SceneCapacityFor(Count);
        InstanceCount_ = Capacity;
        TlasSRVSlot = TlasSlot;
        InstanceGeoSRVSlot = InstanceGeoSlot;
        TransformBuffer = CreateUploadBuffer(Device,
            static_cast<UINT64>(kFrames) * Capacity * sizeof(FTemporalInstanceTransformGPU),
            &MappedTransforms);

        for (u32 f = 0; f < kFrames; ++f) {
            TransformSRVSlot[f] = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format                     = DXGI_FORMAT_UNKNOWN;
            S.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            S.Buffer.FirstElement        = static_cast<UINT64>(f) * Capacity;
            S.Buffer.NumElements         = Capacity;
            S.Buffer.StructureByteStride = sizeof(FTemporalInstanceTransformGPU);
            SRVHeap.CreateSRV(Device, TransformBuffer.Get(), S, TransformSRVSlot[f]);
        }

        // Dimensionado pela CAPACIDADE (ver acima); so os [0, Count) existem na cena, o resto
        // fica identidade ate um objeto novo ocupar o slot.
        PreviousModels.resize(Capacity, Mat44::Identity());
        for (u32 i = 0; i < Count; ++i)
            PreviousModels[i] = Scene.Renderables()[i].Transform.Matrix();
        SceneReady = true;
    }

    void FTemporalMotionVectors::SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                                 u32 NewWidth, u32 NewHeight,
                                                 u32 DepthSlot, u32 VelocitySlot) {
        if (!Initialized) return;
        ReleaseResize(SRVHeap);
        if (!SceneReady || NewWidth == 0 || NewHeight == 0 ||
            DepthSlot == kInvalidSlot || VelocitySlot == kInvalidSlot) return;

        Width = NewWidth; Height = NewHeight;
        constexpr DXGI_FORMAT SurfaceFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constexpr DXGI_FORMAT MotionFormat  = DXGI_FORMAT_R16G16_FLOAT;
        for (u32 i = 0; i < kFrames; ++i)
            Surface[i] = CreateTexture2D(Device, Width, Height, SurfaceFormat,
                                         "Historico de superficie");
        ReliableMotion = CreateTexture2D(Device, Width, Height, MotionFormat,
                                         "Motion confiavel");

        auto MakeViews = [&](ID3D12Resource* Resource, DXGI_FORMAT Format, u32& SRV, u32& UAV) {
            SRV = SRVHeap.Allocate(1); UAV = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format = Format;
            S.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            S.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.Texture2D.MipLevels = 1;
            SRVHeap.CreateSRV(Device, Resource, S, SRV);
            D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
            U.Format = Format;
            U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            SRVHeap.CreateUAV(Device, Resource, U, UAV);
        };
        for (u32 i = 0; i < kFrames; ++i)
            MakeViews(Surface[i].Get(), SurfaceFormat, SurfaceSRVSlot[i], SurfaceUAVSlot[i]);
        MakeViews(ReliableMotion.Get(), MotionFormat, ReliableMotionSRV, ReliableMotionUAV);

        auto CopyTable = [&](u32 DstSlot, const u32* Slots, u32 Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[4]{};
            UINT Counts[4]{};
            for (u32 i = 0; i < Count; ++i) {
                Src[i] = SRVHeap.CpuHandleStaging(Slots[i]);
                Counts[i] = 1;
            }
            UINT DstCount = Count;
            Device->CopyDescriptors(1, &Dst, &DstCount, Count, Src, Counts,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        for (u32 f = 0; f < kFrames; ++f) {
            SurfaceTable[f] = SRVHeap.Allocate(3);
            const u32 SurfaceSlots[3] = { DepthSlot, TlasSRVSlot, InstanceGeoSRVSlot };
            CopyTable(SurfaceTable[f], SurfaceSlots, 3);

            DualTable[f] = SRVHeap.Allocate(4);
            const u32 DualSlots[4] = {
                VelocitySlot, SurfaceSRVSlot[f], SurfaceSRVSlot[1u - f], TransformSRVSlot[f] };
            CopyTable(DualTable[f], DualSlots, 4);
        }

        Ready = true;
        HistoryValid = false;
    }

    void FTemporalMotionVectors::UpdatePerFrame(u32 NewFrameSlot, const FScene& Scene,
                                                 const Mat44& InvViewProj,
                                                 const Mat44& ViewProj,
                                                 const Mat44& PrevViewProj,
                                                 const Vec3& CameraPos) {
        FrameSlot = NewFrameSlot % kFrames;
        if (!Ready || !MappedTransforms || !MappedCB) return;

        const u32 Count = std::min(InstanceCount_, static_cast<u32>(Scene.Renderables().size()));
        auto* Dst = reinterpret_cast<FTemporalInstanceTransformGPU*>(MappedTransforms) +
                    static_cast<size_t>(FrameSlot) * InstanceCount_;
        const bool CanReuseTransforms = HistoryValid && PreviousModels.size() == InstanceCount_;
        for (u32 i = 0; i < Count; ++i) {
            const Mat44 Current = Scene.Renderables()[i].Transform.Matrix();
            const Mat44 Previous = CanReuseTransforms ? PreviousModels[i] : Current;
            Dst[i].CurrentToPrevious = Current.Inverse() * Previous;
            Dst[i].PreviousToCurrent = Previous.Inverse() * Current;
            PreviousModels[i] = Current;
        }
        for (u32 i = Count; i < InstanceCount_; ++i) {
            Dst[i].CurrentToPrevious = Mat44::Identity();
            Dst[i].PreviousToCurrent = Mat44::Identity();
        }

        CPU.InvViewProj = InvViewProj;
        CPU.ViewProj = ViewProj;
        CPU.PrevViewProj = PrevViewProj;
        CPU.CameraPos = { CameraPos.X, CameraPos.Y, CameraPos.Z, 0.0f };
        CPU.ScreenParams = { static_cast<f32>(Width), static_cast<f32>(Height),
                             1.0f / static_cast<f32>(Width),
                             1.0f / static_cast<f32>(Height) };
        CPU.Params = { static_cast<f32>(FrameSlot), HistoryValid ? 1.0f : 0.0f,
                       PositionRejectScale, static_cast<f32>(Count) };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(FTemporalMotionConstants),
                    &CPU, sizeof(CPU));
    }

    D3D12_GPU_VIRTUAL_ADDRESS FTemporalMotionVectors::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(FTemporalMotionConstants);
    }

    void FTemporalMotionVectors::Transition(ID3D12GraphicsCommandList* CL,
                                              ID3D12Resource* Resource,
                                              D3D12_RESOURCE_STATES& State,
                                              D3D12_RESOURCE_STATES After) {
        if (!Resource || State == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource = Resource;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = State;
        B.Transition.StateAfter = After;
        CL->ResourceBarrier(1, &B);
        State = After;
    }

    void FTemporalMotionVectors::Record(ID3D12GraphicsCommandList* CL,
                                         FTextureSRVHeap& SRVHeap,
                                         FGpuProfiler* Profiler) {
        if (!Ready || !CL) return;
        const u32 Curr = FrameSlot;
        if (Profiler) Profiler->Begin(CL, "Captura de superficie");
        Transition(CL, Surface[Curr].Get(), SurfaceState[Curr],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SurfacePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(SurfaceTable[Curr]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SurfaceUAVSlot[Curr]));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        Transition(CL, Surface[Curr].Get(), SurfaceState[Curr],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Profiler) Profiler->End(CL);

        if (Profiler) Profiler->Begin(CL, "Dual motion");
        Transition(CL, ReliableMotion.Get(), ReliableMotionState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DualPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(DualTable[Curr]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(ReliableMotionUAV));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        Transition(CL, ReliableMotion.Get(), ReliableMotionState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Profiler) Profiler->End(CL);
        HistoryValid = true;
    }

    void FTemporalMotionVectors::CreatePipelines(ID3D12Device* _Device) {
        SurfacePSO.Initialize(_Device, "TemporalSurface.cs_6_6.cso", 3, 1, true);
        DualPSO.Initialize(_Device, "DualMotion.cs_6_0.cso", 4, 1, false);
    }


    FPassShaderStems FTemporalMotionVectors::ShaderStems() const {
        static const char* const kStems[] = { "TemporalSurface.cs", "DualMotion.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FTemporalMotionVectors::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
