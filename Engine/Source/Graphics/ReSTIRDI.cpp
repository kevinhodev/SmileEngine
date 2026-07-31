#include "Smile/Graphics/ReSTIRDI.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT kResAFormat   = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constexpr DXGI_FORMAT kResBFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr u32 kInitialSRVs = 8;
        constexpr u32 kInitialUAVs = 2;
        constexpr u32 kSpatialSRVs = 9;
        constexpr u32 kSpatialUAVs = 3;
        constexpr u32 kNrdPackSRVs = 7;
        constexpr u32 kNrdPackUAVs = 5;
        constexpr u32 kNrdCompositeSRVs = 6;

        ComPtr<ID3D12Resource> CreateUAVTexture(ID3D12Device* Device, u32 Width, u32 Height,
                                                DXGI_FORMAT Format) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width = Width;
            Desc.Height = Height;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels = 1;
            Desc.Format = Format;
            Desc.SampleDesc = { 1, 0 };
            Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> Result;
            SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Result)));
            VramTracker::Register(Result.Get(), EVramCategory::RenderTargets);
            return Result;
        }
    }

    void FReSTIRDI::Initialize(ID3D12Device* Device) {
        InitialTemporalPSO.Initialize(Device, "ReSTIRDIInitialTemporal.cs_6_6.cso",
                                      kInitialSRVs, kInitialUAVs, false);
        SpatialPSO.Initialize(Device, "ReSTIRDISpatial.cs_6_6.cso",
                              kSpatialSRVs, kSpatialUAVs, true);
        NrdPackPSO.Initialize(Device, "ReSTIRDINrdPack.cs_6_6.cso",
                              kNrdPackSRVs, kNrdPackUAVs, false);
        NrdCompositePSO.Initialize(Device, "ReSTIRDINrdComposite.cs_6_6.cso",
                                   kNrdCompositeSRVs, 1, false);
        CreateConstantBuffer(Device);
        Initialized = true;
    }

    void FReSTIRDI::RecreatePSO(ID3D12Device* Device) {
        if (!Initialized) return;
        InitialTemporalPSO.Initialize(Device, "ReSTIRDIInitialTemporal.cs_6_6.cso",
                                      kInitialSRVs, kInitialUAVs, false);
        SpatialPSO.Initialize(Device, "ReSTIRDISpatial.cs_6_6.cso",
                              kSpatialSRVs, kSpatialUAVs, true);
        NrdPackPSO.Initialize(Device, "ReSTIRDINrdPack.cs_6_6.cso",
                              kNrdPackSRVs, kNrdPackUAVs, false);
        NrdCompositePSO.Initialize(Device, "ReSTIRDINrdComposite.cs_6_6.cso",
                                   kNrdCompositeSRVs, 1, false);
    }

    void FReSTIRDI::CreateConstantBuffer(ID3D12Device* Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                            sizeof(ReSTIRDIConstants);
        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width = Size;
        Desc.Height = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels = 1;
        Desc.Format = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc = { 1, 0 };
        Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));
    }

    D3D12_GPU_VIRTUAL_ADDRESS FReSTIRDI::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<u64>(FrameSlot) * sizeof(ReSTIRDIConstants);
    }

    void FReSTIRDI::ReleaseResize(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(OutSRV, 1); Free(OutUAV, 1);
        Free(RawDiffuseSRV, 1); Free(RawDiffuseUAV, 1);
        Free(RawSpecularSRV, 1); Free(RawSpecularUAV, 1);
        Free(SpatialUAVTable, kSpatialUAVs);
        Free(NrdPackSrvTable, kNrdPackSRVs);
        Free(NrdPackUavTable, kNrdPackUAVs);
        Free(NrdCompositeSrvTable, kNrdCompositeSRVs);
        Free(NrdOutDiffuseSRV, 1); Free(NrdOutSpecularSRV, 1);
        for (u32 p = 0; p < kParityCount; ++p) {
            Free(ResASRV[p], 1); Free(ResBSRV[p], 1);
            Free(ResAUAV[p], 1); Free(ResBUAV[p], 1);
            Free(InitialUAVTable[p], kInitialUAVs);
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                Free(InitialTable[p][f], kInitialSRVs);
                Free(SpatialTable[p][f], kSpatialSRVs);
            }
            ResA[p].Reset(); ResB[p].Reset();
            ResAState[p] = ResBState[p] = D3D12_RESOURCE_STATE_COMMON;
        }
        Output.Reset(); RawDiffuse.Reset(); RawSpecular.Reset();
        OutputState = RawDiffuseState = RawSpecularState = D3D12_RESOURCE_STATE_COMMON;
        NrdReady = false;
        Ready = false;
    }

    void FReSTIRDI::SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                   u32 NewWidth, u32 NewHeight,
                                   u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                                   u32 DepthSlot, u32 VelocitySlot, u32 TlasSlot, u32 InstanceSlot,
                                   const u32 LightSlots[FCommandQueue::kFramesInFlight]) {
        if (!Initialized) return;
        ReleaseResize(SRVHeap);
        if (NewWidth == 0 || NewHeight == 0 ||
            GBufferASlot == kInvalidSlot || GBufferBSlot == kInvalidSlot ||
            GBufferCSlot == kInvalidSlot || DepthSlot == kInvalidSlot ||
            VelocitySlot == kInvalidSlot || TlasSlot == kInvalidSlot ||
            InstanceSlot == kInvalidSlot) return;
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f)
            if (LightSlots[f] == kInvalidSlot) return;

        Width = NewWidth; Height = NewHeight;
        Output = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        RawDiffuse = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        RawSpecular = CreateUAVTexture(Device, Width, Height, kOutputFormat);
        for (u32 p = 0; p < kParityCount; ++p) {
            ResA[p] = CreateUAVTexture(Device, Width, Height, kResAFormat);
            ResB[p] = CreateUAVTexture(Device, Width, Height, kResBFormat);
        }

        auto MakeViews = [&](ID3D12Resource* Resource, DXGI_FORMAT Format,
                             u32& SRV, u32& UAV) {
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
        MakeViews(Output.Get(), kOutputFormat, OutSRV, OutUAV);
        MakeViews(RawDiffuse.Get(), kOutputFormat, RawDiffuseSRV, RawDiffuseUAV);
        MakeViews(RawSpecular.Get(), kOutputFormat, RawSpecularSRV, RawSpecularUAV);
        for (u32 p = 0; p < kParityCount; ++p) {
            MakeViews(ResA[p].Get(), kResAFormat, ResASRV[p], ResAUAV[p]);
            MakeViews(ResB[p].Get(), kResBFormat, ResBSRV[p], ResBUAV[p]);
        }

        auto CopyTable = [&](u32 DstSlot, const u32* Slots, u32 Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[9]{};
            UINT SrcCounts[9]{};
            for (u32 i = 0; i < Count; ++i) {
                Src[i] = SRVHeap.CpuHandleStaging(Slots[i]);
                SrcCounts[i] = 1;
            }
            UINT DstCount = Count;
            Device->CopyDescriptors(1, &Dst, &DstCount, Count, Src, SrcCounts,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        SpatialUAVTable = SRVHeap.Allocate(kSpatialUAVs);
        const u32 SpatialUAVSlots[kSpatialUAVs] = {
            OutUAV, RawDiffuseUAV, RawSpecularUAV };
        CopyTable(SpatialUAVTable, SpatialUAVSlots, kSpatialUAVs);

        for (u32 p = 0; p < kParityCount; ++p) {
            const u32 Prev = 1u - p;
            InitialUAVTable[p] = SRVHeap.Allocate(kInitialUAVs);
            const u32 InitialUAVSlots[kInitialUAVs] = { ResAUAV[p], ResBUAV[p] };
            CopyTable(InitialUAVTable[p], InitialUAVSlots, kInitialUAVs);

            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                InitialTable[p][f] = SRVHeap.Allocate(kInitialSRVs);
                const u32 InitialSlots[kInitialSRVs] = {
                    GBufferASlot, GBufferBSlot, GBufferCSlot, DepthSlot, VelocitySlot,
                    ResASRV[Prev], ResBSRV[Prev], LightSlots[f] };
                CopyTable(InitialTable[p][f], InitialSlots, kInitialSRVs);

                SpatialTable[p][f] = SRVHeap.Allocate(kSpatialSRVs);
                const u32 SpatialSlots[kSpatialSRVs] = {
                    GBufferASlot, GBufferBSlot, GBufferCSlot, DepthSlot, TlasSlot, InstanceSlot,
                    ResASRV[p], ResBSRV[p], LightSlots[f] };
                CopyTable(SpatialTable[p][f], SpatialSlots, kSpatialSRVs);
            }
        }

        FrameParity = 0;
        NeedsClear = true;
        Ready = true;
    }

    void FReSTIRDI::SetupNrdPack(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                 u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                                 u32 DepthSlot, u32 VelocitySlot,
                                 ID3D12Resource* NrdInViewZ,
                                 ID3D12Resource* NrdInNormalRough,
                                 ID3D12Resource* NrdInMv,
                                 ID3D12Resource* NrdInDiffRadHit,
                                 ID3D12Resource* NrdInSpecRadHit,
                                 ID3D12Resource* NrdOutDiff,
                                 ID3D12Resource* NrdOutSpec) {
        NrdReady = false;
        if (!Ready || !NrdInViewZ || !NrdInNormalRough || !NrdInMv ||
            !NrdInDiffRadHit || !NrdInSpecRadHit || !NrdOutDiff || !NrdOutSpec ||
            GBufferASlot == kInvalidSlot || GBufferBSlot == kInvalidSlot ||
            GBufferCSlot == kInvalidSlot || DepthSlot == kInvalidSlot ||
            VelocitySlot == kInvalidSlot) return;

        auto MakeUAV = [&](ID3D12Resource* Resource, DXGI_FORMAT Format, u32& Slot) {
            Slot = SRVHeap.Allocate(1);
            D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
            U.Format = Format;
            U.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            SRVHeap.CreateUAV(Device, Resource, U, Slot);
        };
        auto MakeSRV = [&](ID3D12Resource* Resource, DXGI_FORMAT Format, u32& Slot) {
            Slot = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC S{};
            S.Format = Format;
            S.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            S.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            S.Texture2D.MipLevels = 1;
            SRVHeap.CreateSRV(Device, Resource, S, Slot);
        };
        auto CopyTable = [&](u32 DstSlot, const u32* Slots, u32 Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DstSlot);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[7]{};
            UINT SrcCounts[7]{};
            for (u32 i = 0; i < Count; ++i) {
                Src[i] = SRVHeap.CpuHandleStaging(Slots[i]);
                SrcCounts[i] = 1;
            }
            UINT DstCount = Count;
            Device->CopyDescriptors(1, &Dst, &DstCount, Count, Src, SrcCounts,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        u32 NrdInputUAV[kNrdPackUAVs]{};
        MakeUAV(NrdInViewZ,       DXGI_FORMAT_R32_FLOAT,             NrdInputUAV[0]);
        MakeUAV(NrdInNormalRough, DXGI_FORMAT_R10G10B10A2_UNORM,    NrdInputUAV[1]);
        MakeUAV(NrdInMv,          DXGI_FORMAT_R16G16_FLOAT,          NrdInputUAV[2]);
        MakeUAV(NrdInDiffRadHit,  DXGI_FORMAT_R16G16B16A16_FLOAT,   NrdInputUAV[3]);
        MakeUAV(NrdInSpecRadHit,  DXGI_FORMAT_R16G16B16A16_FLOAT,   NrdInputUAV[4]);
        MakeSRV(NrdOutDiff, DXGI_FORMAT_R16G16B16A16_FLOAT, NrdOutDiffuseSRV);
        MakeSRV(NrdOutSpec, DXGI_FORMAT_R16G16B16A16_FLOAT, NrdOutSpecularSRV);

        NrdPackSrvTable = SRVHeap.Allocate(kNrdPackSRVs);
        const u32 PackSRVs[kNrdPackSRVs] = {
            RawDiffuseSRV, RawSpecularSRV, GBufferASlot, GBufferBSlot, GBufferCSlot,
            DepthSlot, VelocitySlot };
        CopyTable(NrdPackSrvTable, PackSRVs, kNrdPackSRVs);

        NrdPackUavTable = SRVHeap.Allocate(kNrdPackUAVs);
        CopyTable(NrdPackUavTable, NrdInputUAV, kNrdPackUAVs);
        for (u32& Slot : NrdInputUAV) SRVHeap.Free(Slot, 1);

        NrdCompositeSrvTable = SRVHeap.Allocate(kNrdCompositeSRVs);
        const u32 CompositeSRVs[kNrdCompositeSRVs] = {
            NrdOutDiffuseSRV, NrdOutSpecularSRV,
            GBufferASlot, GBufferBSlot, GBufferCSlot, DepthSlot };
        CopyTable(NrdCompositeSrvTable, CompositeSRVs, kNrdCompositeSRVs);
        NrdReady = true;
    }

    void FReSTIRDI::UpdatePerFrame(u32 NewFrameSlot, const Mat44& InvViewProj,
                                   const Mat44& View,
                                   const Vec3& CameraPos, u32 NewWidth, u32 NewHeight,
                                   u32 FrameIndex, u32 LightCount, u32 ShadowRayMask,
                                   bool EnableTemporalPermutation) {
        FrameSlot = NewFrameSlot;
        if (!MappedCB || NewWidth == 0 || NewHeight == 0) return;
        CPU.InvViewProj = InvViewProj;
        CPU.CameraPos = { CameraPos.X, CameraPos.Y, CameraPos.Z, 0.0f };
        CPU.ScreenParams = { static_cast<f32>(NewWidth), static_cast<f32>(NewHeight),
                             1.0f / static_cast<f32>(NewWidth),
                             1.0f / static_cast<f32>(NewHeight) };
        CPU.Params = { static_cast<f32>(LightCount), static_cast<f32>(FrameIndex),
                       static_cast<f32>(ShadowRayMask), RayEps.VisRayEndMargin };
        CPU.Sampling = { static_cast<f32>(InitialCandidates), MCap,
                         static_cast<f32>(SpatialCount), SpatialRadius };
        CPU.Reuse = { (Temporal && !NeedsClear) ? 1.0f : 0.0f,
                      PosRejectScale, NormalReject, MaxAge };
        CPU.TemporalPolicy = { EnableTemporalPermutation ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        CPU.RayEpsA = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                        RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB = { RayEps.ShadowRayTMin, RayEps.VisRayTMin,
                        RayEps.VisRayEndMargin, FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.View = View;
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReSTIRDIConstants),
                    &CPU, sizeof(CPU));
    }

    void FReSTIRDI::Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                               D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After) {
        if (State == After) return;
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = Resource;
        Barrier.Transition.StateBefore = State;
        Barrier.Transition.StateAfter = After;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CL->ResourceBarrier(1, &Barrier);
        State = After;
    }

    void FReSTIRDI::Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!Ready) return;
        const u32 P = FrameParity;
        const u32 Prev = 1u - P;
        Transition(CL, ResA[Prev].Get(), ResAState[Prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ResB[Prev].Get(), ResBState[Prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ResA[P].Get(), ResAState[P], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, ResB[P].Get(), ResBState[P], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, RawDiffuse.Get(), RawDiffuseState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, RawSpecular.Get(), RawSpecularState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        InitialTemporalPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(InitialTable[P][FrameSlot]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(InitialUAVTable[P]));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER Uav[2]{};
        for (u32 i = 0; i < 2; ++i) Uav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Uav[0].UAV.pResource = ResA[P].Get();
        Uav[1].UAV.pResource = ResB[P].Get();
        CL->ResourceBarrier(2, Uav);
        Transition(CL, ResA[P].Get(), ResAState[P], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ResB[P].Get(), ResBState[P], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        SpatialPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(SpatialTable[P][FrameSlot]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SpatialUAVTable));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);

        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(CL, RawDiffuse.Get(), RawDiffuseState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, RawSpecular.Get(), RawSpecularState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        NeedsClear = false;
        FrameParity = Prev;
    }

    void FReSTIRDI::RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!NrdReady) return;
        NrdPackPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(NrdPackSrvTable));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(NrdPackUavTable));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
    }

    void FReSTIRDI::RecordNrdComposite(ID3D12GraphicsCommandList* CL,
                                       FTextureSRVHeap& SRVHeap) {
        if (!NrdReady) return;
        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        NrdCompositePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(NrdCompositeSrvTable));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(OutUAV));
        CL->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
        Transition(CL, Output.Get(), OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
