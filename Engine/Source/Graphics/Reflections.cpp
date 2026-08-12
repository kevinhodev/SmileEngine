#include "Smile/Graphics/Reflections.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/RTMasks.h"
#include "Smile/Graphics/ShaderTimer.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cmath>
#include <cstring>
#include <exception>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kRadianceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ComPtr<ID3D12Resource> CreateUAVTex2D(ID3D12Device* _Device, u32 _W, u32 _H,
                                              DXGI_FORMAT _Fmt, const char* _Label) {
            return GpuResources::CreateTex2D(
                _Device, _W, _H, _Fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI, nullptr, 1, 1, _Label);
        }
    }

    void FReflections::Initialize(ID3D12Device* _Device) {
        RecreatePipelines(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FReflections::RecreatePipelines(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "ReflectionTrace.cs_6_6.cso", 12, 3, true);
        TraceMirrorPSO.Initialize(_Device, "ReflectionTraceMirror.cs_6_6.cso", 12, 2, true);
        ResolvePSO.Initialize(_Device, "ReflectionResolve.cs_6_0.cso", 5, 2, false);
        TemporalPSO.Initialize(_Device, "ReflectionTemporal.cs_6_0.cso", 5, 1, false);
        SpatialPSO.Initialize(_Device, "ReflectionSpatial.cs_6_0.cso", 3, 1, false);
        NrdPackPSO.Initialize(_Device, "ReflectionNrdPack.cs_6_0.cso", 3, 1, false);
        WaterTracePSO.Initialize(_Device, "WaterReflectionTrace.cs_6_6.cso", 15, 2, true);
        WaterTemporalPSO.Initialize(_Device, "WaterReflectionTemporal.cs_6_0.cso", 5, 1, false);
        // Gemea instrumentada do trace half-res (ver FShaderTimer): mesmas tabelas, mais o slot
        // falso da NVAPI no root sig. Fora de GPU NVIDIA nao existe e o passe segue igual.
        TraceTimed = false;
        if (FShaderTimer::IsAvailable()) {
            try {
                TracePSOTimed.Initialize(_Device, "ReflectionTraceTimed.cs_6_6.cso", 12, 3, true, true);
                TraceTimed = true;
            } catch (const std::exception&) {
                LogWarning("ReflectionTraceTimed.cso ausente — timer das reflexoes indisponivel.");
            }
        }
        CreateCompositePipeline(_Device);
        // Hot reload pode mudar o significado dos canais temporais (especialmente hit-distance).
        // Nunca reaproveitar history produzido por uma versao anterior do shader.
        NeedsHistoryClear = true;
        NeedsWaterHistoryClear = true;
    }

    void FReflections::CreateConstantBuffer(ID3D12Device* _Device) {
        static_assert(sizeof(ReflectionConstants) % 256 == 0,
                      "o CBAddr indexa por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(ReflectionConstants), FCommandQueue::kFramesInFlight);
        CB       = Upload.Resource;
        MappedCB = Upload.Mapped;
    }

    void FReflections::CreateCompositePipeline(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 6; // t4 = GBufferC (metallic), t5 = GBufferA (BaseColor)
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[2]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samp{};
        Samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samp.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Samp.MaxLOD           = D3D12_FLOAT32_MAX;
        Samp.ShaderRegister   = 0;
        Samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC RSDesc{};
        RSDesc.NumParameters     = _countof(RootParams);
        RSDesc.pParameters       = RootParams;
        RSDesc.NumStaticSamplers = 1;
        RSDesc.pStaticSamplers   = &Samp;
        RSDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> RootBlob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&RSDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &RootBlob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob) LogError(std::string("Reflection composite root sig: ") +
                                    static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, RootBlob->GetBufferPointer(),
                 RootBlob->GetBufferSize(), IID_PPV_ARGS(&CompositeRS)));

        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PS = LoadShaderBytecode("ReflectionComposite.ps_6_0.cso");
        auto WaterPS = LoadShaderBytecode("WaterReflectionComposite.ps_6_0.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature = CompositeRS.Get();
        PSODesc.VS             = { VS.data(), VS.size() };
        PSODesc.PS             = { PS.data(), PS.size() };
        PSODesc.BlendState.RenderTarget[0].BlendEnable           = TRUE;
        PSODesc.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        PSODesc.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        PSODesc.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        PSODesc.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ZERO;
        PSODesc.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ONE;
        PSODesc.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        PSODesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        PSODesc.SampleMask                 = UINT_MAX;
        PSODesc.RasterizerState.FillMode   = D3D12_FILL_MODE_SOLID;
        PSODesc.RasterizerState.CullMode   = D3D12_CULL_MODE_NONE;
        PSODesc.RasterizerState.DepthClipEnable = FALSE;
        PSODesc.DepthStencilState.DepthEnable   = FALSE;
        PSODesc.DepthStencilState.StencilEnable = FALSE;
        PSODesc.PrimitiveTopologyType      = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets           = 1;
        PSODesc.RTVFormats[0]              = kRadianceFormat;
        PSODesc.DSVFormat                  = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc                 = { 1, 0 };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&CompositePSO)));
        PSODesc.PS = { WaterPS.data(), WaterPS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&WaterCompositePSO)));
    }

    void FReflections::ReleaseResize(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(RadianceSRVSlot, 1);
        Free(RayDataSRVSlot, 1);
        Free(RayMotionSRVSlot, 1);
        Free(ResolvedSRVSlot, 1);
        Free(ResolvedUAVSlot, 1);
        Free(ResolvedMotionSRVSlot, 1);
        Free(ResolvedMotionUAVSlot, 1);
        Free(HistorySRVSlot[0], 1); Free(HistorySRVSlot[1], 1);
        Free(HistoryUAVSlot[0], 1); Free(HistoryUAVSlot[1], 1);
        Free(DenoisedSRVSlot, 1); Free(DenoisedUAVSlot, 1);
        Free(TraceUAVTable, 3);
        Free(ResolvedUAVTable, 2);
        for (u32 i = 0; i < kTraceTables; ++i) Free(TraceTable[i], 12);
        Free(ResolveTableStart, 5);
        Free(TemporalTable[0], 5); Free(TemporalTable[1], 5);
        Free(SpatialTable[0], 3); Free(SpatialTable[1], 3);
        Free(CompositeTable[0], 6); Free(CompositeTable[1], 6);
        Free(SpecPackSrvTable, 3);
        Free(SpecPackUAVSlot, 1);
        Free(NrdOutSpecSRV, 1);
        NrdInSpec = nullptr;
        Free(CompositeTableNrd[0], 6); Free(CompositeTableNrd[1], 6);
        Free(CompositeTableRaw, 6);
        Free(WaterResolvedSRVSlot, 1); Free(WaterResolvedUAVSlot, 1);
        Free(WaterMotionSRVSlot, 1); Free(WaterMotionUAVSlot, 1);
        Free(WaterHistorySRVSlot[0], 1); Free(WaterHistorySRVSlot[1], 1);
        Free(WaterHistoryUAVSlot[0], 1); Free(WaterHistoryUAVSlot[1], 1);
        Free(WaterTraceUAVTable, 2);
        for (u32 i = 0; i < kTraceTables; ++i) Free(WaterTraceTable[i], 15);
        Free(WaterTemporalTable[0], 5); Free(WaterTemporalTable[1], 5);
        Free(WaterCompositeTable[0], 6); Free(WaterCompositeTable[1], 6);
        Free(WaterCompositeRawTable, 6);
        Free(WaterSpecHitTable, 2);
        Radiance.Reset();
        RayData.Reset();
        RayMotion.Reset();
        Resolved.Reset();
        ResolvedMotion.Reset();
        History[0].Reset(); History[1].Reset();
        Denoised.Reset();
        WaterResolved.Reset(); WaterMotion.Reset();
        WaterHistory[0].Reset(); WaterHistory[1].Reset();
        RadianceState = D3D12_RESOURCE_STATE_COMMON;
        RayDataState  = D3D12_RESOURCE_STATE_COMMON;
        RayMotionState = D3D12_RESOURCE_STATE_COMMON;
        ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        ResolvedMotionState = D3D12_RESOURCE_STATE_COMMON;
        HistoryState[0] = HistoryState[1] = D3D12_RESOURCE_STATE_COMMON;
        DenoisedState = D3D12_RESOURCE_STATE_COMMON;
        WaterResolvedState = WaterMotionState = D3D12_RESOURCE_STATE_COMMON;
        WaterHistoryState[0] = WaterHistoryState[1] = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FReflections::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                      u32 _Width, u32 _Height, u32 _TlasSlot, u32 _SkyViewSlot,
                                      u32 _InstanceSlot, u32 _IrradSlot, u32 _DepthSlot,
                                       u32 _GBufferSlot, u32 _GBufferCSlot, u32 _BRDFLutSlot,
                                       u32 _GBufferASlot, u32 _VelocitySlot,
                                       u32 _SceneColorSlot, u32 _SceneDepthSlot,
                                       u32 _SceneColorMipCount,
                                       u32 _AtmosphereSpecularSlot, u32 _HDRSpecularSlot,
                                       u32 _DistSlot, u32 _ProbeDataSlot,
                                      const u32 _TransformSlots[FCommandQueue::kFramesInFlight],
                                      const u32 _SurfaceSlots[FCommandQueue::kFramesInFlight]) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        if (_Width == 0 || _Height == 0 || _TlasSlot == kInvalidSlot ||
            _InstanceSlot == kInvalidSlot || _VelocitySlot == kInvalidSlot ||
            _SceneColorSlot == kInvalidSlot || _SceneDepthSlot == kInvalidSlot ||
            _AtmosphereSpecularSlot == kInvalidSlot || _HDRSpecularSlot == kInvalidSlot)
            return;
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f)
            if (_TransformSlots[f] == kInvalidSlot || _SurfaceSlots[f] == kInvalidSlot) return;

        Width = _Width; Height = _Height;
        HalfWidth = (_Width + 1) / 2; HalfHeight = (_Height + 1) / 2;
        WaterSceneColorMaxMip = static_cast<f32>(_SceneColorMipCount > 0
            ? _SceneColorMipCount - 1 : 0);
        DepthSlotCached = _DepthSlot; GBufferSlotCached = _GBufferSlot; BRDFLutSlotCached = _BRDFLutSlot;
        GBufferCSlotCached = _GBufferCSlot; GBufferASlotCached = _GBufferASlot;
        // Rotulos separados por ETAPA (meia-res / resolve / historico / agua): e o agrupamento que
        // responde "onde estao os MB", ja que todos usam o mesmo formato e so mudam de resolucao.
        Radiance = CreateUAVTex2D(_Device, HalfWidth, HalfHeight, kRadianceFormat,
                                  "Reflexoes · meia-res");
        RayData  = CreateUAVTex2D(_Device, HalfWidth, HalfHeight, kRadianceFormat,
                                  "Reflexoes · meia-res");
        RayMotion = CreateUAVTex2D(_Device, HalfWidth, HalfHeight, kRadianceFormat,
                                   "Reflexoes · meia-res");
        Resolved = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat, "Reflexoes · resolve");
        ResolvedMotion = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat,
                                        "Reflexoes · resolve");
        History[0] = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat,
                                    "Reflexoes · historico");
        History[1] = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat,
                                    "Reflexoes · historico");
        Denoised   = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat, "Reflexoes · denoise");
        WaterResolved = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat, "Reflexoes · agua");
        WaterMotion   = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat, "Reflexoes · agua");
        WaterHistory[0] = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat,
                                         "Reflexoes · agua");
        WaterHistory[1] = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat,
                                         "Reflexoes · agua");
        RadianceState = RayDataState = RayMotionState = ResolvedState =
            ResolvedMotionState = D3D12_RESOURCE_STATE_COMMON;
        HistoryState[0] = HistoryState[1] = D3D12_RESOURCE_STATE_COMMON;
        DenoisedState = D3D12_RESOURCE_STATE_COMMON;
        WaterResolvedState = WaterMotionState = D3D12_RESOURCE_STATE_COMMON;
        WaterHistoryState[0] = WaterHistoryState[1] = D3D12_RESOURCE_STATE_COMMON;
        NeedsHistoryClear = true; FrameParity = 0;
        NeedsWaterHistoryClear = true; WaterFrameParity = WaterCurrParity = 0;
        WaterUseRaw = false;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kRadianceFormat;
        Srv.Texture2D.MipLevels     = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kRadianceFormat;

        RadianceSRVSlot = _SRVHeap.Allocate(1);
        RayDataSRVSlot  = _SRVHeap.Allocate(1);
        RayMotionSRVSlot = _SRVHeap.Allocate(1);
        ResolvedSRVSlot = _SRVHeap.Allocate(1);
        ResolvedUAVSlot = _SRVHeap.Allocate(1);
        ResolvedMotionSRVSlot = _SRVHeap.Allocate(1);
        ResolvedMotionUAVSlot = _SRVHeap.Allocate(1);
        HistorySRVSlot[0] = _SRVHeap.Allocate(1); HistorySRVSlot[1] = _SRVHeap.Allocate(1);
        HistoryUAVSlot[0] = _SRVHeap.Allocate(1); HistoryUAVSlot[1] = _SRVHeap.Allocate(1);
        DenoisedSRVSlot = _SRVHeap.Allocate(1); DenoisedUAVSlot = _SRVHeap.Allocate(1);
        WaterResolvedSRVSlot = _SRVHeap.Allocate(1); WaterResolvedUAVSlot = _SRVHeap.Allocate(1);
        WaterMotionSRVSlot = _SRVHeap.Allocate(1); WaterMotionUAVSlot = _SRVHeap.Allocate(1);
        WaterHistorySRVSlot[0] = _SRVHeap.Allocate(1); WaterHistorySRVSlot[1] = _SRVHeap.Allocate(1);
        WaterHistoryUAVSlot[0] = _SRVHeap.Allocate(1); WaterHistoryUAVSlot[1] = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, Radiance.Get(), Srv, RadianceSRVSlot);
        _SRVHeap.CreateSRV(_Device, RayData.Get(),  Srv, RayDataSRVSlot);
        _SRVHeap.CreateSRV(_Device, RayMotion.Get(), Srv, RayMotionSRVSlot);
        _SRVHeap.CreateSRV(_Device, Resolved.Get(), Srv, ResolvedSRVSlot);
        _SRVHeap.CreateUAV(_Device, Resolved.Get(), Uav, ResolvedUAVSlot);
        _SRVHeap.CreateSRV(_Device, ResolvedMotion.Get(), Srv, ResolvedMotionSRVSlot);
        _SRVHeap.CreateUAV(_Device, ResolvedMotion.Get(), Uav, ResolvedMotionUAVSlot);
        _SRVHeap.CreateSRV(_Device, History[0].Get(), Srv, HistorySRVSlot[0]);
        _SRVHeap.CreateSRV(_Device, History[1].Get(), Srv, HistorySRVSlot[1]);
        _SRVHeap.CreateUAV(_Device, History[0].Get(), Uav, HistoryUAVSlot[0]);
        _SRVHeap.CreateUAV(_Device, History[1].Get(), Uav, HistoryUAVSlot[1]);
        _SRVHeap.CreateSRV(_Device, Denoised.Get(), Srv, DenoisedSRVSlot);
        _SRVHeap.CreateUAV(_Device, Denoised.Get(), Uav, DenoisedUAVSlot);
        _SRVHeap.CreateSRV(_Device, WaterResolved.Get(), Srv, WaterResolvedSRVSlot);
        _SRVHeap.CreateUAV(_Device, WaterResolved.Get(), Uav, WaterResolvedUAVSlot);
        _SRVHeap.CreateSRV(_Device, WaterMotion.Get(), Srv, WaterMotionSRVSlot);
        _SRVHeap.CreateUAV(_Device, WaterMotion.Get(), Uav, WaterMotionUAVSlot);
        for (u32 i = 0; i < 2; ++i) {
            _SRVHeap.CreateSRV(_Device, WaterHistory[i].Get(), Srv, WaterHistorySRVSlot[i]);
            _SRVHeap.CreateUAV(_Device, WaterHistory[i].Get(), Uav, WaterHistoryUAVSlot[i]);
        }

        TraceUAVTable = _SRVHeap.Allocate(3);
        _SRVHeap.CreateUAV(_Device, Radiance.Get(), Uav, TraceUAVTable);
        _SRVHeap.CreateUAV(_Device, RayData.Get(),  Uav, TraceUAVTable + 1);
        _SRVHeap.CreateUAV(_Device, RayMotion.Get(), Uav, TraceUAVTable + 2);

        ResolvedUAVTable = _SRVHeap.Allocate(2);
        _SRVHeap.CreateUAV(_Device, Resolved.Get(), Uav, ResolvedUAVTable);
        _SRVHeap.CreateUAV(_Device, ResolvedMotion.Get(), Uav, ResolvedUAVTable + 1);

        WaterTraceUAVTable = _SRVHeap.Allocate(2);
        _SRVHeap.CreateUAV(_Device, WaterResolved.Get(), Uav, WaterTraceUAVTable);
        _SRVHeap.CreateUAV(_Device, WaterMotion.Get(), Uav, WaterTraceUAVTable + 1);

        // t0..t7 fixos; t8 = luzes; t9 = transform atual<->anterior; t10/t11 = superficies
        // atual/anterior. As tres ultimas entradas sao versionadas pelo FrameSlot.
        // Uma tabela por frame em voo: o t8 muda todo frame e a tabela do frame anterior ainda
        // pode estar sendo lida pela GPU (descriptor versioning). t4/t5 eram os merged VB/IB,
        // aposentados pelo bindless (InstanceGeo); hoje levam o atlas de distancia e o ProbeData
        // do DDGI, que o ShadeSurfaceHit usa no gather completo do 2o bounce.
        D3D12_CPU_DESCRIPTOR_HANDLE TSrc[8] = {
            _SRVHeap.CpuHandleStaging(_TlasSlot),
            _SRVHeap.CpuHandleStaging(_SkyViewSlot),
            _SRVHeap.CpuHandleStaging(_InstanceSlot),
            _SRVHeap.CpuHandleStaging(_IrradSlot),
            _SRVHeap.CpuHandleStaging(_DistSlot),
            _SRVHeap.CpuHandleStaging(_ProbeDataSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
        };
        UINT TDstCount = 8; UINT TSrcCounts[8] = { 1,1,1,1,1,1,1,1 };
        for (u32 i = 0; i < kTraceTables; ++i) {
            TraceTable[i] = _SRVHeap.Allocate(12);
            D3D12_CPU_DESCRIPTOR_HANDLE TDst = _SRVHeap.CpuHandle(TraceTable[i]);
            _Device->CopyDescriptors(1, &TDst, &TDstCount, 8, TSrc, TSrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE MotionDst = _SRVHeap.CpuHandle(TraceTable[i] + 9);
            D3D12_CPU_DESCRIPTOR_HANDLE MotionSrc[3] = {
                _SRVHeap.CpuHandleStaging(_TransformSlots[i]),
                _SRVHeap.CpuHandleStaging(_SurfaceSlots[i]),
                _SRVHeap.CpuHandleStaging(_SurfaceSlots[1u - i]),
            };
            UINT MotionCount = 3; UINT MotionOnes[3] = { 1,1,1 };
            _Device->CopyDescriptors(1, &MotionDst, &MotionCount, 3, MotionSrc, MotionOnes,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            // A agua nao pertence ao snapshot temporal dos opacos. Seu trace usa t0..t7 iguais,
            // t8 versionado para luzes e t9 = velocity que inclui a fase da onda.
            WaterTraceTable[i] = _SRVHeap.Allocate(15);
            D3D12_CPU_DESCRIPTOR_HANDLE WDst = _SRVHeap.CpuHandle(WaterTraceTable[i]);
            _Device->CopyDescriptors(1, &WDst, &TDstCount, 8, TSrc, TSrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE WVelDst = _SRVHeap.CpuHandle(WaterTraceTable[i] + 9);
            D3D12_CPU_DESCRIPTOR_HANDLE WVelSrc = _SRVHeap.CpuHandleStaging(_VelocitySlot);
            UINT One = 1;
            _Device->CopyDescriptors(1, &WVelDst, &One, 1, &WVelSrc, &One,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE WDataDst = _SRVHeap.CpuHandle(WaterTraceTable[i] + 10);
            D3D12_CPU_DESCRIPTOR_HANDLE WDataSrc = _SRVHeap.CpuHandleStaging(_GBufferCSlot);
            _Device->CopyDescriptors(1, &WDataDst, &One, 1, &WDataSrc, &One,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE WSceneDst =
                _SRVHeap.CpuHandle(WaterTraceTable[i] + 11);
            D3D12_CPU_DESCRIPTOR_HANDLE WSceneSrc[2] = {
                _SRVHeap.CpuHandleStaging(_SceneColorSlot),
                _SRVHeap.CpuHandleStaging(_SceneDepthSlot),
            };
            UINT SceneCount = 2; UINT SceneOnes[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &WSceneDst, &SceneCount, 2, WSceneSrc, SceneOnes,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE WEnvironmentDst =
                _SRVHeap.CpuHandle(WaterTraceTable[i] + 13);
            D3D12_CPU_DESCRIPTOR_HANDLE WEnvironmentSrc[2] = {
                _SRVHeap.CpuHandleStaging(_AtmosphereSpecularSlot),
                _SRVHeap.CpuHandleStaging(_HDRSpecularSlot),
            };
            UINT EnvironmentCount = 2; UINT EnvironmentOnes[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &WEnvironmentDst, &EnvironmentCount, 2,
                                     WEnvironmentSrc, EnvironmentOnes,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        ResolveTableStart = _SRVHeap.Allocate(5);
        D3D12_CPU_DESCRIPTOR_HANDLE RDst = _SRVHeap.CpuHandle(ResolveTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE RSrc[5] = {
            _SRVHeap.CpuHandleStaging(RadianceSRVSlot),
            _SRVHeap.CpuHandleStaging(RayDataSRVSlot),
            _SRVHeap.CpuHandleStaging(RayMotionSRVSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
        };
        UINT RDstCount = 5; UINT RSrcCounts[5] = { 1,1,1,1,1 };
        _Device->CopyDescriptors(1, &RDst, &RDstCount, 5, RSrc, RSrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        UINT Five = 5; UINT Ones[5] = { 1,1,1,1,1 };
        for (u32 curr = 0; curr < 2; ++curr) {
            const u32 prev = 1u - curr;
            TemporalTable[curr] = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE TDst2 = _SRVHeap.CpuHandle(TemporalTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE TSrc2[5] = {
                _SRVHeap.CpuHandleStaging(ResolvedSRVSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(HistorySRVSlot[prev]),
                _SRVHeap.CpuHandleStaging(ResolvedMotionSRVSlot),
            };
            _Device->CopyDescriptors(1, &TDst2, &Five, 5, TSrc2, Ones,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            // Spatial le o History[curr] (recem-acumulado) + gbuf + depth -> Denoised.
            SpatialTable[curr] = _SRVHeap.Allocate(3);
            D3D12_CPU_DESCRIPTOR_HANDLE SDst = _SRVHeap.CpuHandle(SpatialTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE SSrc[3] = {
                _SRVHeap.CpuHandleStaging(HistorySRVSlot[curr]),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
            };
            UINT Three = 3;
            _Device->CopyDescriptors(1, &SDst, &Three, 3, SSrc, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            // Composite le o Denoised (saida do spatial), nao mais o History[curr] direto.
            // t4 = GBufferC (metallic saiu do B.a na dieta do G-buffer).
            CompositeTable[curr] = _SRVHeap.Allocate(6);
            D3D12_CPU_DESCRIPTOR_HANDLE CDst2 = _SRVHeap.CpuHandle(CompositeTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE CSrc2[6] = {
                _SRVHeap.CpuHandleStaging(DenoisedSRVSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_BRDFLutSlot),
                _SRVHeap.CpuHandleStaging(_GBufferCSlot),
                _SRVHeap.CpuHandleStaging(_GBufferASlot), // t5 = BaseColor
            };
            UINT Six = 6; UINT OnesC[6] = { 1,1,1,1,1,1 };
            _Device->CopyDescriptors(1, &CDst2, &Six, 6, CSrc2, OnesC, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        // NRD: tabela do pack especular [Resolved, GBuffer, Depth]. A UAV da IN_SPEC e a SRV da
        // OUT_SPEC sao montadas em SetupNrdSpec (dependem dos recursos do FNrdDenoiser).
        SpecPackSrvTable = _SRVHeap.Allocate(3);
        D3D12_CPU_DESCRIPTOR_HANDLE PDst = _SRVHeap.CpuHandle(SpecPackSrvTable);
        D3D12_CPU_DESCRIPTOR_HANDLE PSrc[3] = {
            _SRVHeap.CpuHandleStaging(ResolvedSRVSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
        };
        UINT Three2 = 3; UINT Ones3[3] = { 1,1,1 };
        _Device->CopyDescriptors(1, &PDst, &Three2, 3, PSrc, Ones3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // RR: composite lendo o Resolved CRU (ruidoso) — mesma tabela do caseiro, mas com o Resolved
        // no lugar do Denoised. Parity-independente (Resolved nao faz ping-pong), entao uma tabela so.
        CompositeTableRaw = _SRVHeap.Allocate(6);
        D3D12_CPU_DESCRIPTOR_HANDLE RawDst = _SRVHeap.CpuHandle(CompositeTableRaw);
        D3D12_CPU_DESCRIPTOR_HANDLE RawSrc[6] = {
            _SRVHeap.CpuHandleStaging(ResolvedSRVSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
            _SRVHeap.CpuHandleStaging(_BRDFLutSlot),
            _SRVHeap.CpuHandleStaging(_GBufferCSlot),
            _SRVHeap.CpuHandleStaging(_GBufferASlot), // t5 = BaseColor
        };
        UINT RawSix = 6; UINT RawOnes[6] = { 1,1,1,1,1,1 };
        _Device->CopyDescriptors(1, &RawDst, &RawSix, 6, RawSrc, RawOnes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (u32 curr = 0; curr < 2; ++curr) {
            const u32 prev = 1u - curr;
            WaterTemporalTable[curr] = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE WTDst = _SRVHeap.CpuHandle(WaterTemporalTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE WTSrc[5] = {
                _SRVHeap.CpuHandleStaging(WaterResolvedSRVSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(WaterHistorySRVSlot[prev]),
                _SRVHeap.CpuHandleStaging(WaterMotionSRVSlot),
            };
            _Device->CopyDescriptors(1, &WTDst, &Five, 5, WTSrc, Ones,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            WaterCompositeTable[curr] = _SRVHeap.Allocate(6);
            D3D12_CPU_DESCRIPTOR_HANDLE WCDst = _SRVHeap.CpuHandle(WaterCompositeTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE WCSrc[6] = {
                _SRVHeap.CpuHandleStaging(WaterHistorySRVSlot[curr]),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_BRDFLutSlot),
                _SRVHeap.CpuHandleStaging(_GBufferCSlot),
                _SRVHeap.CpuHandleStaging(_GBufferASlot),
            };
            _Device->CopyDescriptors(1, &WCDst, &RawSix, 6, WCSrc, RawOnes,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        WaterCompositeRawTable = _SRVHeap.Allocate(6);
        D3D12_CPU_DESCRIPTOR_HANDLE WRawDst = _SRVHeap.CpuHandle(WaterCompositeRawTable);
        D3D12_CPU_DESCRIPTOR_HANDLE WRawSrc[6] = {
            _SRVHeap.CpuHandleStaging(WaterResolvedSRVSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
            _SRVHeap.CpuHandleStaging(_BRDFLutSlot),
            _SRVHeap.CpuHandleStaging(_GBufferCSlot),
            _SRVHeap.CpuHandleStaging(_GBufferASlot),
        };
        _Device->CopyDescriptors(1, &WRawDst, &RawSix, 6, WRawSrc, RawOnes,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        WaterSpecHitTable = _SRVHeap.Allocate(2);
        D3D12_CPU_DESCRIPTOR_HANDLE WHDst = _SRVHeap.CpuHandle(WaterSpecHitTable);
        D3D12_CPU_DESCRIPTOR_HANDLE WHSrc[2] = {
            _SRVHeap.CpuHandleStaging(WaterResolvedSRVSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
        };
        UINT Two = 2; UINT TwoOnes[2] = { 1, 1 };
        _Device->CopyDescriptors(1, &WHDst, &Two, 2, WHSrc, TwoOnes,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        Ready = true;
    }

    void FReflections::SetupNrdSpec(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                    ID3D12Resource* _NrdInSpec, ID3D12Resource* _NrdOutSpec) {
        // Idempotente: o NRD agora e alocado sob demanda (Renderer::ReconcileNrdAllocation) e
        // isto e re-chamado a cada toggle, nao so depois de um ReleaseResize.
        {
            auto FreeIf = [&](u32& Slot) {
                if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, 1); Slot = kInvalidSlot; }
            };
            FreeIf(SpecPackUAVSlot);
            FreeIf(NrdOutSpecSRV);
        }
        if (!Ready || !_NrdInSpec || !_NrdOutSpec) return;

        // UAV da IN_SPEC do NRD (o pack escreve aqui; o RecordNrdSpecZero limpa via ponteiro).
        NrdInSpec = _NrdInSpec;
        SpecPackUAVSlot = _SRVHeap.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kRadianceFormat; // RGBA16F (== IoFormat IO_SPEC_RADIANCE_HITDIST)
        _SRVHeap.CreateUAV(_Device, _NrdInSpec, Uav, SpecPackUAVSlot);

        // SRV da OUT_SPEC do NRD (o composite le aqui em vez do Denoised caseiro).
        NrdOutSpecSRV = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kRadianceFormat;
        Srv.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, _NrdOutSpec, Srv, NrdOutSpecSRV);

        // Composite-NRD: [OUT_SPEC, GBuffer, Depth, BRDFLut, GBufferC]. Identico p/ as 2 paridades
        // (a OUT do NRD nao e ping-pong), entao CurrParity nao importa neste caminho.
        UINT Six = 6; UINT Ones[6] = { 1,1,1,1,1,1 };
        for (u32 i = 0; i < 2; ++i) {
            CompositeTableNrd[i] = _SRVHeap.Allocate(6);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(CompositeTableNrd[i]);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[6] = {
                _SRVHeap.CpuHandleStaging(NrdOutSpecSRV),
                _SRVHeap.CpuHandleStaging(GBufferSlotCached),
                _SRVHeap.CpuHandleStaging(DepthSlotCached),
                _SRVHeap.CpuHandleStaging(BRDFLutSlotCached),
                _SRVHeap.CpuHandleStaging(GBufferCSlotCached),
                _SRVHeap.CpuHandleStaging(GBufferASlotCached), // t5 = BaseColor
            };
            _Device->CopyDescriptors(1, &Dst, &Six, 6, Src, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    void FReflections::SetGIParams(const Vec3& _GridMin, f32 _Spacing, const Vec3& _GridCount,
                                   f32 _AtlasTile, f32 _AtlasW, f32 _AtlasH, f32 _MaxRayDist) {
        GIGridMinSpacing = { _GridMin.X, _GridMin.Y, _GridMin.Z, _Spacing };
        GIGridCount      = { _GridCount.X, _GridCount.Y, _GridCount.Z, 0.0f };
        GIAtlasParams    = { _AtlasTile, _AtlasW, _AtlasH, 0.0f };
        GIMaxRayDist     = _MaxRayDist;
    }

    void FReflections::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj,
                                      const Mat44& _ViewProj, const Mat44& _PrevViewProj,
                                      const Vec3& _CameraPos, const Vec3& _PrevCameraPos,
                                      u32 _Width, u32 _Height, const Vec3& _SunDir,
                                      f32 _SunIntensity, const Vec3& _SunColor, u32 _FrameIndex,
                                      f32 _SkyIntensity,
                                      const Mat44& _View, bool _UseAtmosphereSky,
                                      f32 _WaterEnvironmentIntensity, u32 _PunctualLightCount,
                                      u32 _TemporalInstanceCount, bool _MotionHistoryValid) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.InvViewProj     = _InvViewProj;
        CPU.ViewProj        = _ViewProj;
        CPU.PrevViewProj    = _PrevViewProj;
        CPU.PrevCameraPos   = { _PrevCameraPos.X, _PrevCameraPos.Y, _PrevCameraPos.Z,
                                static_cast<f32>(_TemporalInstanceCount) };
        CPU.View            = _View;
        CPU.WaterEnvironmentParams = { _UseAtmosphereSky ? 1.0f : 0.0f,
                                       _WaterEnvironmentIntensity, 6.0f,
                                       WaterSceneColorMaxMip };
        CPU.TemporalParams  = { Temporal ? MaxFrames : 1.0f, NeighborhoodGamma, SpatialRadius,
                                FullResMaxRough };
        // w = slot bindless do alvo de timer; -1 e o sentinela de "captura off" (ver FShaderTimer).
        CPU.DebugParams     = { (f32)DebugMode, MaxFrames,
                                _MotionHistoryValid ? 1.0f : 0.0f,
                                (TraceTimed && TimerSlot != kInvalidSlot)
                                    ? static_cast<f32>(TimerSlot) : -1.0f };
        // w = nº de luzes puntuais no t8 (F5) — o componente era constante 1.0, livre.
        CPU.CameraPos       = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z,
                                static_cast<f32>(_PunctualLightCount) };
        CPU.ScreenParams    = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.ReflectParams   = { MaxRoughnessToTrace, RoughnessFadeLength,
                                0.0f, AlbedoLOD }; // .z livre
        CPU.GridMinSpacing  = GIGridMinSpacing;
        CPU.GridCount       = GIGridCount;
        CPU.AtlasParams     = GIAtlasParams;
        CPU.SunDirIntensity = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        // w = ShadowRayMask (ver ReSTIRGI.cpp: translucido fora dos dois casos).
        CPU.SunColor        = { _SunColor.X, _SunColor.Y, _SunColor.Z,
                                static_cast<f32>(FoliageShadows ? kRTMaskShadowFull
                                                                : kRTMaskShadowFast) };
        CPU.TraceParams     = { (f32)_FrameIndex, GIMaxRayDist, _SkyIntensity,
                                RayEps.HitShadowRayBias };
        CPU.PolicyParams    = { BackfaceCull ? 1.0f : 0.0f, WaterReflectionScale,
                                std::cos(WaterWindDirection), std::sin(WaterWindDirection) };
        CPU.RayEpsA         = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                                RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB         = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                                FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.GIDistParams    = { GIHit.DistTile, GIHit.DistAtlasW, GIHit.DistAtlasH,
                                GIHit.SkipModePacked() };
        // .w fica em ZERO de proposito: e o piso de roughness do hit secundario, e ele so vale
        // p/ quem guarda a radiancia num cache nao-direcional (DDGI e ReSTIR GI). Aqui o hit e
        // sombreado p/ uma visada conhecida e consumido uma vez so — clampar borraria
        // espelho-no-espelho. Os shaders de reflexao tambem passam 0 na unha; esta linha existe
        // p/ o `.w` nao ser preenchido por engano num refactor do GIBiasParams.
        CPU.GIBiasParams    = { GIHit.BiasScale, GIHit.BiasMax, GIHit.FadeProbes, 0.0f };
        CPU.ReGIRGridMinSlots     = ReGIRParams.GridMinSlots;
        CPU.ReGIRInvCellEnabled   = ReGIRParams.InvCellSizeEnabled;
        CPU.ReGIRGridCountSamples = ReGIRParams.GridCountSamples;
        CPU.ReGIRResources        = ReGIRParams.Resources;
        CPU.RadianceCacheCamCell     = RadianceCacheParams.CameraPosCell;
        CPU.RadianceCacheLodCapFlags = RadianceCacheParams.LodCapacityFlags;
        CPU.RadianceCacheResources   = RadianceCacheParams.Resources;
        CPU.GICascades               = GICascadesCPU;
        CPU.SkyParams             = SkyLutParams;
        CPU.HalfScreenParams = { (f32)HalfWidth, (f32)HalfHeight,
                                 1.0f / (f32)HalfWidth, 1.0f / (f32)HalfHeight };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReflectionConstants),
                    &CPU, sizeof(ReflectionConstants));
    }

    void FReflections::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                            u32 _StagingSlot, u32 _FrameSlot) {
        static_assert(kTraceTables == FCommandQueue::kFramesInFlight,
                      "tabela de trace versionada por frame em voo");
        if (!Ready) return;
        // Escreve so na tabela DESTE FrameSlot: a outra pertence ao frame em voo.
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[_FrameSlot] + 8);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE WaterDst = _SRVHeap.CpuHandle(WaterTraceTable[_FrameSlot] + 8);
        _Device->CopyDescriptors(1, &WaterDst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FReflections::Transition(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _Res,
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

    D3D12_GPU_VIRTUAL_ADDRESS FReflections::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(ReflectionConstants);
    }

    void FReflections::RecordTrace(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap,
                                    FGpuProfiler* _Profiler) {
        if (!Ready) return;
        const u32 HGX = (HalfWidth + 7) / 8, HGY = (HalfHeight + 7) / 8; 
        const u32 FGX = (Width + 7) / 8,     FGY = (Height + 7) / 8;     

        if (_Profiler) _Profiler->Begin(_CL, "Trace half-res");
        Transition(_CL, Radiance.Get(), RadianceState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, RayData.Get(),  RayDataState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, RayMotion.Get(), RayMotionState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool Timed = TraceTimed && TimerSlot != kInvalidSlot;
        (Timed ? TracePSOTimed : TracePSO).Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable));
        // O UAV falso da extensao: o driver troca o acesso, mas a tabela precisa estar setada.
        if (Timed)
            _CL->SetComputeRootDescriptorTable(FComputePipeline::kNvApiRootParam,
                                               FShaderTimer::ExtnTable(_SRVHeap));
        _CL->Dispatch(HGX, HGY, 1);

        Transition(_CL, Radiance.Get(), RadianceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, RayData.Get(),  RayDataState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, RayMotion.Get(), RayMotionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (_Profiler) _Profiler->End(_CL);

        if (_Profiler) _Profiler->Begin(_CL, "Resolve half para full");
        Transition(_CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, ResolvedMotion.Get(), ResolvedMotionState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ResolvePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ResolveTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ResolvedUAVTable));
        _CL->Dispatch(FGX, FGY, 1);
        if (_Profiler) _Profiler->End(_CL);

        // Mirror full-res: traca os pixels quase-espelho em resolucao cheia e sobrescreve o
        // Resolved (cromado fino fica nitido/estavel, sem o shimmer do half-res). Reusa as 8 SRVs
        // do trace e o UAV do Resolved. O UAV barrier garante que o resolve terminou antes.
        if (_Profiler) _Profiler->Begin(_CL, "Mirror full-res");
        {
            D3D12_RESOURCE_BARRIER UB[2]{};
            UB[0].Type = UB[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UB[0].UAV.pResource = Resolved.Get();
            UB[1].UAV.pResource = ResolvedMotion.Get();
            _CL->ResourceBarrier(2, UB);
        }
        TraceMirrorPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ResolvedUAVTable));
        _CL->Dispatch(FGX, FGY, 1);
        if (_Profiler) _Profiler->End(_CL);

        // NRD/RR: para no Resolved (radiancia crua + hitDist). O denoise fica a cargo do NRD
        // (RecordNrdPack -> Nrd.Denoise) ou do DLSS Ray Reconstruction (RawSpec: o composite le o
        // Resolved cru). Deixa o Resolved legivel por compute (NON_PIXEL) e pula Temporal/Spatial.
        if (UseNrd || RawSpec) {
            Transition(_CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, ResolvedMotion.Get(), ResolvedMotionState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CurrParity = FrameParity;
            FrameParity ^= 1u;
            return;
        }

        const u32 curr = FrameParity, prev = 1u - FrameParity;
        CurrParity = curr;

        if (NeedsHistoryClear) {
            const float Zero[4] = { 0, 0, 0, 0 };
            for (u32 i = 0; i < 2; ++i) {
                Transition(_CL, History[i].Get(), HistoryState[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(HistoryUAVSlot[i]),
                                                   _SRVHeap.CpuHandleStaging(HistoryUAVSlot[i]),
                                                   History[i].Get(), Zero, 0, nullptr);
            }
            NeedsHistoryClear = false;
        }

        if (_Profiler) _Profiler->Begin(_CL, "Acumulacao temporal");
        Transition(_CL, Resolved.Get(),       ResolvedState,      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ResolvedMotion.Get(), ResolvedMotionState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, History[prev].Get(),  HistoryState[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, History[curr].Get(),  HistoryState[curr], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TemporalPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TemporalTable[curr]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(HistoryUAVSlot[curr]));
        _CL->Dispatch(FGX, FGY, 1);
        if (_Profiler) _Profiler->End(_CL);

        // Denoise espacial pos-temporal: History[curr] (acumulado) -> Denoised. Limpa as bordas
        // (acumulacao baixa) sem borrar o interior convergido. O History[curr] segue intacto p/
        // virar History[prev] no proximo frame (sem feedback do blur na acumulacao).
        if (_Profiler) _Profiler->Begin(_CL, "Filtro espacial");
        Transition(_CL, History[curr].Get(), HistoryState[curr], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Denoised.Get(),      DenoisedState,      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpatialPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpatialTable[curr]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DenoisedUAVSlot));
        _CL->Dispatch(FGX, FGY, 1);

        Transition(_CL, Denoised.Get(), DenoisedState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (_Profiler) _Profiler->End(_CL);
        FrameParity ^= 1u;
    }

    void FReflections::RecordWaterTrace(ID3D12GraphicsCommandList* _CL,
                                         FTextureSRVHeap& _SRVHeap,
                                         FGpuProfiler* _Profiler) {
        if (!Ready) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;

        if (_Profiler) _Profiler->Begin(_CL, "Agua: trace DXR");
        Transition(_CL, WaterResolved.Get(), WaterResolvedState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, WaterMotion.Get(), WaterMotionState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        WaterTracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(WaterTraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(WaterTraceUAVTable));
        _CL->Dispatch(GX, GY, 1);
        const D3D12_RESOURCE_STATES WaterResolvedRead = RawSpec
            ? (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Transition(_CL, WaterResolved.Get(), WaterResolvedState, WaterResolvedRead);
        Transition(_CL, WaterMotion.Get(), WaterMotionState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (_Profiler) _Profiler->End(_CL);

        WaterUseRaw = RawSpec;
        if (WaterUseRaw) {
            // O RR recebe o sinal estocastico cru e aplica seu proprio historico neural.
            // Limpar ao voltar ao caminho comum evita reutilizar history envelhecido.
            NeedsWaterHistoryClear = true;
            return;
        }

        const u32 curr = WaterFrameParity, prev = 1u - curr;
        WaterCurrParity = curr;
        if (NeedsWaterHistoryClear) {
            const float Zero[4] = { 0, 0, 0, 0 };
            for (u32 i = 0; i < 2; ++i) {
                Transition(_CL, WaterHistory[i].Get(), WaterHistoryState[i],
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(WaterHistoryUAVSlot[i]),
                                                   _SRVHeap.CpuHandleStaging(WaterHistoryUAVSlot[i]),
                                                   WaterHistory[i].Get(), Zero, 0, nullptr);
            }
            NeedsWaterHistoryClear = false;
        }

        if (_Profiler) _Profiler->Begin(_CL, "Agua: historico de reflexo");
        Transition(_CL, WaterHistory[prev].Get(), WaterHistoryState[prev],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, WaterHistory[curr].Get(), WaterHistoryState[curr],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        WaterTemporalPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(WaterTemporalTable[curr]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(WaterHistoryUAVSlot[curr]));
        _CL->Dispatch(GX, GY, 1);
        Transition(_CL, WaterHistory[curr].Get(), WaterHistoryState[curr],
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (_Profiler) _Profiler->End(_CL);
        WaterFrameParity ^= 1u;
    }

    void FReflections::RecordNrdPack(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || SpecPackUAVSlot == kInvalidSlot) return;
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        NrdPackPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpecPackSrvTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(SpecPackUAVSlot));
        _CL->Dispatch(GX, GY, 1);
    }

    void FReflections::RecordNrdSpecZero(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (SpecPackUAVSlot == kInvalidSlot || !NrdInSpec) return;
        // Radiancia 0 + hitT 0 = "sem sinal especular" valido pro RELAX (vs. lixo indefinido).
        // Ordenacao: TransitionInputsToWrite (antes) e a transition UAV->SRV do proprio NRD
        // (depois) ja serializam o clear contra leituras — sem UAV barrier extra.
        const float Zero[4] = { 0, 0, 0, 0 };
        _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(SpecPackUAVSlot),
                                           _SRVHeap.CpuHandleStaging(SpecPackUAVSlot),
                                           NrdInSpec, Zero, 0, nullptr);
    }

    void FReflections::RecordComposite(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap,
                                       D3D12_CPU_DESCRIPTOR_HANDLE _HdrRtv, u32 _Width, u32 _Height) {
        if (!Ready) return;
        _CL->OMSetRenderTargets(1, &_HdrRtv, FALSE, nullptr);
        D3D12_VIEWPORT VP{ 0.0f, 0.0f, (f32)_Width, (f32)_Height, 0.0f, 1.0f };
        D3D12_RECT     SR{ 0, 0, (LONG)_Width, (LONG)_Height };
        _CL->RSSetViewports(1, &VP);
        _CL->RSSetScissorRects(1, &SR);
        _CL->SetGraphicsRootSignature(CompositeRS.Get());
        _CL->SetPipelineState(CompositePSO.Get());
        _CL->SetGraphicsRootConstantBufferView(0, CBAddr());
        u32 CompTable;
        if (RawSpec && CompositeTableRaw != kInvalidSlot) {
            // RR: compoe o Resolved CRU. Veio NON_PIXEL do RecordTrace (p/ o compute do specHitDist);
            // aqui e um PS -> transiciona p/ PIXEL. O caller ja rodou o extract do specHitDist antes.
            Transition(_CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            CompTable = CompositeTableRaw;
        } else {
            CompTable = (UseNrd && CompositeTableNrd[CurrParity] != kInvalidSlot)
                      ? CompositeTableNrd[CurrParity] : CompositeTable[CurrParity];
        }
        _CL->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(CompTable));
        _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CL->DrawInstanced(3, 1, 0, 0);
    }

    void FReflections::RecordWaterComposite(ID3D12GraphicsCommandList* _CL,
                                             FTextureSRVHeap& _SRVHeap,
                                             D3D12_CPU_DESCRIPTOR_HANDLE _HdrRtv,
                                             u32 _Width, u32 _Height) {
        const u32 Table = WaterUseRaw ? WaterCompositeRawTable
                                      : WaterCompositeTable[WaterCurrParity];
        if (!Ready || Table == kInvalidSlot) return;
        _CL->OMSetRenderTargets(1, &_HdrRtv, FALSE, nullptr);
        D3D12_VIEWPORT VP{ 0.0f, 0.0f, (f32)_Width, (f32)_Height, 0.0f, 1.0f };
        D3D12_RECT SR{ 0, 0, (LONG)_Width, (LONG)_Height };
        _CL->RSSetViewports(1, &VP);
        _CL->RSSetScissorRects(1, &SR);
        _CL->SetGraphicsRootSignature(CompositeRS.Get());
        _CL->SetPipelineState(WaterCompositePSO.Get());
        _CL->SetGraphicsRootConstantBufferView(0, CBAddr());
        _CL->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(Table));
        _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CL->DrawInstanced(3, 1, 0, 0);
    }

    FPassShaderStems FReflections::ShaderStems() const {
        static const char* const kStems[] = { "ReflectionTrace.cs", "ReflectionTraceTimed.cs",
                                              "ReflectionTraceMirror.cs", "ReflectionResolve.cs",
                                              "ReflectionTemporal.cs", "ReflectionSpatial.cs",
                                              "ReflectionNrdPack.cs", "ReflectionComposite.ps",
                                              "WaterReflectionTrace.cs", "WaterReflectionTemporal.cs",
                                              "WaterReflectionComposite.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FReflections::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Ready) RecreatePipelines(_Ctx.Device);
    }

}
