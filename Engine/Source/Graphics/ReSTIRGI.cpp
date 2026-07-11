#include "Smile/Graphics/ReSTIRGI.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kGIFormat  = DXGI_FORMAT_R16G16B16A16_FLOAT; 
        constexpr DXGI_FORMAT kResFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; 

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

    void FReSTIRGI::Initialize(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "ReSTIRGITrace.cs_6_6.cso", 14, 5, true); // t13 = luzes (F5)
        // t7..t9 = Instances/Vertices/Indices (alpha-test dos visibility rays, M6);
        // heap-indexed pelo ResourceDescriptorHeap do AlphaTestPass.
        SpatialPSO.Initialize(_Device, "ReSTIRGISpatial.cs_6_6.cso", 10, 1, true);
        NrdPackPSO.Initialize(_Device, "ReSTIRNrdPack.cs_6_6.cso", 4, 4, false);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FReSTIRGI::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(ReSTIRGIConstants);
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

    void FReSTIRGI::SetGIParams(const Vec3& _GridMin, f32 _Spacing, const Vec3& _GridCount,
                                f32 _AtlasTile, f32 _AtlasW, f32 _AtlasH, f32 _MaxRayDist) {
        GIGridMinSpacing = { _GridMin.X, _GridMin.Y, _GridMin.Z, _Spacing };
        GIGridCount      = { _GridCount.X, _GridCount.Y, _GridCount.Z, 0.0f };
        GIAtlasParams    = { _AtlasTile, _AtlasW, _AtlasH, 0.0f };
        GIMaxRayDist     = _MaxRayDist;
    }

    void FReSTIRGI::ReleaseResize(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(GITexSRV, 1);
        Free(GITexUAV, 1);
        for (u32 i = 0; i < 2; ++i) {
            Free(ResASRV[i], 1); Free(ResBSRV[i], 1); Free(ResCSRV[i], 1); Free(ResDSRV[i], 1);
            Free(ResAUAV[i], 1); Free(ResBUAV[i], 1); Free(ResCUAV[i], 1); Free(ResDUAV[i], 1);
            Free(TraceTable[i], 14); Free(TraceUAVTable[i], 5); Free(SpatialTable[i], 10);
            ResA[i].Reset(); ResB[i].Reset(); ResC[i].Reset(); ResD[i].Reset();
            ResAState[i] = ResBState[i] = ResCState[i] = ResDState[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        Free(PackSrvTable, 4); Free(PackUavTable, 4); Free(NrdOutSRV, 1);
        GITexture.Reset();
        GITextureState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FReSTIRGI::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                   u32 _Width, u32 _Height, u32 _TlasSlot, u32 _SkyViewSlot,
                                   u32 _InstanceSlot, u32 _IrradSlot, u32 _VertexSlot,
                                   u32 _IndexSlot, u32 _DepthSlot, u32 _GBufferSlot, u32 _VelocitySlot) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        if (_Width == 0 || _Height == 0 || _TlasSlot == kInvalidSlot ||
            _InstanceSlot == kInvalidSlot || _DepthSlot == kInvalidSlot)
            return;

        Width = _Width; Height = _Height;
        DepthSlot = _DepthSlot; GBufferSlot = _GBufferSlot; VelocitySlot = _VelocitySlot;
        GITexture = CreateUAVTex2D(_Device, Width, Height, kGIFormat);
        for (u32 i = 0; i < 2; ++i) {
            ResA[i] = CreateUAVTex2D(_Device, Width, Height, kResFormat);
            ResB[i] = CreateUAVTex2D(_Device, Width, Height, kResFormat);
            ResC[i] = CreateUAVTex2D(_Device, Width, Height, kGIFormat);
            ResD[i] = CreateUAVTex2D(_Device, Width, Height, kGIFormat);
        }
        GITextureState = D3D12_RESOURCE_STATE_COMMON;
        FrameParity = 0; NeedsClear = true;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Texture2D.MipLevels     = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        auto MakeSrvUav = [&](ID3D12Resource* Res, DXGI_FORMAT Fmt, u32& SrvSlot, u32& UavSlot) {
            SrvSlot = _SRVHeap.Allocate(1);
            UavSlot = _SRVHeap.Allocate(1);
            Srv.Format = Fmt; Uav.Format = Fmt;
            _SRVHeap.CreateSRV(_Device, Res, Srv, SrvSlot);
            _SRVHeap.CreateUAV(_Device, Res, Uav, UavSlot);
        };
        MakeSrvUav(GITexture.Get(), kGIFormat, GITexSRV, GITexUAV);
        for (u32 i = 0; i < 2; ++i) {
            MakeSrvUav(ResA[i].Get(), kResFormat, ResASRV[i], ResAUAV[i]);
            MakeSrvUav(ResB[i].Get(), kResFormat, ResBSRV[i], ResBUAV[i]);
            MakeSrvUav(ResC[i].Get(), kGIFormat,  ResCSRV[i], ResCUAV[i]);
            MakeSrvUav(ResD[i].Get(), kGIFormat,  ResDSRV[i], ResDUAV[i]);
        }

        auto CopyTable = [&](u32 Dst, const D3D12_CPU_DESCRIPTOR_HANDLE* Src, UINT Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE D = _SRVHeap.CpuHandle(Dst);
            std::vector<UINT> Ones(Count, 1u);
            _Device->CopyDescriptors(1, &D, &Count, Count, Src, Ones.data(),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        for (u32 p = 0; p < 2; ++p) {
            const u32 prev = 1u - p;
            // t0..t12 fixos + t13 = luzes puntuais (F5; copiado por frame).
            TraceTable[p] = _SRVHeap.Allocate(14);
            D3D12_CPU_DESCRIPTOR_HANDLE TSrc[13] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(_SkyViewSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_IrradSlot),
                _SRVHeap.CpuHandleStaging(_VertexSlot),
                _SRVHeap.CpuHandleStaging(_IndexSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_VelocitySlot),
                _SRVHeap.CpuHandleStaging(ResASRV[prev]),
                _SRVHeap.CpuHandleStaging(ResBSRV[prev]),
                _SRVHeap.CpuHandleStaging(ResCSRV[prev]),
                _SRVHeap.CpuHandleStaging(ResDSRV[prev]),
            };
            CopyTable(TraceTable[p], TSrc, 13);

            TraceUAVTable[p] = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE USrc[5] = {
                _SRVHeap.CpuHandleStaging(GITexUAV),
                _SRVHeap.CpuHandleStaging(ResAUAV[p]),
                _SRVHeap.CpuHandleStaging(ResBUAV[p]),
                _SRVHeap.CpuHandleStaging(ResCUAV[p]),
                _SRVHeap.CpuHandleStaging(ResDUAV[p]),
            };
            CopyTable(TraceUAVTable[p], USrc, 5);

            // t7..t9 = geometria p/ o alpha-test dos visibility rays (M6).
            SpatialTable[p] = _SRVHeap.Allocate(10);
            D3D12_CPU_DESCRIPTOR_HANDLE SSrc[10] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(ResASRV[p]),
                _SRVHeap.CpuHandleStaging(ResBSRV[p]),
                _SRVHeap.CpuHandleStaging(ResCSRV[p]),
                _SRVHeap.CpuHandleStaging(ResDSRV[p]),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_VertexSlot),
                _SRVHeap.CpuHandleStaging(_IndexSlot),
            };
            CopyTable(SpatialTable[p], SSrc, 10);
        }

        Ready = true;
    }

    void FReSTIRGI::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                         u32 _StagingSlot) {
        if (!Ready) return;
        // SO a tabela da paridade DESTE frame: a outra foi bindada pelo frame anterior, que com
        // 2 frames em voo ainda pode estar lendo o heap shader-visible — reescrever descriptor
        // em uso pela GPU e UB (descriptor versioning). A tabela corrente foi usada pela ultima
        // vez ha >=2 frames, ja coberta pelo fence do FrameSlot.
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[FrameParity] + 13);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FReSTIRGI::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj, const Vec3& _CameraPos,
                                   u32 _Width, u32 _Height, const Vec3& _SunDir, f32 _SunIntensity,
                                   const Vec3& _SunColor, u32 _FrameIndex, f32 _SkyIntensity,
                                   f32 _ShadowRayBias, const Mat44& _View, const Vec2& _JitterDeltaUv,
                                   u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.InvViewProj     = _InvViewProj;
        CPU.View            = _View;
        CPU.CameraPos       = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 1.0f };
        CPU.ScreenParams    = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.GridMinSpacing  = GIGridMinSpacing;
        CPU.GridCount       = GIGridCount;
        CPU.AtlasParams     = GIAtlasParams;
        CPU.SunDirIntensity = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        CPU.SunColor        = { _SunColor.X, _SunColor.Y, _SunColor.Z, 0.0f };
        CPU.TraceParams     = { (f32)_FrameIndex, GIMaxRayDist, _SkyIntensity, _ShadowRayBias };
        CPU.ShadeParams     = { RealHit ? 1.0f : 0.0f, AlbedoLOD, FireflyMax, ValidateInterval };
        CPU.ReuseParams     = { MCap, PosRejectScale, Visibility ? 1.0f : 0.0f, Temporal ? 1.0f : 0.0f };
        CPU.SpatialParams   = { SpatialRadius, SpatialCount, Spatial ? 1.0f : 0.0f, NormalReject };
        CPU.JitterParams    = { _JitterDeltaUv.X, _JitterDeltaUv.Y,
                                static_cast<f32>(_PunctualLightCount), 0.0f }; // z = luzes (F5)
        CPU.NrdHitDistParams = { 3.0f, 0.1f, 20.0f, 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReSTIRGIConstants),
                    &CPU, sizeof(ReSTIRGIConstants));
    }

    void FReSTIRGI::Transition(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _Res,
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

    D3D12_GPU_VIRTUAL_ADDRESS FReSTIRGI::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(ReSTIRGIConstants);
    }

    void FReSTIRGI::RecordTrace(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        const u32 p = FrameParity, prev = 1u - FrameParity;
        CurrParity = p;

        if (NeedsClear) {
            // SO o conjunto prev: o trace escreve TODO pixel do curr incondicionalmente, e
            // clear+dispatch no mesmo recurso sem UAV barrier e WAW nao ordenado (o clear
            // poderia terminar DEPOIS do dispatch). A transition prev->SRV logo abaixo ja
            // ordena o clear contra a leitura.
            const float Zero[4] = { 0, 0, 0, 0 };
            auto ClearRes = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES& St, u32 UavSlot) {
                Transition(_CL, R, St, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(UavSlot),
                                                   _SRVHeap.CpuHandleStaging(UavSlot), R, Zero, 0, nullptr);
            };
            ClearRes(ResA[prev].Get(), ResAState[prev], ResAUAV[prev]);
            ClearRes(ResB[prev].Get(), ResBState[prev], ResBUAV[prev]);
            ClearRes(ResC[prev].Get(), ResCState[prev], ResCUAV[prev]);
            ClearRes(ResD[prev].Get(), ResDState[prev], ResDUAV[prev]);
            NeedsClear = false;
        }

        Transition(_CL, ResA[prev].Get(), ResAState[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ResB[prev].Get(), ResBState[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ResC[prev].Get(), ResCState[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ResD[prev].Get(), ResDState[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ResA[p].Get(), ResAState[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, ResB[p].Get(), ResBState[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, ResC[p].Get(), ResCState[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, ResD[p].Get(), ResDState[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, GITexture.Get(), GITextureState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[p]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable[p]));
        _CL->Dispatch(GX, GY, 1);

        if (Spatial) {
            Transition(_CL, ResA[p].Get(), ResAState[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, ResB[p].Get(), ResBState[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, ResC[p].Get(), ResCState[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, ResD[p].Get(), ResDState[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_RESOURCE_BARRIER UB{};
            UB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UB.UAV.pResource = GITexture.Get();
            _CL->ResourceBarrier(1, &UB);

            SpatialPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpatialTable[p]));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(GITexUAV));
            _CL->Dispatch(GX, GY, 1);
        }

        Transition(_CL, GITexture.Get(), GITextureState,
                   UseNrd ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                          : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        FrameParity ^= 1u;
    }

    void FReSTIRGI::SetupNrdPack(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                 ID3D12Resource* _ViewZ, ID3D12Resource* _NormalRough,
                                 ID3D12Resource* _Mv, ID3D12Resource* _DiffRadHit, ID3D12Resource* _Out) {
        if (!Ready || !_ViewZ || !_Out) return;

        PackUavTable = _SRVHeap.Allocate(4);
        auto MakeUav = [&](ID3D12Resource* R, DXGI_FORMAT F, u32 Slot) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; u.Format = F;
            _SRVHeap.CreateUAV(_Device, R, u, Slot);
        };
        MakeUav(_ViewZ,       DXGI_FORMAT_R32_FLOAT,          PackUavTable + 0);
        MakeUav(_NormalRough, DXGI_FORMAT_R10G10B10A2_UNORM,  PackUavTable + 1);
        MakeUav(_Mv,          DXGI_FORMAT_R16G16_FLOAT,       PackUavTable + 2);
        MakeUav(_DiffRadHit,  DXGI_FORMAT_R16G16B16A16_FLOAT, PackUavTable + 3);

        NrdOutSRV = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; s.Texture2D.MipLevels = 1;
        _SRVHeap.CreateSRV(_Device, _Out, s, NrdOutSRV);

        PackSrvTable = _SRVHeap.Allocate(4);
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(PackSrvTable);
        D3D12_CPU_DESCRIPTOR_HANDLE Src[4] = {
            _SRVHeap.CpuHandleStaging(GITexSRV),
            _SRVHeap.CpuHandleStaging(GBufferSlot),
            _SRVHeap.CpuHandleStaging(DepthSlot),
            _SRVHeap.CpuHandleStaging(VelocitySlot),
        };
        UINT N = 4; UINT Ones[4] = { 1, 1, 1, 1 };
        _Device->CopyDescriptors(1, &Dst, &N, 4, Src, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FReSTIRGI::RecordNrdPack(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || PackSrvTable == kInvalidSlot) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        NrdPackPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(PackSrvTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(PackUavTable));
        _CL->Dispatch(GX, GY, 1);
    }
}
