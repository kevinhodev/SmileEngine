#include "Smile/Graphics/Reflections.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kRadianceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

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
            VramTracker::Register(Tex.Get(), EVramCategory::GI);
            return Tex;
        }
    }

    void FReflections::Initialize(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "ReflectionTrace.cs_6_6.cso", 9, 2, true);       // t8 = luzes (F5)
        TraceMirrorPSO.Initialize(_Device, "ReflectionTraceMirror.cs_6_6.cso", 9, 1, true);
        ResolvePSO.Initialize(_Device, "ReflectionResolve.cs_6_0.cso", 4, 1, false);
        TemporalPSO.Initialize(_Device, "ReflectionTemporal.cs_6_0.cso", 4, 1, false);
        SpatialPSO.Initialize(_Device, "ReflectionSpatial.cs_6_0.cso", 3, 1, false);
        NrdPackPSO.Initialize(_Device, "ReflectionNrdPack.cs_6_0.cso", 3, 1, false);
        CreateCompositePipeline(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FReflections::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(ReflectionConstants);
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

    void FReflections::CreateCompositePipeline(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 5; // t4 = GBufferC (metallic pos-dieta)
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
    }

    void FReflections::ReleaseResize(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(RadianceSRVSlot, 1);
        Free(RayDataSRVSlot, 1);
        Free(ResolvedSRVSlot, 1);
        Free(ResolvedUAVSlot, 1);
        Free(HistorySRVSlot[0], 1); Free(HistorySRVSlot[1], 1);
        Free(HistoryUAVSlot[0], 1); Free(HistoryUAVSlot[1], 1);
        Free(DenoisedSRVSlot, 1); Free(DenoisedUAVSlot, 1);
        Free(TraceUAVTable, 2);
        for (u32 i = 0; i < kTraceTables; ++i) Free(TraceTable[i], 9);
        Free(ResolveTableStart, 4);
        Free(TemporalTable[0], 4); Free(TemporalTable[1], 4);
        Free(SpatialTable[0], 3); Free(SpatialTable[1], 3);
        Free(CompositeTable[0], 5); Free(CompositeTable[1], 5);
        Free(SpecPackSrvTable, 3);
        Free(SpecPackUAVSlot, 1);
        Free(NrdOutSpecSRV, 1);
        NrdInSpec = nullptr;
        Free(CompositeTableNrd[0], 5); Free(CompositeTableNrd[1], 5);
        Radiance.Reset();
        RayData.Reset();
        Resolved.Reset();
        History[0].Reset(); History[1].Reset();
        Denoised.Reset();
        RadianceState = D3D12_RESOURCE_STATE_COMMON;
        RayDataState  = D3D12_RESOURCE_STATE_COMMON;
        ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        HistoryState[0] = HistoryState[1] = D3D12_RESOURCE_STATE_COMMON;
        DenoisedState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FReflections::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                      u32 _Width, u32 _Height, u32 _TlasSlot, u32 _SkyViewSlot,
                                      u32 _InstanceSlot, u32 _IrradSlot, u32 _DepthSlot,
                                      u32 _GBufferSlot, u32 _GBufferCSlot, u32 _BRDFLutSlot) {
        if (!Initialized) return;
        ReleaseResize(_SRVHeap);
        if (_Width == 0 || _Height == 0 || _TlasSlot == kInvalidSlot || _InstanceSlot == kInvalidSlot)
            return;

        Width = _Width; Height = _Height;
        HalfWidth = (_Width + 1) / 2; HalfHeight = (_Height + 1) / 2;
        DepthSlotCached = _DepthSlot; GBufferSlotCached = _GBufferSlot; BRDFLutSlotCached = _BRDFLutSlot;
        GBufferCSlotCached = _GBufferCSlot;
        Radiance = CreateUAVTex2D(_Device, HalfWidth, HalfHeight, kRadianceFormat);
        RayData  = CreateUAVTex2D(_Device, HalfWidth, HalfHeight, kRadianceFormat);
        Resolved = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat);
        History[0] = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat);
        History[1] = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat);
        Denoised   = CreateUAVTex2D(_Device, Width, Height, kRadianceFormat);
        RadianceState = RayDataState = ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        HistoryState[0] = HistoryState[1] = D3D12_RESOURCE_STATE_COMMON;
        DenoisedState = D3D12_RESOURCE_STATE_COMMON;
        NeedsHistoryClear = true; FrameParity = 0;

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
        ResolvedSRVSlot = _SRVHeap.Allocate(1);
        ResolvedUAVSlot = _SRVHeap.Allocate(1);
        HistorySRVSlot[0] = _SRVHeap.Allocate(1); HistorySRVSlot[1] = _SRVHeap.Allocate(1);
        HistoryUAVSlot[0] = _SRVHeap.Allocate(1); HistoryUAVSlot[1] = _SRVHeap.Allocate(1);
        DenoisedSRVSlot = _SRVHeap.Allocate(1); DenoisedUAVSlot = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, Radiance.Get(), Srv, RadianceSRVSlot);
        _SRVHeap.CreateSRV(_Device, RayData.Get(),  Srv, RayDataSRVSlot);
        _SRVHeap.CreateSRV(_Device, Resolved.Get(), Srv, ResolvedSRVSlot);
        _SRVHeap.CreateUAV(_Device, Resolved.Get(), Uav, ResolvedUAVSlot);
        _SRVHeap.CreateSRV(_Device, History[0].Get(), Srv, HistorySRVSlot[0]);
        _SRVHeap.CreateSRV(_Device, History[1].Get(), Srv, HistorySRVSlot[1]);
        _SRVHeap.CreateUAV(_Device, History[0].Get(), Uav, HistoryUAVSlot[0]);
        _SRVHeap.CreateUAV(_Device, History[1].Get(), Uav, HistoryUAVSlot[1]);
        _SRVHeap.CreateSRV(_Device, Denoised.Get(), Srv, DenoisedSRVSlot);
        _SRVHeap.CreateUAV(_Device, Denoised.Get(), Uav, DenoisedUAVSlot);

        TraceUAVTable = _SRVHeap.Allocate(2);
        _SRVHeap.CreateUAV(_Device, Radiance.Get(), Uav, TraceUAVTable);
        _SRVHeap.CreateUAV(_Device, RayData.Get(),  Uav, TraceUAVTable + 1);

        // t0..t7 fixos + t8 = luzes puntuais (F5; copiado por frame no SetPunctualLightsSRV).
        // Uma tabela por frame em voo: o t8 muda todo frame e a tabela do frame anterior ainda
        // pode estar sendo lida pela GPU (descriptor versioning). t4/t5 eram os merged VB/IB,
        // aposentados pelo bindless (InstanceGeo) — filler valido p/ manter o layout.
        D3D12_CPU_DESCRIPTOR_HANDLE TSrc[8] = {
            _SRVHeap.CpuHandleStaging(_TlasSlot),
            _SRVHeap.CpuHandleStaging(_SkyViewSlot),
            _SRVHeap.CpuHandleStaging(_InstanceSlot),
            _SRVHeap.CpuHandleStaging(_IrradSlot),
            _SRVHeap.CpuHandleStaging(_InstanceSlot),
            _SRVHeap.CpuHandleStaging(_InstanceSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
        };
        UINT TDstCount = 8; UINT TSrcCounts[8] = { 1,1,1,1,1,1,1,1 };
        for (u32 i = 0; i < kTraceTables; ++i) {
            TraceTable[i] = _SRVHeap.Allocate(9);
            D3D12_CPU_DESCRIPTOR_HANDLE TDst = _SRVHeap.CpuHandle(TraceTable[i]);
            _Device->CopyDescriptors(1, &TDst, &TDstCount, 8, TSrc, TSrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        ResolveTableStart = _SRVHeap.Allocate(4);
        D3D12_CPU_DESCRIPTOR_HANDLE RDst = _SRVHeap.CpuHandle(ResolveTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE RSrc[4] = {
            _SRVHeap.CpuHandleStaging(RadianceSRVSlot),
            _SRVHeap.CpuHandleStaging(RayDataSRVSlot),
            _SRVHeap.CpuHandleStaging(_DepthSlot),
            _SRVHeap.CpuHandleStaging(_GBufferSlot),
        };
        UINT RDstCount = 4; UINT RSrcCounts[4] = { 1,1,1,1 };
        _Device->CopyDescriptors(1, &RDst, &RDstCount, 4, RSrc, RSrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        UINT Four = 4; UINT Ones[4] = { 1,1,1,1 };
        for (u32 curr = 0; curr < 2; ++curr) {
            const u32 prev = 1u - curr;
            TemporalTable[curr] = _SRVHeap.Allocate(4);
            D3D12_CPU_DESCRIPTOR_HANDLE TDst2 = _SRVHeap.CpuHandle(TemporalTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE TSrc2[4] = {
                _SRVHeap.CpuHandleStaging(ResolvedSRVSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(HistorySRVSlot[prev]),
            };
            _Device->CopyDescriptors(1, &TDst2, &Four, 4, TSrc2, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
            CompositeTable[curr] = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE CDst2 = _SRVHeap.CpuHandle(CompositeTable[curr]);
            D3D12_CPU_DESCRIPTOR_HANDLE CSrc2[5] = {
                _SRVHeap.CpuHandleStaging(DenoisedSRVSlot),
                _SRVHeap.CpuHandleStaging(_GBufferSlot),
                _SRVHeap.CpuHandleStaging(_DepthSlot),
                _SRVHeap.CpuHandleStaging(_BRDFLutSlot),
                _SRVHeap.CpuHandleStaging(_GBufferCSlot),
            };
            UINT Five = 5; UINT OnesC[5] = { 1,1,1,1,1 };
            _Device->CopyDescriptors(1, &CDst2, &Five, 5, CSrc2, OnesC, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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

        Ready = true;
    }

    void FReflections::SetupNrdSpec(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                    ID3D12Resource* _NrdInSpec, ID3D12Resource* _NrdOutSpec) {
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
        UINT Five = 5; UINT Ones[5] = { 1,1,1,1,1 };
        for (u32 i = 0; i < 2; ++i) {
            CompositeTableNrd[i] = _SRVHeap.Allocate(5);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(CompositeTableNrd[i]);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[5] = {
                _SRVHeap.CpuHandleStaging(NrdOutSpecSRV),
                _SRVHeap.CpuHandleStaging(GBufferSlotCached),
                _SRVHeap.CpuHandleStaging(DepthSlotCached),
                _SRVHeap.CpuHandleStaging(BRDFLutSlotCached),
                _SRVHeap.CpuHandleStaging(GBufferCSlotCached),
            };
            _Device->CopyDescriptors(1, &Dst, &Five, 5, Src, Ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    void FReflections::SetGIParams(const Vec3& _GridMin, f32 _Spacing, const Vec3& _GridCount,
                                   f32 _AtlasTile, f32 _AtlasW, f32 _AtlasH, f32 _MaxRayDist) {
        GIGridMinSpacing = { _GridMin.X, _GridMin.Y, _GridMin.Z, _Spacing };
        GIGridCount      = { _GridCount.X, _GridCount.Y, _GridCount.Z, 0.0f };
        GIAtlasParams    = { _AtlasTile, _AtlasW, _AtlasH, 0.0f };
        GIMaxRayDist     = _MaxRayDist;
    }

    void FReflections::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProj, const Mat44& _PrevViewProj,
                                      const Vec3& _CameraPos, u32 _Width, u32 _Height, const Vec3& _SunDir,
                                      f32 _SunIntensity, const Vec3& _SunColor, u32 _FrameIndex,
                                      f32 _SkyIntensity, f32 _ShadowRayBias, bool _RealHitShading,
                                      const Mat44& _View, u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.InvViewProj     = _InvViewProj;
        CPU.PrevViewProj    = _PrevViewProj;
        CPU.View            = _View;
        // xyz = {A,B,C} default do nrd::ReblurSettings (igual ao driver) — NAO divergir do FNrdDenoiser.
        // w = UseNrd: avisa o composite que a OUT_SPEC esta em YCoCg (desempacotar p/ linear).
        CPU.NrdSpecParams   = { 3.0f, 0.1f, 20.0f, UseNrd ? 1.0f : 0.0f };
        CPU.TemporalParams  = { Temporal ? MaxFrames : 1.0f, NeighborhoodGamma, SpatialRadius,
                                FullResMaxRough };
        CPU.DebugParams     = { (f32)DebugMode, MaxFrames, 0.0f, 0.0f }; // y = cap real p/ debug acumulacao
        // w = nº de luzes puntuais no t8 (F5) — o componente era constante 1.0, livre.
        CPU.CameraPos       = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z,
                                static_cast<f32>(_PunctualLightCount) };
        CPU.ScreenParams    = { (f32)_Width, (f32)_Height, 1.0f / (f32)_Width, 1.0f / (f32)_Height };
        CPU.ReflectParams   = { MaxRoughnessToTrace, RoughnessFadeLength,
                                _RealHitShading ? 1.0f : 0.0f, AlbedoLOD };
        CPU.GridMinSpacing  = GIGridMinSpacing;
        CPU.GridCount       = GIGridCount;
        CPU.AtlasParams     = GIAtlasParams;
        CPU.SunDirIntensity = { _SunDir.X, _SunDir.Y, _SunDir.Z, _SunIntensity };
        CPU.SunColor        = { _SunColor.X, _SunColor.Y, _SunColor.Z,
                                FoliageShadows ? 255.0f : 1.0f }; // w = ShadowRayMask
        CPU.TraceParams     = { (f32)_FrameIndex, GIMaxRayDist, _SkyIntensity, _ShadowRayBias };
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

    void FReflections::RecordTrace(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;
        const u32 HGX = (HalfWidth + 7) / 8, HGY = (HalfHeight + 7) / 8; 
        const u32 FGX = (Width + 7) / 8,     FGY = (Height + 7) / 8;     

        Transition(_CL, Radiance.Get(), RadianceState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, RayData.Get(),  RayDataState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(TraceUAVTable));
        _CL->Dispatch(HGX, HGY, 1);

        Transition(_CL, Radiance.Get(), RadianceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, RayData.Get(),  RayDataState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ResolvePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ResolveTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ResolvedUAVSlot));
        _CL->Dispatch(FGX, FGY, 1);

        // Mirror full-res: traca os pixels quase-espelho em resolucao cheia e sobrescreve o
        // Resolved (cromado fino fica nitido/estavel, sem o shimmer do half-res). Reusa as 8 SRVs
        // do trace e o UAV do Resolved. O UAV barrier garante que o resolve terminou antes.
        {
            D3D12_RESOURCE_BARRIER UB{};
            UB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UB.UAV.pResource = Resolved.Get();
            _CL->ResourceBarrier(1, &UB);
        }
        TraceMirrorPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ResolvedUAVSlot));
        _CL->Dispatch(FGX, FGY, 1);

        // NRD: para no Resolved (radiancia crua + hitDist). O denoise especular fica a cargo do NRD
        // (RecordNrdPack -> Nrd.Denoise). Deixa o Resolved legivel pelo pack (compute) e pula o
        // denoiser caseiro (Temporal/Spatial).
        if (UseNrd) {
            Transition(_CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

        Transition(_CL, Resolved.Get(),       ResolvedState,      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, History[prev].Get(),  HistoryState[prev], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, History[curr].Get(),  HistoryState[curr], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TemporalPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TemporalTable[curr]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(HistoryUAVSlot[curr]));
        _CL->Dispatch(FGX, FGY, 1);

        // Denoise espacial pos-temporal: History[curr] (acumulado) -> Denoised. Limpa as bordas
        // (acumulacao baixa) sem borrar o interior convergido. O History[curr] segue intacto p/
        // virar History[prev] no proximo frame (sem feedback do blur na acumulacao).
        Transition(_CL, History[curr].Get(), HistoryState[curr], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, Denoised.Get(),      DenoisedState,      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpatialPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpatialTable[curr]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DenoisedUAVSlot));
        _CL->Dispatch(FGX, FGY, 1);

        Transition(_CL, Denoised.Get(), DenoisedState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        FrameParity ^= 1u;
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
        // Radiancia 0 + hitT 0 = "sem sinal especular" valido pro REBLUR (vs. lixo indefinido).
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
        const u32 CompTable = (UseNrd && CompositeTableNrd[CurrParity] != kInvalidSlot)
                            ? CompositeTableNrd[CurrParity] : CompositeTable[CurrParity];
        _CL->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(CompTable));
        _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CL->DrawInstanced(3, 1, 0, 0);
    }
}
