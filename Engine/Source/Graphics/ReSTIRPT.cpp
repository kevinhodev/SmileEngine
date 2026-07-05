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
        TracePSO.Initialize(_Device, "PTInitial.cs_6_6.cso", 12, 5, true);
        SpatialPSO.Initialize(_Device, "PTSpatial.cs_6_6.cso", 12, 2, true);
        NrdPackPSO.Initialize(_Device, "PTNrdPack.cs_6_6.cso", 6, 5, false);
        CompositePSO.Initialize(_Device, "PTComposite.cs_6_6.cso", 4, 3, false);
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
        Free(PTLitSRV, 1);
        Free(PTLitUAV, 1);
        Free(PTGiDiffSRV, 1);
        Free(PTGiDiffUAV, 1);
        Free(PTFinalSRV, 1);
        Free(PTFinalUAV, 1);
        Free(SpatialUAVTable, 2);
        Free(NrdPackSrvTable, 6);
        Free(NrdPackUavTable, 5);
        Free(CompositeSrvTable, 4);
        Free(CompositeUavTable, 3);
        for (u32 i = 0; i < 2; ++i) {
            Free(ReservoirSRV[i], 1); Free(ReservoirUAV[i], 1);
            Free(TraceTable[i], 12);  Free(TraceUAVTable[i], 5);
            Free(SpatialTable[i], 12);
            ReservoirBuf[i].Reset();
            ReservoirState[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        PTOutput.Reset(); AccumTex.Reset(); PTLitTex.Reset();
        PTGiDiffTex.Reset(); PTFinalTex.Reset();
        PTOutputState = AccumState = PTLitState = D3D12_RESOURCE_STATE_COMMON;
        PTGiDiffState = PTFinalState = D3D12_RESOURCE_STATE_COMMON;
        NrdOutDiffRes = NrdOutSpecRes = nullptr;
        NrdSetup = false; UseNrd = false;
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
        GBufASlot = _GBufASlot; GBufBSlot = _GBufBSlot;
        DepthSlot = _DepthSlot; VelocitySlot = _VelocitySlot;
        PTOutput    = CreateUAVTex2D(_Device, Width, Height, kPTOutFormat);
        AccumTex    = CreateUAVTex2D(_Device, Width, Height, kAccumFormat);
        PTLitTex    = CreateUAVTex2D(_Device, Width, Height, kPTOutFormat);
        PTGiDiffTex = CreateUAVTex2D(_Device, Width, Height, kPTOutFormat);
        PTFinalTex  = CreateUAVTex2D(_Device, Width, Height, kPTOutFormat);
        const u32 Pixels = Width * Height;
        for (u32 i = 0; i < 2; ++i)
            ReservoirBuf[i] = CreateStructuredBuffer(_Device, Pixels, kReservoirStride);
        PTOutputState = AccumState = PTLitState = D3D12_RESOURCE_STATE_COMMON;
        PTGiDiffState = PTFinalState = D3D12_RESOURCE_STATE_COMMON;
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

        PTLitSRV = _SRVHeap.Allocate(1);
        PTLitUAV = _SRVHeap.Allocate(1);
        Srv.Format = kPTOutFormat; Uav.Format = kPTOutFormat;
        _SRVHeap.CreateSRV(_Device, PTLitTex.Get(), Srv, PTLitSRV);
        _SRVHeap.CreateUAV(_Device, PTLitTex.Get(), Uav, PTLitUAV);

        PTGiDiffSRV = _SRVHeap.Allocate(1);
        PTGiDiffUAV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, PTGiDiffTex.Get(), Srv, PTGiDiffSRV);
        _SRVHeap.CreateUAV(_Device, PTGiDiffTex.Get(), Uav, PTGiDiffUAV);

        PTFinalSRV = _SRVHeap.Allocate(1);
        PTFinalUAV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, PTFinalTex.Get(), Srv, PTFinalSRV);
        _SRVHeap.CreateUAV(_Device, PTFinalTex.Get(), Uav, PTFinalUAV);

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

            TraceUAVTable[p] = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE USrc[5] = {
                _SRVHeap.CpuHandleStaging(PTOutUAV),
                _SRVHeap.CpuHandleStaging(AccumUAV),
                _SRVHeap.CpuHandleStaging(ReservoirUAV[p]),
                _SRVHeap.CpuHandleStaging(PTLitUAV),
                _SRVHeap.CpuHandleStaging(PTGiDiffUAV),
            };
            CopyTable(TraceUAVTable[p], USrc, 5);

            // Pass B (F3): mesmo prefixo de cena/G-buffer do trace, mas le o reservoir do
            // FRAME ATUAL ([p], escrito pelo Pass A) e o PTLit; sem velocity.
            SpatialTable[p] = _SRVHeap.Allocate(12);
            D3D12_CPU_DESCRIPTOR_HANDLE SSrc[12] = {
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
                _SRVHeap.CpuHandleStaging(ReservoirSRV[p]),
                _SRVHeap.CpuHandleStaging(PTLitSRV),
            };
            CopyTable(SpatialTable[p], SSrc, 12);
        }

        SpatialUAVTable = _SRVHeap.Allocate(2);
        D3D12_CPU_DESCRIPTOR_HANDLE SU[2] = {
            _SRVHeap.CpuHandleStaging(PTOutUAV),
            _SRVHeap.CpuHandleStaging(PTGiDiffUAV),
        };
        CopyTable(SpatialUAVTable, SU, 2);

        Ready = true;
    }

    void FReSTIRPT::SetupNrd(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                             ID3D12Resource* _ViewZ, ID3D12Resource* _NormalRough, ID3D12Resource* _Mv,
                             ID3D12Resource* _DiffIn, ID3D12Resource* _SpecIn,
                             ID3D12Resource* _OutDiff, ID3D12Resource* _OutSpec) {
        if (!Ready || !_ViewZ || !_OutDiff || !_OutSpec) return;

        // UAVs do pack sobre as IN do denoiser (formatos = os das IO do FNrdDenoiser).
        NrdPackUavTable = _SRVHeap.Allocate(5);
        auto MakeUav = [&](ID3D12Resource* R, DXGI_FORMAT F, u32 Slot) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; u.Format = F;
            _SRVHeap.CreateUAV(_Device, R, u, Slot);
        };
        MakeUav(_ViewZ,       DXGI_FORMAT_R32_FLOAT,          NrdPackUavTable + 0);
        MakeUav(_NormalRough, DXGI_FORMAT_R10G10B10A2_UNORM,  NrdPackUavTable + 1);
        MakeUav(_Mv,          DXGI_FORMAT_R16G16_FLOAT,       NrdPackUavTable + 2);
        MakeUav(_DiffIn,      DXGI_FORMAT_R16G16B16A16_FLOAT, NrdPackUavTable + 3);
        MakeUav(_SpecIn,      DXGI_FORMAT_R16G16B16A16_FLOAT, NrdPackUavTable + 4);

        NrdPackSrvTable = _SRVHeap.Allocate(6);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(NrdPackSrvTable);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[6] = {
                _SRVHeap.CpuHandleStaging(PTOutSRV),
                _SRVHeap.CpuHandleStaging(PTGiDiffSRV),
                _SRVHeap.CpuHandleStaging(GBufASlot),
                _SRVHeap.CpuHandleStaging(GBufBSlot),
                _SRVHeap.CpuHandleStaging(DepthSlot),
                _SRVHeap.CpuHandleStaging(VelocitySlot),
            };
            UINT N = 6; UINT Ones[6] = { 1, 1, 1, 1, 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &N, 6, Src, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        // Composite: le OUT_DIFF/OUT_SPEC como UAV (nao transiciona recursos do driver do NRD).
        CompositeUavTable = _SRVHeap.Allocate(3);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(CompositeUavTable);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[1] = { _SRVHeap.CpuHandleStaging(PTFinalUAV) };
            UINT N = 1; UINT One = 1;
            _Device->CopyDescriptors(1, &Dst, &N, 1, Src, &One, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            _SRVHeap.CreateUAV(_Device, _OutDiff, u, CompositeUavTable + 1);
            _SRVHeap.CreateUAV(_Device, _OutSpec, u, CompositeUavTable + 2);
        }

        CompositeSrvTable = _SRVHeap.Allocate(4);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(CompositeSrvTable);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[4] = {
                _SRVHeap.CpuHandleStaging(PTLitSRV),
                _SRVHeap.CpuHandleStaging(GBufASlot),
                _SRVHeap.CpuHandleStaging(GBufBSlot),
                _SRVHeap.CpuHandleStaging(DepthSlot),
            };
            UINT N = 4; UINT Ones[4] = { 1, 1, 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &N, 4, Src, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        NrdOutDiffRes = _OutDiff;
        NrdOutSpecRes = _OutSpec;
        NrdSetup = true;
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
        // SpatialParams.z avisa o Pass A se o Pass B VAI rodar (PTOut = so GI do canonico).
        // Modos de debug do Pass A (1-3) desligam o spatial; o mapa espacial (4) mantem.
        // ReuseParams.w = NRD ativo (F4, ja gated por debug no SetUseNrd): os passes deixam o
        // GI separado p/ o pack/composite.
        const bool SpatialRuns = SpatialOn && (DebugMode == 0 || DebugMode == 4);
        CPU.ReuseParams        = { 20.0f, 0.01f, 1.0f, UseNrd ? 1.0f : 0.0f };
        CPU.SpatialParams      = { SpatialSigma, SpatialCount, SpatialRuns ? 1.0f : 0.0f, NormalReject };
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
        Transition(_CL, PTLitTex.Get(), PTLitState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, PTGiDiffTex.Get(), PTGiDiffState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, ReservoirBuf[prev].Get(), ReservoirState[prev],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ReservoirBuf[p].Get(), ReservoirState[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[p]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable[p]));
        _CL->Dispatch(GX, GY, 1);

        // Pass B (F3): reuso espacial pairwise MIS. Le o reservoir atual + PTLit como SRV e
        // reescreve PTOut in-place (UAV barrier entre os passes). Mesma condicao do CB
        // (SpatialParams.z) — Pass A escreveu so o GI do canonico em PTOut nesse caso.
        if (SpatialOn && (DebugMode == 0 || DebugMode == 4)) {
            Transition(_CL, ReservoirBuf[p].Get(), ReservoirState[p],
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, PTLitTex.Get(), PTLitState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_RESOURCE_BARRIER Uav[2]{};
            Uav[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            Uav[0].UAV.pResource = PTOutput.Get();
            Uav[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            Uav[1].UAV.pResource = PTGiDiffTex.Get();
            _CL->ResourceBarrier(2, Uav);

            SpatialPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpatialTable[p]));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(SpatialUAVTable));
            _CL->Dispatch(GX, GY, 1);
        }

        // Sem NRD: o deferred (pixel shader) le PTOut direto (ReflectionParams.w == 3). Com NRD,
        // PTOut/PTGiDiff viram SRV do pack (compute) — o PTFinal e quem vai p/ o deferred.
        if (UseNrd) {
            Transition(_CL, PTOutput.Get(), PTOutputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, PTGiDiffTex.Get(), PTGiDiffState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, PTLitTex.Get(), PTLitState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            Transition(_CL, PTOutput.Get(), PTOutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        FrameParity ^= 1u;
    }

    void FReSTIRPT::RecordNrdPack(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || !UseNrd || NrdPackSrvTable == kInvalidSlot) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        // RecordTrace ja deixou PTOut/PTGiDiff em NON_PIXEL; as IN do NRD ja estao em UAV
        // (Nrd.TransitionInputsToWrite, chamado pelo Renderer antes).
        NrdPackPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(NrdPackSrvTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(NrdPackUavTable));
        _CL->Dispatch(GX, GY, 1);
    }

    void FReSTIRPT::RecordComposite(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || !UseNrd || CompositeUavTable == kInvalidSlot) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;

        // OUT_DIFF/OUT_SPEC ficam no estado que o driver do NRD deixou (UAV) — so garantimos a
        // visibilidade das escritas com UAV barrier (licao do P4: transicionar recursos do NRD
        // dessincroniza o tracking interno dele).
        D3D12_RESOURCE_BARRIER Uav[2]{};
        Uav[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Uav[0].UAV.pResource = NrdOutDiffRes;
        Uav[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Uav[1].UAV.pResource = NrdOutSpecRes;
        _CL->ResourceBarrier(2, Uav);
        Transition(_CL, PTFinalTex.Get(), PTFinalState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        CompositePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(CompositeSrvTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(CompositeUavTable));
        _CL->Dispatch(GX, GY, 1);

        // O deferred le PTFinal como passthrough (ReflectionParams.w == 3).
        Transition(_CL, PTFinalTex.Get(), PTFinalState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
