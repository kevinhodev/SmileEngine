#include "Smile/Graphics/DDGIDebug.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/DDGI.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Graphics/Barriers.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr u32 kSphereRings = 8;
        constexpr u32 kSphereSegs  = 12;
        constexpr u32 kSphereVerts = kSphereRings * kSphereSegs * 6;
        constexpr u32 kVolumeVerts = 24; 
    }

    void FDDGIDebug::BuildRootSignature(ID3D12Device* _Device) {
        constexpr u32 kNumSRV = 6;
        D3D12_DESCRIPTOR_RANGE Ranges[kNumSRV]{};
        for (u32 i = 0; i < kNumSRV; ++i) {
            Ranges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            Ranges[i].NumDescriptors                    = 1;
            Ranges[i].BaseShaderRegister                = i; 
            Ranges[i].RegisterSpace                     = 0;
            Ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }

        D3D12_ROOT_PARAMETER RootParams[1 + kNumSRV]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; 
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        for (u32 i = 0; i < kNumSRV; ++i) {
            RootParams[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            RootParams[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            RootParams[1 + i].DescriptorTable.pDescriptorRanges   = &Ranges[i];
            RootParams[1 + i].ShaderVisibility = (i == 2 || i == 3) ? D3D12_SHADER_VISIBILITY_PIXEL
                                                                    : D3D12_SHADER_VISIBILITY_VERTEX;
        }

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.MaxAnisotropy    = 1;
        Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Sampler.MinLOD           = 0.0f;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ShaderRegister   = 0; 
        Sampler.RegisterSpace    = 0;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("DDGIDebug root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));
    }

    void FDDGIDebug::BuildPSOs(ID3D12Device* _Device,
                               DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        auto ProbeVS  = LoadShaderBytecode("DDGIDebugProbes.vs_6_0.cso");
        auto ProbePS  = LoadShaderBytecode("DDGIDebugProbes.ps_6_0.cso");
        auto VolumeVS = LoadShaderBytecode("DDGIDebugVolume.vs_6_0.cso");
        auto VolumePS = LoadShaderBytecode("DDGIDebugVolume.ps_6_0.cso");
        auto RaysVS   = LoadShaderBytecode("DDGIDebugRays.vs_6_0.cso");
        auto RaysPS   = LoadShaderBytecode("DDGIDebugRays.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode              = D3D12_FILL_MODE_SOLID;
        Raster.CullMode              = D3D12_CULL_MODE_NONE; 
        Raster.FrontCounterClockwise = FALSE;
        Raster.DepthClipEnable       = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Depth.DepthFunc      = kDepthFuncLess;
        Depth.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = RootSignature.Get();
        Desc.VS                    = { ProbeVS.data(), ProbeVS.size() };
        Desc.PS                    = { ProbePS.data(), ProbePS.size() };
        Desc.BlendState            = Blend;
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.DepthStencilState     = Depth;
        Desc.InputLayout           = { nullptr, 0 };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.NumRenderTargets      = 1;
        Desc.RTVFormats[0]         = _RTFormat;
        Desc.DSVFormat             = _DSFormat;
        Desc.SampleDesc            = { 1, 0 };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&ProbePSO)));

        Desc.VS                    = { VolumeVS.data(), VolumeVS.size() };
        Desc.PS                    = { VolumePS.data(), VolumePS.size() };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        Depth.DepthWriteMask       = D3D12_DEPTH_WRITE_MASK_ZERO;
        Desc.DepthStencilState     = Depth;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&VolumePSO)));

        Desc.VS                    = { RaysVS.data(), RaysVS.size() };
        Desc.PS                    = { RaysPS.data(), RaysPS.size() };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Depth.DepthEnable          = FALSE; 
        Desc.DepthStencilState     = Depth;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&RaysPSO)));
    }

    void FDDGIDebug::Initialize(ID3D12Device* _Device,
                                DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildRootSignature(_Device);
        BuildPSOs(_Device, _RTFormat, _DSFormat);

        StatsPSO.Initialize(_Device, "DDGIDebugStats.cs_6_0.cso", 1, 1);
        PointDiagnosticPSO.Initialize(_Device, "DDGIDebugPoint.cs_6_0.cso", 5, 1);

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(DDGIDebugConstants), FCommandQueue::kFramesInFlight);
        CB            = Upload.Resource;
        MappedCBBase  = Upload.Mapped;
        CreatePointDiagnosticResources(_Device);
    }

    void FDDGIDebug::Recreate(ID3D12Device* _Device,
                              DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildPSOs(_Device, _RTFormat, _DSFormat);
        StatsPSO = FComputePipeline{};
        PointDiagnosticPSO = FComputePipeline{};
        StatsPSO.Initialize(_Device, "DDGIDebugStats.cs_6_0.cso", 1, 1);
        PointDiagnosticPSO.Initialize(
            _Device, "DDGIDebugPoint.cs_6_0.cso", 5, 1);
    }

    void FDDGIDebug::SetupForScene(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, u32 _NumProbes) {
        CancelPointDiagnostic();
        PointInputsReady = false;
        auto FreeSlot = [&](u32& Slot) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, 1); Slot = kInvalidSlot; }
        };
        FreeSlot(ProbeStatsSRVSlot);
        FreeSlot(ProbeStatsUAVSlot);
        ProbeStatsBuf.Reset();
        ProbeStatsState = D3D12_RESOURCE_STATE_COMMON;
        NumProbes = _NumProbes;
        if (_NumProbes == 0) return;

        ProbeStatsBuf = GpuResources::CreateBuffer(
            _Device, static_cast<u64>(_NumProbes) * sizeof(Vec4),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
            EVramCategory::GI, "DDGI · stats de probe");

        ProbeStatsSRVSlot = _SRVHeap.Allocate(1);
        ProbeStatsUAVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                     = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Srv.Buffer.FirstElement        = 0;
        Srv.Buffer.NumElements         = _NumProbes;
        Srv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeStatsBuf.Get(), Srv, ProbeStatsSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Uav.Buffer.FirstElement = 0;
        Uav.Buffer.NumElements  = _NumProbes;
        _SRVHeap.CreateUAV(_Device, ProbeStatsBuf.Get(), Uav, ProbeStatsUAVSlot);
    }

    void FDDGIDebug::CreatePointDiagnosticResources(ID3D12Device* _Device) {
        static_assert(FCommandQueue::kFramesInFlight == 2,
                      "Atualize os arrays de readback do diagnostico DDGI");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(PointDiagnosticConstants), FCommandQueue::kFramesInFlight);
        PointDiagnosticCB       = Upload.Resource;
        PointDiagnosticMappedCB = Upload.Mapped;

        // O readback tem o tamanho da SAIDA (kPointOutputRows x Vec4), nao o do CB — antes os
        // dois compartilhavam o mesmo Desc e a distincao vinha de um `Desc.Width =` no meio.
        const u64 OutputBytes = static_cast<u64>(kPointOutputRows) * sizeof(Vec4);
        PointDiagnosticOutput = GpuResources::CreateBuffer(
            _Device, OutputBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI,
            "DDGI · diagnostico de ponto");
        PointOutputState = D3D12_RESOURCE_STATE_COMMON;

        for (u32 I = 0; I < FCommandQueue::kFramesInFlight; ++I) {
            PointDiagnosticReadback[I] = GpuResources::CreateReadbackBuffer(_Device, OutputBytes);
            PointReadbackPending[I] = false;
            PointReadbackVersion[I] = 0;
        }
    }

    void FDDGIDebug::SetupPointDiagnosticInputs(
            ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
            u32 _GBufferTableStart, const FDDGI& _DDGI) {
        PointInputsReady = false;
        if (!_DDGI.IsReady() || _GBufferTableStart == kInvalidSlot ||
            _DDGI.IrradianceAtlasSRV() == kInvalidSlot ||
            _DDGI.DistAtlasSRV() == kInvalidSlot ||
            _DDGI.ProbeDataSRV() == kInvalidSlot ||
            !PointDiagnosticOutput) {
            return;
        }

        if (PointInputTableStart == kInvalidSlot)
            PointInputTableStart = _SRVHeap.Allocate(5);
        if (PointOutputUAVSlot == kInvalidSlot)
            PointOutputUAVSlot = _SRVHeap.Allocate(1);

        const D3D12_CPU_DESCRIPTOR_HANDLE Sources[5] = {
            _SRVHeap.CpuHandleStaging(_GBufferTableStart + 1u), // normal oct
            _SRVHeap.CpuHandleStaging(_GBufferTableStart + 3u), // depth
            _SRVHeap.CpuHandleStaging(_DDGI.IrradianceAtlasSRV()),
            _SRVHeap.CpuHandleStaging(_DDGI.DistAtlasSRV()),
            _SRVHeap.CpuHandleStaging(_DDGI.ProbeDataSRV()),
        };
        UINT SourceCounts[5] = { 1, 1, 1, 1, 1 };
        D3D12_CPU_DESCRIPTOR_HANDLE Destination =
            _SRVHeap.CpuHandle(PointInputTableStart);
        UINT DestinationCount = 5;
        _Device->CopyDescriptors(
            1, &Destination, &DestinationCount,
            5, Sources, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Uav.Buffer.FirstElement = 0;
        Uav.Buffer.NumElements  = kPointOutputRows;
        _SRVHeap.CreateUAV(
            _Device, PointDiagnosticOutput.Get(), Uav, PointOutputUAVSlot);
        PointInputsReady = true;
    }

    bool FDDGIDebug::RequestPointDiagnostic(u32 _X, u32 _Y) {
        if (!PointInputsReady || PointRequestPending) return false;
        PointRequestX = _X;
        PointRequestY = _Y;
        PointHasLastRequest = true;
        PointRequestPending = true;
        PointResultReady = false;
        ++PointRequestVersion;
        return true;
    }

    // Re-executa o ULTIMO ponto pedido. O diagnostico e one-shot por clique, entao mudar um knob
    // que altera o peso das probes deixava o painel exibindo o snapshot anterior — dois estados
    // diferentes com numeros identicos, que e o modo de falha mais caro possivel numa ferramenta
    // de auditoria (parece que o knob nao funciona). Chamado pelos setters de amostragem do GI.
    bool FDDGIDebug::RepeatPointDiagnostic() {
        if (!PointInputsReady || !PointHasLastRequest || PointRequestPending) return false;
        PointRequestPending = true;
        PointResultReady = false;
        ++PointRequestVersion;
        return true;
    }

    void FDDGIDebug::CancelPointDiagnostic() {
        PointRequestPending = false;
        PointHasLastRequest = false;
        PointResultReady = false;
        PointResult = {};
        ++PointRequestVersion;
    }

    void FDDGIDebug::RecordPointDiagnostic(
            u32 _FrameSlot, ID3D12GraphicsCommandList* _CL,
            FTextureSRVHeap& _SRVHeap, const FDDGI& _DDGI,
            const Mat44& _InvViewProj, const Vec3& _CameraPos,
            u32 _Width, u32 _Height, u32 _GIFlags) {
        if (!PointRequestPending || !PointInputsReady || !_DDGI.IsReady() ||
            _FrameSlot >= FCommandQueue::kFramesInFlight ||
            _Width == 0 || _Height == 0) {
            return;
        }

        PointDiagnosticConstants* C =
            reinterpret_cast<PointDiagnosticConstants*>(
                PointDiagnosticMappedCB +
                static_cast<size_t>(_FrameSlot) * sizeof(PointDiagnosticConstants));
        const Vec3 GridMin = _DDGI.GridMin();
        const Vec3 GridCount = _DDGI.GridCount();
        C->InvViewProj = _InvViewProj;
        C->GridMinSpacing = {
            GridMin.X, GridMin.Y, GridMin.Z, _DDGI.Spacing()
        };
        C->GridCount = {
            GridCount.X, GridCount.Y, GridCount.Z,
            static_cast<f32>(_DDGI.NumProbesCount())
        };
        C->AtlasParams = {
            _DDGI.TileSizeF(), _DDGI.AtlasW(), _DDGI.AtlasH(), 0.0f
        };
        C->DistAtlasParams = {
            _DDGI.DistTileSizeF(), _DDGI.DistAtlasW(), _DDGI.DistAtlasH(),
            _DDGI.DistanceMomentMax()
        };
        C->CameraPositionFlags = {
            _CameraPos.X, _CameraPos.Y, _CameraPos.Z, static_cast<f32>(_GIFlags)
        };
        C->PixelParams = {
            static_cast<f32>(std::min(PointRequestX, _Width - 1u)),
            static_cast<f32>(std::min(PointRequestY, _Height - 1u)),
            static_cast<f32>(_Width), static_cast<f32>(_Height)
        };
        // Os mesmos knobs que o deferred usa — o diagnostico tem que pesar as probes com o
        // bias real, senao volta a relatar numeros de outro gather.
        C->BiasParams = {
            _DDGI.GetSurfaceBiasScale(), _DDGI.GetSurfaceBiasMax(),
            _DDGI.GetVolumeFadeProbes(), 0.0f
        };

        if (PointOutputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER ToUAV{};
            ToUAV.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ToUAV.Transition.pResource   = PointDiagnosticOutput.Get();
            ToUAV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ToUAV.Transition.StateBefore = PointOutputState;
            ToUAV.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            _CL->ResourceBarrier(1, &ToUAV);
            PointOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        PointDiagnosticPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(
            0, PointDiagnosticCB->GetGPUVirtualAddress() +
               static_cast<UINT64>(_FrameSlot) * sizeof(PointDiagnosticConstants));
        _CL->SetComputeRootDescriptorTable(
            1, _SRVHeap.GpuHandle(PointInputTableStart));
        _CL->SetComputeRootDescriptorTable(
            2, _SRVHeap.GpuHandle(PointOutputUAVSlot));
        _CL->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER ToCopy{};
        ToCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ToCopy.Transition.pResource   = PointDiagnosticOutput.Get();
        ToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        _CL->ResourceBarrier(1, &ToCopy);
        PointOutputState = D3D12_RESOURCE_STATE_COPY_SOURCE;

        _CL->CopyBufferRegion(
            PointDiagnosticReadback[_FrameSlot].Get(), 0,
            PointDiagnosticOutput.Get(), 0,
            static_cast<UINT64>(kPointOutputRows) * sizeof(Vec4));
        PointReadbackPending[_FrameSlot] = true;
        PointReadbackVersion[_FrameSlot] = PointRequestVersion;
        PointRequestPending = false;
    }

    void FDDGIDebug::CollectPointDiagnostic(u32 _FrameSlot) {
        if (_FrameSlot >= FCommandQueue::kFramesInFlight ||
            !PointReadbackPending[_FrameSlot]) {
            return;
        }
        PointReadbackPending[_FrameSlot] = false;
        if (PointReadbackVersion[_FrameSlot] != PointRequestVersion) return;

        void* Mapped = nullptr;
        const SIZE_T Bytes = static_cast<SIZE_T>(kPointOutputRows) * sizeof(Vec4);
        D3D12_RANGE ReadRange{ 0, Bytes };
        SMILE_HR(PointDiagnosticReadback[_FrameSlot]->Map(
            0, &ReadRange, &Mapped));
        const Vec4* Rows = static_cast<const Vec4*>(Mapped);

        FDDGIPointDiagnostic Result{};
        Result.Valid = Rows[0].W > 0.5f;
        if (Result.Valid) {
            Result.WorldPosition = { Rows[0].X, Rows[0].Y, Rows[0].Z };
            Result.WorldNormal   = { Rows[1].X, Rows[1].Y, Rows[1].Z };
            Result.TotalWeight   = Rows[1].W;
            Result.VolumeWeight  = Rows[kPointRowVolumeIdx].X;
            f32 BestWeight = -1.0f;
            // Abaixo deste limiar a perda de visibilidade e residual; nao destaque
            // uma probe como "risco" apenas porque ela foi a maior entre oito zeros.
            f32 BestRisk = 0.005f;
            for (u32 I = 0; I < kPointProbeCount; ++I) {
                const Vec4& A = Rows[2u + I * 3u];
                const Vec4& B = Rows[3u + I * 3u];
                const Vec4& C = Rows[4u + I * 3u];
                FDDGIPointProbeDiagnostic& P = Result.Probes[I];
                P.Active = A.X >= 0.0f && B.Z >= 0.0f;
                const f32 IndexValue = P.Active ? A.X : (-A.X - 1.0f);
                P.ProbeIndex = static_cast<u32>(
                    std::max(0.0f, std::round(IndexValue)));
                P.DistanceToPoint    = A.Y;
                P.MeanDistance       = A.Z;
                P.DistanceDeviation  = A.W;
                P.TrilinearWeight    = B.X;
                P.Visibility         = B.Y;
                P.RawWeight          = P.Active ? B.Z : 0.0f;
                P.NormalizedWeight   = P.Active ? B.W : 0.0f;
                P.Irradiance         = { C.X, C.Y, C.Z };
                P.LeakRisk           = P.Active ? C.W : 0.0f;
                if (P.Active && P.NormalizedWeight > BestWeight) {
                    BestWeight = P.NormalizedWeight;
                    Result.DominantSlot = static_cast<i32>(I);
                }
                if (P.Active && P.LeakRisk > BestRisk) {
                    BestRisk = P.LeakRisk;
                    Result.RiskSlot = static_cast<i32>(I);
                }
            }
        }

        D3D12_RANGE NoWrite{ 0, 0 };
        PointDiagnosticReadback[_FrameSlot]->Unmap(0, &NoWrite);
        PointResult = Result;
        PointResultReady = true;
    }

    bool FDDGIDebug::ConsumePointDiagnostic(
            FDDGIPointDiagnostic& _OutDiagnostic) {
        if (!PointResultReady) return false;
        _OutDiagnostic = PointResult;
        PointResultReady = false;
        return true;
    }

    void FDDGIDebug::SetSelectedProbe(i32 _Probe) {
        SelectedProbes.fill(-1);
        SelectedWeights.fill(0.0f);
        SelectedRiskSlot = -1;
        FocusedProbe = _Probe;
        if (_Probe >= 0) {
            SelectedProbes[0] = _Probe;
            SelectedWeights[0] = 1.0f;
            SelectedProbeCount = 1;
        } else {
            SelectedProbeCount = 0;
        }
    }

    void FDDGIDebug::SetSelectedProbes(
            const u32* _ProbeIndices, const f32* _Weights,
            u32 _Count, i32 _RiskSlot) {
        SelectedProbes.fill(-1);
        SelectedWeights.fill(0.0f);
        SelectedProbeCount = std::min(_Count, kPointProbeCount);
        for (u32 I = 0; I < SelectedProbeCount; ++I) {
            SelectedProbes[I] = static_cast<i32>(_ProbeIndices[I]);
            SelectedWeights[I] = _Weights ? std::max(_Weights[I], 0.0f) : 1.0f;
        }
        const bool FocusStillSelected = std::find(
            SelectedProbes.begin(),
            SelectedProbes.begin() + SelectedProbeCount,
            FocusedProbe) != SelectedProbes.begin() + SelectedProbeCount;
        if (!FocusStillSelected) {
            FocusedProbe = SelectedProbeCount > 0 ? SelectedProbes[0] : -1;
        }
        SelectedRiskSlot = _RiskSlot >= 0 &&
                           static_cast<u32>(_RiskSlot) < SelectedProbeCount
                         ? _RiskSlot : -1;
    }

    void FDDGIDebug::Render(u32 _FrameSlot, ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap,
                            FDDGI& _DDGI, const Mat44& _ViewProj, const Vec3& _CameraPos, u32 _FrameIndex) {
        if (!Enabled || !_DDGI.IsReady() || NumProbes == 0) return;

        DDGIDebugConstants* C = reinterpret_cast<DDGIDebugConstants*>(
            MappedCBBase + static_cast<size_t>(_FrameSlot) * sizeof(DDGIDebugConstants));
        const Vec3 GMin = _DDGI.GridMin();
        const Vec3 GCnt = _DDGI.GridCount();
        C->ViewProj        = _ViewProj;
        C->GridMinSpacing  = { GMin.X, GMin.Y, GMin.Z, _DDGI.Spacing() };
        C->GridCount       = { GCnt.X, GCnt.Y, GCnt.Z, (f32)_DDGI.NumProbesCount() };
        C->AtlasParams     = { _DDGI.TileSizeF(), _DDGI.AtlasW(), _DDGI.AtlasH(), 0.0f };
        C->DistAtlasParams = { _DDGI.DistTileSizeF(), _DDGI.DistAtlasW(), _DDGI.DistAtlasH(),
                               _DDGI.DistanceMomentMax() };
        C->DebugParams     = { (f32)Mode, ProbeRadius, _DDGI.Spacing() * 0.45f,
                               _DDGI.GetDeactivationThreshold() }; 
        C->CameraPos       = {
            _CameraPos.X, _CameraPos.Y, _CameraPos.Z,
            static_cast<f32>(FocusedProbe)
        };
        C->RayParams       = { (f32)_FrameIndex, RayRadius,
                               static_cast<f32>(SelectedProbeCount),
                               static_cast<f32>(SelectedRiskSlot) };
        C->SelectedIndices0 = {
            static_cast<f32>(SelectedProbes[0]), static_cast<f32>(SelectedProbes[1]),
            static_cast<f32>(SelectedProbes[2]), static_cast<f32>(SelectedProbes[3])
        };
        C->SelectedIndices1 = {
            static_cast<f32>(SelectedProbes[4]), static_cast<f32>(SelectedProbes[5]),
            static_cast<f32>(SelectedProbes[6]), static_cast<f32>(SelectedProbes[7])
        };
        C->SelectedWeights0 = {
            SelectedWeights[0], SelectedWeights[1],
            SelectedWeights[2], SelectedWeights[3]
        };
        C->SelectedWeights1 = {
            SelectedWeights[4], SelectedWeights[5],
            SelectedWeights[6], SelectedWeights[7]
        };
        const D3D12_GPU_VIRTUAL_ADDRESS CBAddr =
            CB->GetGPUVirtualAddress() + static_cast<UINT64>(_FrameSlot) * sizeof(DDGIDebugConstants);

        auto Transition = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES& State,
                              D3D12_RESOURCE_STATES After) {
            TransitionResource(_CL, R, State, After);
            State = After;
        };
        // Stability e um retrato do trace ATUAL. ProbeData.w so e atualizado durante a
        // fase transiente de relocation e fica obsoleto depois; por isso este modo calcula
        // as estatisticas sob demanda a partir de ProbesTrace.
        if (Mode == EMode::Stability) {
            Transition(ProbeStatsBuf.Get(), ProbeStatsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            StatsPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr);
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(_DDGI.ProbesTraceSRV()));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbeStatsUAVSlot));
            _CL->Dispatch((NumProbes + 63) / 64, 1, 1);
            Transition(ProbeStatsBuf.Get(), ProbeStatsState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        _CL->SetGraphicsRootSignature(RootSignature.Get());
        _CL->SetPipelineState(ProbePSO.Get());
        _CL->SetGraphicsRootConstantBufferView(0, CBAddr);
        _CL->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_DDGI.ProbeDataSRV()));
        _CL->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbeStatsSRVSlot));
        _CL->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_DDGI.IrradianceAtlasSRV()));
        _CL->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(_DDGI.DistAtlasSRV()));
        _CL->SetGraphicsRootDescriptorTable(5, _SRVHeap.GpuHandle(_DDGI.ProbesTraceSRV()));   
        _CL->SetGraphicsRootDescriptorTable(6, _SRVHeap.GpuHandle(_DDGI.ProbeRayCountSRV())); 
        _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CL->IASetVertexBuffers(0, 0, nullptr);
        _CL->IASetIndexBuffer(nullptr);
        // O modo Selected manda mesmo com contagem ZERO: "nenhuma probe selecionada" tem que
        // desenhar NENHUMA, e nao o grid inteiro. Com o `SelectedProbeCount > 0` que existia
        // aqui, limpar a selecao (ponto fora do volume de sondas, onde nenhuma contribui) caia
        // no ramo de baixo e despejava as ~4,4 mil esferas do volume na tela.
        const bool SelectedMode = Mode == EMode::Selected;
        const u32 InstanceCount = SelectedMode ? SelectedProbeCount : NumProbes;
        _CL->DrawInstanced(kSphereVerts, InstanceCount, 0, 0);

        if (ShowVolume) {
            _CL->SetPipelineState(VolumePSO.Get());
            _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            _CL->DrawInstanced(kVolumeVerts, 1, 0, 0);
        }

        if (ShowRays) {
            _CL->SetPipelineState(RaysPSO.Get());
            _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // O inspector desliga rays. Nos demais modos, desenha o volume inteiro como antes.
            _CL->DrawInstanced(6, SelectedMode ? 0u : NumProbes, 0, 0);
        }
    }

    FPassShaderStems FDDGIDebug::ShaderStems() const {
        static const char* const kStems[] = { "DDGIDebugProbes.vs", "DDGIDebugProbes.ps", "DDGIDebugVolume.vs", "DDGIDebugVolume.ps", "DDGIDebugRays.vs", "DDGIDebugRays.ps", "DDGIDebugStats.cs", "DDGIDebugPoint.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FDDGIDebug::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (ProbePSO) Recreate(_Ctx.Device, _Ctx.SceneColorFormat, _Ctx.SceneDepthFormat);
    }

}
