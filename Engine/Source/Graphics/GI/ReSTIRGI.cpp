#include "Smile/Graphics/GI/ReSTIRGI.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Debug/GpuProfiler.h"
#include "Smile/Graphics/RayTracing/RTMasks.h"
#include "Smile/Graphics/Debug/ShaderTimer.h"
#include "Smile/Graphics/Debug/DebugTargets.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>
#include <exception>
#include <vector>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kGIFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // x2 usa FP32; os demais campos são empacotados em quatro uints.
        constexpr DXGI_FORMAT kRes0Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constexpr DXGI_FORMAT kRes1Format = DXGI_FORMAT_R32G32B32A32_UINT;

        ComPtr<ID3D12Resource> CreateUAVTex2D(ID3D12Device* _Device, u32 _W, u32 _H,
                                              DXGI_FORMAT _Fmt, const char* _Label) {
            return GpuResources::CreateTex2D(
                _Device, _W, _H, _Fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI, nullptr, 1, 1, _Label);
        }

    }

    void FReSTIRGI::Initialize(ID3D12Device* _Device) {
        CreatePipelines(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FReSTIRGI::CreateConstantBuffer(ID3D12Device* _Device) {
        static_assert(sizeof(ReSTIRGIConstants) % 256 == 0,
                      "o CBAddr indexa por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(ReSTIRGIConstants), FCommandQueue::kFramesInFlight);
        CB       = Upload.Resource;
        MappedCB = Upload.Mapped;
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
        // Em full-res, os slots de trace são aliases dos slots públicos.
        if (TraceGITexture) {
            Free(TraceGITexSRV, 1);
            Free(TraceGITexUAV, 1);
        } else {
            TraceGITexSRV = TraceGITexUAV = kInvalidSlot;
        }
        Free(UpsampleTable, 5);
        Free(ResolvedGITexUAV, 1);
        Free(GITexSRV, 1);
        Free(GITexUAV, 1);
        for (u32 i = 0; i < 2; ++i) {
            Free(Res0SRV[i], 1); Free(Res1SRV[i], 1);
            Free(Res0UAV[i], 1); Free(Res1UAV[i], 1);
            Free(TraceTable[i], 14); Free(TraceUAVTable[i], 3); Free(SpatialTable[i], 10);
            Res0[i].Reset(); Res1[i].Reset();
            Res0State[i] = Res1State[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        Free(PackSrvTable, 4); Free(PackUavTable, 4); Free(NrdOutSRV, 1);
        Free(SourceDebugSRV, 1);
        Free(SourceDebugUAV, 1);
        SourceDebugTex.Reset();
        SourceDebugState = D3D12_RESOURCE_STATE_COMMON;
        TraceGITexture.Reset();
        TraceGITextureState = D3D12_RESOURCE_STATE_COMMON;
        ResolvedGITexture.Reset();
        ResolvedGITextureState = D3D12_RESOURCE_STATE_COMMON;
        GITexture.Reset();
        GITextureState = D3D12_RESOURCE_STATE_COMMON;
        FullWidth = FullHeight = Width = Height = 0;
        ReconstructionHistoryValid = false;
        Ready = false;
    }

    void FReSTIRGI::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                   u32 _Width, u32 _Height, u32 _TlasSlot, u32 _SkyViewSlot,
                                   u32 _InstanceSlot, const FGIFallbackBindings& _Fallback,
                                   u32 _DepthSlot, u32 _GBufferSlot, u32 _VelocitySlot) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        const u32 _IrradSlot     = _Fallback.IrradianceAtlasSRV;
        const u32 _DistSlot      = _Fallback.DistanceAtlasSRV;
        const u32 _ProbeDataSlot = _Fallback.ProbeDataSRV;
        // Todos os slots entram em CopyDescriptors; fallback ausente usa slots neutros válidos.
        if (_Width == 0 || _Height == 0 || _TlasSlot == kInvalidSlot ||
            _SkyViewSlot == kInvalidSlot || _InstanceSlot == kInvalidSlot ||
            _IrradSlot == kInvalidSlot || _DistSlot == kInvalidSlot ||
            _ProbeDataSlot == kInvalidSlot || _DepthSlot == kInvalidSlot ||
            _GBufferSlot == kInvalidSlot || _VelocitySlot == kInvalidSlot)
            return;

        FullWidth = _Width; FullHeight = _Height;
        Width  = HalfRes ? (_Width + 1u) / 2u : _Width;
        Height = HalfRes ? (_Height + 1u) / 2u : _Height;
        DepthSlot = _DepthSlot; GBufferSlot = _GBufferSlot; VelocitySlot = _VelocitySlot;
        GITexture = CreateUAVTex2D(_Device, FullWidth, FullHeight, kGIFormat,
                                   "ReSTIR GI · saida full-res");
        if (HalfRes) {
            TraceGITexture = CreateUAVTex2D(_Device, Width, Height, kGIFormat,
                                            "ReSTIR GI · trace half-res");
            ResolvedGITexture = CreateUAVTex2D(_Device, FullWidth, FullHeight, kGIFormat,
                                               "ReSTIR GI · reconstrução temporal");
        }
        for (u32 i = 0; i < 2; ++i) {
            Res0[i] = CreateUAVTex2D(_Device, Width, Height, kRes0Format, "ReSTIR GI · reservoir");
            Res1[i] = CreateUAVTex2D(_Device, Width, Height, kRes1Format, "ReSTIR GI · reservoir");
        }
        // Falsa-cor de diagnóstico; não armazena radiância.
        SourceDebugTex = CreateUAVTex2D(_Device, Width, Height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                        "ReSTIR GI · fonte do candidato");
        GITextureState = D3D12_RESOURCE_STATE_COMMON;
        TraceGITextureState = D3D12_RESOURCE_STATE_COMMON;
        ResolvedGITextureState = D3D12_RESOURCE_STATE_COMMON;
        SourceDebugState = D3D12_RESOURCE_STATE_COMMON;
        FrameParity = 0; NeedsClear = true; ReconstructionHistoryValid = false;

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
        if (HalfRes) {
            MakeSrvUav(TraceGITexture.Get(), kGIFormat, TraceGITexSRV, TraceGITexUAV);
            ResolvedGITexUAV = _SRVHeap.Allocate(1);
            Uav.Format = kGIFormat;
            _SRVHeap.CreateUAV(_Device, ResolvedGITexture.Get(), Uav, ResolvedGITexUAV);
        } else {
            TraceGITexSRV = GITexSRV;
            TraceGITexUAV = GITexUAV;
        }
        MakeSrvUav(SourceDebugTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                   SourceDebugSRV, SourceDebugUAV);
        for (u32 i = 0; i < 2; ++i) {
            MakeSrvUav(Res0[i].Get(), kRes0Format, Res0SRV[i], Res0UAV[i]);
            MakeSrvUav(Res1[i].Get(), kRes1Format, Res1SRV[i], Res1UAV[i]);
        }

        auto CopyTable = [&](u32 Dst, const D3D12_CPU_DESCRIPTOR_HANDLE* Src, UINT Count) {
            D3D12_CPU_DESCRIPTOR_HANDLE D = _SRVHeap.CpuHandle(Dst);
            std::vector<UINT> Ones(Count, 1u);
            _Device->CopyDescriptors(1, &D, &Count, Count, Src, Ones.data(),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        };

        for (u32 p = 0; p < 2; ++p) {
            const u32 prev = 1u - p;

            TraceTable[p] = _SRVHeap.Allocate(14);
            D3D12_CPU_DESCRIPTOR_HANDLE TSrc[13] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(_SkyViewSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_IrradSlot),
                _SRVHeap.CpuHandleStaging(_DistSlot),
                _SRVHeap.CpuHandleStaging(_ProbeDataSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_VelocitySlot),
                _SRVHeap.CpuHandleStaging(Res0SRV[prev]),
                _SRVHeap.CpuHandleStaging(Res1SRV[prev]),
                // t11/t12 preservam o offset fixo t13 das luzes.
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
            };
            CopyTable(TraceTable[p], TSrc, 13);

            TraceUAVTable[p] = _SRVHeap.Allocate(3);
            D3D12_CPU_DESCRIPTOR_HANDLE USrc[3] = {
                _SRVHeap.CpuHandleStaging(TraceGITexUAV),
                _SRVHeap.CpuHandleStaging(Res0UAV[p]),
                _SRVHeap.CpuHandleStaging(Res1UAV[p]),
            };
            CopyTable(TraceUAVTable[p], USrc, 3);

            SpatialTable[p] = _SRVHeap.Allocate(10);
            D3D12_CPU_DESCRIPTOR_HANDLE SSrc[10] = {
                _SRVHeap.CpuHandleStaging(_TlasSlot),
                _SRVHeap.CpuHandleStaging(Res0SRV[p]),
                _SRVHeap.CpuHandleStaging(Res1SRV[p]),
                _SRVHeap.CpuHandleStaging(_InstanceSlot), // t3/t4: filler (ver acima)
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
                _SRVHeap.CpuHandleStaging(_InstanceSlot),
            };
            CopyTable(SpatialTable[p], SSrc, 10);
        }

        if (HalfRes) {
            UpsampleTable = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE UpsampleSrc[5] = {
                _SRVHeap.CpuHandleStaging(TraceGITexSRV),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_VelocitySlot),
                _SRVHeap.CpuHandleStaging(GITexSRV),
            };
            CopyTable(UpsampleTable, UpsampleSrc, 5);
        }

        Ready = true;
    }

    void FReSTIRGI::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                         u32 _StagingSlot) {
        // A tabela shader-visible é versionada pela mesma paridade dos reservoirs.
        static_assert(kParityTables == FCommandQueue::kFramesInFlight,
                      "tabela de trace do ReSTIR versionada por paridade: com mais frames em voo "
                      "e preciso mais tabelas (ou desacoplar paridade do ping-pong do versionamento)");
        if (!Ready) return;

        // FrameSlot avança mesmo quando o passe não grava; FrameParity identifica a tabela correta.
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[FrameParity] + 13);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FReSTIRGI::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj, const Vec3& _CameraPos,
                                   u32 _Width, u32 _Height, const Vec3& _SunDir, f32 _SunIntensity,
                                   const Vec3& _SunColor, u32 _FrameIndex, f32 _SkyIntensity,
                                   const Mat44& _View, const Vec2& _JitterDeltaUv,
                                   u32 _PrevSurfaceSlot, u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.InvViewProj     = _InvViewProj;
        CPU.View            = _View;
        CPU.CameraPos       = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 1.0f };
        CPU.ScreenParams    = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.TraceScreenParams = { static_cast<f32>(Width), static_cast<f32>(Height),
                                  1.0f / static_cast<f32>(Width),
                                  1.0f / static_cast<f32>(Height) };
        // Cada fase 2x2 atualiza um pixel full-res por texel interno.
        const u32 Phase = HalfRes ? (_FrameIndex & 3u) : 0u;
        CPU.ResolutionParams = { HalfRes ? 2.0f : 1.0f,
                                 static_cast<f32>(Phase & 1u),
                                 static_cast<f32>((Phase >> 1u) & 1u),
                                 HalfRes ? 1.0f : 0.0f };
        CPU.GridMinSpacing  = GIGridMinSpacing;
        CPU.GridCount       = GIGridCount;
        CPU.AtlasParams     = GIAtlasParams;
        CPU.SunDirIntensity = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        // w = ShadowRayMask, sem translucência.
        CPU.SunColor        = { _SunColor.X, _SunColor.Y, _SunColor.Z,
                                static_cast<f32>(FoliageShadows ? kRTMaskShadowFull
                                                                : kRTMaskShadowFast) };
        CPU.TraceParams     = { (f32)_FrameIndex, GIMaxRayDist, _SkyIntensity,
                                RayEps.HitShadowRayBias };
        // GI cru usa teto de firefly mais restritivo.
        CPU.ShadeParams     = { 0.0f, AlbedoLOD, // .x livre
                                UseNrd ? FireflyMax : FireflyMaxRaw, ValidateInterval };
        // Sem superfície anterior, este frame usa apenas o candidato novo.
        const bool HasSurfaceHistory = _PrevSurfaceSlot != kInvalidSlot;
        CPU.ReuseParams     = { MCap, PosRejectScale, Visibility ? 1.0f : 0.0f,
                                (Temporal && HasSurfaceHistory) ? 1.0f : 0.0f };
        CPU.SpatialParams   = { SpatialRadius, SpatialCount, Spatial ? 1.0f : 0.0f, NormalReject };
        CPU.JitterParams    = { _JitterDeltaUv.X, _JitterDeltaUv.Y,
                                static_cast<f32>(_PunctualLightCount), RayEps.MaxAge }; // z = luzes (F5)
        CPU.PolicyParams    = { BackfacePolicy ? 1.0f : 0.0f, BoilingStrength,
                                TemporalBiasCorr ? 1.0f : 0.0f,
                                JacobianKillBackface ? 1.0f : 0.0f };
        CPU.RayEpsA         = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                                RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB         = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                                FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.GIDistParams    = { GIHit.DistTile, GIHit.DistAtlasW, GIHit.DistAtlasH,
                                GIHit.SkipModePacked() };
        // .w = piso de roughness do cache não direcional.
        CPU.GIBiasParams    = { GIHit.BiasScale, GIHit.BiasMax, GIHit.FadeProbes,
                                GIHit.SecondaryRoughnessMin };
        CPU.ReGIRGridMinSlots     = ReGIRParams.GridMinSlots;
        CPU.ReGIRInvCellEnabled   = ReGIRParams.InvCellSizeEnabled;
        CPU.ReGIRGridCountSamples = ReGIRParams.GridCountSamples;
        CPU.ReGIRResources        = ReGIRParams.Resources;
        CPU.RadianceCacheCamCell     = RadianceCacheParams.CameraPosCell;
        CPU.RadianceCacheLodCapFlags = RadianceCacheParams.LodCapacityFlags;
        CPU.RadianceCacheResources   = RadianceCacheParams.Resources;
        CPU.GICascades               = GICascadesCPU;
        CPU.SkyParams             = SkyLutParams;
        // Slots bindless em float usam -1 como sentinela; nunca converta kInvalidSlot.
        CPU.DebugParams           = { (TraceTimed && TimerSlot != kInvalidSlot)
                                          ? static_cast<f32>(TimerSlot) : -1.0f,
                                      (SourceDebug && SourceDebugUAV != kInvalidSlot)
                                          ? static_cast<f32>(SourceDebugUAV) : -1.0f,
                                      0.0f, 0.0f };
        CPU.HistoryParams         = { static_cast<f32>(HasSurfaceHistory ? _PrevSurfaceSlot : 0u),
                                      (HalfRes && HasSurfaceHistory && ReconstructionHistoryValid)
                                          ? 1.0f : 0.0f,
                                      0.0f, 0.0f };
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

    void FReSTIRGI::RecordTrace(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap,
                                 FGpuProfiler* _Profiler) {
        if (!Ready) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        const u32 p = FrameParity, prev = 1u - FrameParity;

        if (NeedsClear) {
            // Res1 é UINT; M == 0 marca reservoir sem histórico.
            Transition(_CL, Res0[prev].Get(), Res0State[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const float ZeroF[4] = { 0, 0, 0, 0 };
            _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(Res0UAV[prev]),
                                               _SRVHeap.CpuHandleStaging(Res0UAV[prev]),
                                               Res0[prev].Get(), ZeroF, 0, nullptr);
            Transition(_CL, Res1[prev].Get(), Res1State[prev], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const UINT ZeroU[4] = { 0, 0, 0, 0 };
            _CL->ClearUnorderedAccessViewUint(_SRVHeap.GpuHandle(Res1UAV[prev]),
                                              _SRVHeap.CpuHandleStaging(Res1UAV[prev]),
                                              Res1[prev].Get(), ZeroU, 0, nullptr);
            NeedsClear = false;
        }

        Transition(_CL, Res0[prev].Get(), Res0State[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Res1[prev].Get(), Res1State[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Res0[p].Get(), Res0State[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, Res1[p].Get(), Res1State[p], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (HalfRes) {
            Transition(_CL, TraceGITexture.Get(), TraceGITextureState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
            Transition(_CL, GITexture.Get(), GITextureState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (SourceDebug && SourceDebugTex) {
            Transition(_CL, SourceDebugTex.Get(), SourceDebugState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (_Profiler) _Profiler->Begin(_CL, "Temporal + Trace Secundário");
        const bool Timed = TraceTimed && TimerSlot != kInvalidSlot;
        (Timed ? TracePSOTimed : TracePSO).Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[p]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable[p]));
        // A extensão exige a tabela do UAV reservado.
        if (Timed)
            _CL->SetComputeRootDescriptorTable(FComputePipeline::kNvApiRootParam,
                                               FShaderTimer::ExtnTable(_SRVHeap));
        _CL->Dispatch(GX, GY, 1);
        if (_Profiler) _Profiler->End(_CL);


        if (Spatial) {
            if (_Profiler) _Profiler->Begin(_CL, "Reuso Espacial + Resolve");
            Transition(_CL, Res0[p].Get(), Res0State[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, Res1[p].Get(), Res1State[p], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_RESOURCE_BARRIER UB{};
            UB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UB.UAV.pResource = HalfRes ? TraceGITexture.Get() : GITexture.Get();
            _CL->ResourceBarrier(1, &UB);

            SpatialPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpatialTable[p]));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceGITexUAV));
            _CL->Dispatch(GX, GY, 1);
            if (_Profiler) _Profiler->End(_CL);
        }

        if (HalfRes) {
            if (_Profiler) _Profiler->Begin(_CL, "Reconstrução temporal 4 fases");
            Transition(_CL, TraceGITexture.Get(), TraceGITextureState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, GITexture.Get(), GITextureState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, ResolvedGITexture.Get(), ResolvedGITextureState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            UpsamplePSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpsampleTable));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ResolvedGITexUAV));
            _CL->Dispatch((FullWidth + 7u) / 8u, (FullHeight + 7u) / 8u, 1);

            // Resolve separado evita read/write simultâneo e preserva o descriptor público.
            Transition(_CL, ResolvedGITexture.Get(), ResolvedGITextureState,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(_CL, GITexture.Get(), GITextureState, D3D12_RESOURCE_STATE_COPY_DEST);
            _CL->CopyResource(GITexture.Get(), ResolvedGITexture.Get());
            ReconstructionHistoryValid = true;
            if (_Profiler) _Profiler->End(_CL);
        }

        // Deferred, NRD e debug consomem a saída em estágios diferentes.
        Transition(_CL, GITexture.Get(), GITextureState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (SourceDebug && SourceDebugTex) {
            Transition(_CL, SourceDebugTex.Get(), SourceDebugState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        FrameParity ^= 1u;
    }

    void FReSTIRGI::OnRegisterDebugTargets() {
        if (!SourceDebug || !SourceDebugTex || SourceDebugSRV == kInvalidSlot) return;
        // A cor já está em faixa de visualização.
        DebugTargets::Register(kSourceDebugTargetName, SourceDebugSRV, EDebugDecode::Raw,
                               0, 1, 1.0f, 0, /*LinearFilter*/ false);
    }

    void FReSTIRGI::SetupNrdPack(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                 ID3D12Resource* _ViewZ, ID3D12Resource* _NormalRough,
                                 ID3D12Resource* _Mv, ID3D12Resource* _DiffRadHit, ID3D12Resource* _Out) {
        // Recriado a cada mudança de alocação do NRD.
        {
            auto FreeIf = [&](u32& Slot, u32 Count) {
                if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
            };
            FreeIf(PackUavTable, 4);
            FreeIf(PackSrvTable, 4);
            FreeIf(NrdOutSRV, 1);
        }
        if (!Ready || !_ViewZ || !_NormalRough || !_Mv || !_DiffRadHit || !_Out) return;

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
        const u32 GX = (FullWidth + 7) / 8, GY = (FullHeight + 7) / 8;
        NrdPackPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(PackSrvTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(PackUavTable));
        _CL->Dispatch(GX, GY, 1);
    }

    void FReSTIRGI::CreatePipelines(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "ReSTIRGITrace.cs_6_6.cso", 14, 3, true);
        SpatialPSO.Initialize(_Device, "ReSTIRGISpatial.cs_6_6.cso", 10, 1, true);
        UpsamplePSO.Initialize(_Device, "ReSTIRGIUpsample.cs_6_6.cso", 5, 1, true);
        NrdPackPSO.Initialize(_Device, "ReSTIRNrdPack.cs_6_6.cso", 4, 4, false);

        if (FShaderTimer::IsAvailable()) {
            try {
                TracePSOTimed.Initialize(_Device, "ReSTIRGITraceTimed.cs_6_6.cso", 14, 3, true, true);
                TraceTimed = true;
            } catch (const std::exception&) {
                LogWarning("ReSTIRGITraceTimed.cso Ausente — Timer do ReSTIR GI Indisponível.");
            }
        }
    }


    FPassShaderStems FReSTIRGI::ShaderStems() const {
        static const char* const kStems[] = { "ReSTIRGITrace.cs", "ReSTIRGITraceTimed.cs",
                                              "ReSTIRGISpatial.cs", "ReSTIRGIUpsample.cs",
                                              "ReSTIRNrdPack.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FReSTIRGI::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
