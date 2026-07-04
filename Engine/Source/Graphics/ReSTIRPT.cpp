#include "Smile/Graphics/ReSTIRPT.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kPTOutFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT kAccumFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constexpr f32 kPi = 3.14159265358979323846f;
        constexpr u32 kReservoirStride = 64; // = sizeof(PTReservoirPacked) no HLSL (4x float4/uint4)

        ComPtr<ID3D12Resource> CreateStructuredBuffer(ID3D12Device* _Device, u32 _Elems, u32 _Stride) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = static_cast<UINT64>(_Elems) * _Stride;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> Buf;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Buf)));
            return Buf;
        }

        ComPtr<ID3D12Resource> CreateUAVTex2D(ID3D12Device* _Device, u32 _W, u32 _H, DXGI_FORMAT _Fmt) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = _W;
            Desc.Height           = _H;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = _Fmt;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> Tex;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Tex)));
            return Tex;
        }
    }

    void FReSTIRPT::Initialize(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "PTInitial.cs_6_6.cso", 12, 3, true);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FReSTIRPT::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(ReSTIRPTConstants);
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

    void FReSTIRPT::SetGIParams(const Vec3& _GridMin, f32 _Spacing, const Vec3& _GridCount,
                                f32 _AtlasTile, f32 _AtlasW, f32 _AtlasH, f32 _MaxRayDist) {
        GIGridMinSpacing = { _GridMin.X, _GridMin.Y, _GridMin.Z, _Spacing };
        GIGridCount      = { _GridCount.X, _GridCount.Y, _GridCount.Z, 0.0f };
        GIAtlasParams    = { _AtlasTile, _AtlasW, _AtlasH, 0.0f };
        GIMaxRayDist     = _MaxRayDist;
    }

    void FReSTIRPT::ReleaseResize(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(PTOutSRV, 1);
        Free(PTOutUAV, 1);
        Free(AccumUAV, 1);
        for (u32 i = 0; i < 2; ++i) {
            Free(ReservoirSRV[i], 1); Free(ReservoirUAV[i], 1);
            Free(TraceTable[i], 12);  Free(TraceUAVTable[i], 3);
            ReservoirBuf[i].Reset();
            ReservoirState[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        PTOutput.Reset(); AccumTex.Reset();
        PTOutputState = AccumState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FReSTIRPT::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                   u32 _Width, u32 _Height, u32 _TlasSlot, u32 _SkyViewSlot,
                                   u32 _InstanceSlot, u32 _IrradSlot, u32 _VertexSlot,
                                   u32 _IndexSlot, u32 _DepthSlot,
                                   u32 _GBufASlot, u32 _GBufBSlot, u32 _GBufCSlot, u32 _VelocitySlot) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        if (_Width == 0 || _Height == 0 || _TlasSlot == kInvalidSlot ||
            _InstanceSlot == kInvalidSlot || _DepthSlot == kInvalidSlot)
            return;

        Width = _Width; Height = _Height;
        PTOutput = CreateUAVTex2D(_Device, Width, Height, kPTOutFormat);
        AccumTex = CreateUAVTex2D(_Device, Width, Height, kAccumFormat);
        const u32 Pixels = Width * Height;
        for (u32 i = 0; i < 2; ++i)
            ReservoirBuf[i] = CreateStructuredBuffer(_Device, Pixels, kReservoirStride);
        PTOutputState = AccumState = D3D12_RESOURCE_STATE_COMMON;
        ReservoirState[0] = ReservoirState[1] = D3D12_RESOURCE_STATE_COMMON;
        NeedsClear  = true;
        FrameParity = 0;
        AccumFrames = 0;
        HavePrevVP  = false;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        PTOutSRV = _SRVHeap.Allocate(1);
        PTOutUAV = _SRVHeap.Allocate(1);
        Srv.Format = kPTOutFormat; Uav.Format = kPTOutFormat;
        _SRVHeap.CreateSRV(_Device, PTOutput.Get(), Srv, PTOutSRV);
        _SRVHeap.CreateUAV(_Device, PTOutput.Get(), Uav, PTOutUAV);

        AccumUAV = _SRVHeap.Allocate(1);
        Uav.Format = kAccumFormat;
        _SRVHeap.CreateUAV(_Device, AccumTex.Get(), Uav, AccumUAV);

        // Views dos reservoirs (StructuredBuffer, stride 64B).
        D3D12_SHADER_RESOURCE_VIEW_DESC RSrv{};
        RSrv.ViewDimension                 = D3D12_SRV_DIMENSION_BUFFER;
        RSrv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RSrv.Format                        = DXGI_FORMAT_UNKNOWN;
        RSrv.Buffer.NumElements            = Pixels;
        RSrv.Buffer.StructureByteStride    = kReservoirStride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC RUav{};
        RUav.ViewDimension                 = D3D12_UAV_DIMENSION_BUFFER;
        RUav.Format                        = DXGI_FORMAT_UNKNOWN;
        RUav.Buffer.NumElements            = Pixels;
        RUav.Buffer.StructureByteStride    = kReservoirStride;
        for (u32 i = 0; i < 2; ++i) {
            ReservoirSRV[i] = _SRVHeap.Allocate(1);
            ReservoirUAV[i] = _SRVHeap.Allocate(1);
            _SRVHeap.CreateSRV(_Device, ReservoirBuf[i].Get(), RSrv, ReservoirSRV[i]);
            _SRVHeap.CreateUAV(_Device, ReservoirBuf[i].Get(), RUav, ReservoirUAV[i]);
        }

        auto CopyTable = [&](u32 Dst, const D3D12_CPU_DESCRIPTOR_HANDLE* Src, UINT Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE D = _SRVHeap.CpuHandle(Dst);
            std::vector<UINT> Ones(Count, 1u);
            _Device->CopyDescriptors(1, &D, &Count, Count, Src, Ones.data(),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        for (u32 p = 0; p < 2; ++p) {
            const u32 prev = 1u - p;
            TraceTable[p] = _SRVHeap.Allocate(12);
            D3D12_CPU_DESCRIPTOR_HANDLE TSrc[12] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(_SkyViewSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_IrradSlot),
                _SRVHeap.CpuHandleStaging(_VertexSlot),
                _SRVHeap.CpuHandleStaging(_IndexSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_GBufASlot),
                _SRVHeap.CpuHandleStaging(_GBufBSlot),
                _SRVHeap.CpuHandleStaging(_GBufCSlot),
                _SRVHeap.CpuHandleStaging(_VelocitySlot),
                _SRVHeap.CpuHandleStaging(ReservoirSRV[prev]),
            };
            CopyTable(TraceTable[p], TSrc, 12);

            TraceUAVTable[p] = _SRVHeap.Allocate(3);
            D3D12_CPU_DESCRIPTOR_HANDLE USrc[3] = {
                _SRVHeap.CpuHandleStaging(PTOutUAV),
                _SRVHeap.CpuHandleStaging(AccumUAV),
                _SRVHeap.CpuHandleStaging(ReservoirUAV[p]),
            };
            CopyTable(TraceUAVTable[p], USrc, 3);
        }

        Ready = true;
    }

    void FReSTIRPT::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj, const Vec3& _CameraPos,
                                   u32 _Width, u32 _Height, const Vec3& _SunDir, f32 _SunIntensity,
                                   const Vec3& _SunColor, u32 _FrameIndex, f32 _SkyIntensity,
                                   f32 _NormalBias, const Mat44& _View, const Mat44& _ViewProjUnjittered) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;

        // Acumulacao progressiva: zera quando a camera mexe (compara a VP NAO-jitterada — o
        // jitter do TAA/FSR muda todo frame e nao pode invalidar a media).
        if (!HavePrevVP || std::memcmp(&PrevVPUnjit, &_ViewProjUnjittered, sizeof(Mat44)) != 0)
            AccumFrames = 0;
        else if (DebugMode != 0)
            ++AccumFrames;
        PrevVPUnjit = _ViewProjUnjittered;
        HavePrevVP  = true;

        const f32 CosSunRadius = std::cos(SunAngularRadiusDeg * kPi / 180.0f);

        CPU.InvViewProj        = _InvViewProj;
        CPU.View               = _View;
        CPU.CameraPos          = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 1.0f };
        CPU.ScreenParams       = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.GridMinSpacing     = GIGridMinSpacing;
        CPU.GridCount          = GIGridCount;
        CPU.AtlasParams        = GIAtlasParams;
        CPU.SunDirIntensity    = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        CPU.SunColorCosRadius  = { _SunColor.X, _SunColor.Y, _SunColor.Z, CosSunRadius };
        CPU.TraceParams        = { (f32)_FrameIndex, GIMaxRayDist, _SkyIntensity, _NormalBias };
        CPU.PathParams         = { MaxBounces, RrStartBounce, FireflyMax, AlbedoLOD };
        CPU.ReuseParams        = { 20.0f, 0.01f, 1.0f, 0.0f };
        CPU.SpatialParams      = { 16.0f, 3.0f, 1.0f, 0.9f };
        CPU.ShiftParams        = { 0.02f, 0.2f, 0.0f, 0.0f };
        CPU.NrdHitDistParams   = { 3.0f, 0.1f, 20.0f, 0.0f };
        CPU.DebugParams        = { (f32)DebugMode, (f32)AccumFrames, 0.0f, 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReSTIRPTConstants),
                    &CPU, sizeof(ReSTIRPTConstants));
    }

    void FReSTIRPT::Transition(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _Res,
                               D3D12_RESOURCE_STATES& _State, D3D12_RESOURCE_STATES _After) {
        if (_State == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _Res;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = _State;
        B.Transition.StateAfter  = _After;
        _CL->ResourceBarrier(1, &B);
        _State = _After;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FReSTIRPT::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(ReSTIRPTConstants);
    }

    void FReSTIRPT::RecordTrace(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        const u32 p = FrameParity, prev = 1u - FrameParity;

        if (NeedsClear) {
            const UINT ZeroU[4] = { 0, 0, 0, 0 };
            const float ZeroF[4] = { 0, 0, 0, 0 };
            Transition(_CL, AccumTex.Get(), AccumState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(AccumUAV),
                                               _SRVHeap.CpuHandleStaging(AccumUAV),
                                               AccumTex.Get(), ZeroF, 0, nullptr);
            // Zera ambos os reservoirs (M=0 => o reuso temporal ignora historico invalido).
            for (u32 i = 0; i < 2; ++i) {
                Transition(_CL, ReservoirBuf[i].Get(), ReservoirState[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                _CL->ClearUnorderedAccessViewUint(_SRVHeap.GpuHandle(ReservoirUAV[i]),
                                                  _SRVHeap.CpuHandleStaging(ReservoirUAV[i]),
                                                  ReservoirBuf[i].Get(), ZeroU, 0, nullptr);
            }
            NeedsClear = false;
        }

        Transition(_CL, PTOutput.Get(), PTOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, AccumTex.Get(), AccumState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, ReservoirBuf[prev].Get(), ReservoirState[prev],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ReservoirBuf[p].Get(), ReservoirState[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[p]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable[p]));
        _CL->Dispatch(GX, GY, 1);

        // O deferred (pixel shader) le PTOut como passthrough (ReflectionParams.w == 3).
        Transition(_CL, PTOutput.Get(), PTOutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        FrameParity ^= 1u;
    }
}
