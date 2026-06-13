#include "Smile/Graphics/DDGIDebug.h"
#include "Smile/Graphics/DDGI.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>

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

    void FDDGIDebug::BuildPSOs(ID3D12Device* _Device, u32 _SampleCount,
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
        Desc.SampleDesc            = { _SampleCount, 0 };
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

    void FDDGIDebug::Initialize(ID3D12Device* _Device, u32 _SampleCount,
                                DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildRootSignature(_Device);
        BuildPSOs(_Device, _SampleCount, _RTFormat, _DSFormat);

        StatsPSO.Initialize(_Device, "DDGIDebugStats.cs_6_0.cso", 1, 1);

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(DDGIDebugConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCBBase)));
    }

    void FDDGIDebug::Recreate(ID3D12Device* _Device, u32 _SampleCount,
                              DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildPSOs(_Device, _SampleCount, _RTFormat, _DSFormat);
    }

    void FDDGIDebug::SetupForScene(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap, u32 _NumProbes) {
        auto FreeSlot = [&](u32& Slot) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, 1); Slot = kInvalidSlot; }
        };
        FreeSlot(ProbeStatsSRVSlot);
        FreeSlot(ProbeStatsUAVSlot);
        ProbeStatsBuf.Reset();
        ProbeStatsState = D3D12_RESOURCE_STATE_COMMON;
        NumProbes = _NumProbes;
        if (_NumProbes == 0) return;

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(_NumProbes) * sizeof(Vec4);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ProbeStatsBuf)));

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
                               _DDGI.MaxRayDistance() };
        C->DebugParams     = { (f32)Mode, ProbeRadius, _DDGI.Spacing() * 0.45f,
                               _DDGI.GetDeactivationThreshold() }; 
        C->CameraPos       = { _CameraPos.X, _CameraPos.Y, _CameraPos.Z, 0.0f };
        C->RayParams       = { (f32)_FrameIndex, RayRadius, 0.0f, 0.0f };
        const D3D12_GPU_VIRTUAL_ADDRESS CBAddr =
            CB->GetGPUVirtualAddress() + static_cast<UINT64>(_FrameSlot) * sizeof(DDGIDebugConstants);

        auto Transition = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After) {
            if (State == After) return;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = R;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = State;
            B.Transition.StateAfter  = After;
            _CL->ResourceBarrier(1, &B);
            State = After;
        };
        if (Mode == EMode::Classification) {
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
        _CL->DrawInstanced(kSphereVerts, NumProbes, 0, 0);

        if (ShowVolume) {
            _CL->SetPipelineState(VolumePSO.Get());
            _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            _CL->DrawInstanced(kVolumeVerts, 1, 0, 0);
        }

        if (ShowRays) {
            _CL->SetPipelineState(RaysPSO.Get());
            _CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            _CL->DrawInstanced(6, NumProbes, 0, 0); 
        }
    }
}
