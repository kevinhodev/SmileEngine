#include "Smile/Graphics/VolumetricClouds.h"
#include "Smile/Graphics/CloudNoise.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>

namespace Smile {

    void FVolumetricClouds::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                       FCloudNoise& _Noise, u32 _AtmoTransmittanceSRV,
                                       u32 _AtmoMultiScatterSRV, u32 _SampleCount,
                                       DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat,
                                       u32 _Width, u32 _Height) {
        if (Initialized) return;

        CPUConstants.PlanetRadii  = { 6360.0f, 6362.0f, 6365.0f, 0.0f };
        CPUConstants.CloudParams  = { 0.45f, 1.6f, 0.15f, 0.0f }; 
        CPUConstants.CloudParams2 = { 0.010f, 0.45f, 6.0f, 0.0f }; 
        CPUConstants.WindParams   = { 0.01f, 0.0f, 0.004f, 0.0f };
        CPUConstants.MarchParams  = { 128.0f, 6.0f, 0.35f, 40.0f }; 
        CPUConstants.SunColor     = { 1.0f, 1.0f, 0.96f, 0.0f };
        CPUConstants.SunDir       = { 0.0f, 0.6f, 0.8f, 6.0f };    
        CPUConstants.PhaseParams  = { 0.80f, -0.30f, 0.5f, 0.5f }; 
        CPUConstants.AtmoLink     = { 6460.0f, 3.0f, 1.0f, 0.0f }; 
        CPUConstants.InvViewProjNoTrans = Mat44::Identity();

        CreateConstantBuffer(_Device);
        CreateRT(_Device, _SRVHeap, _Width, _Height);

        RaymarchPSO.Initialize(_Device, "CloudRaymarch.cs_6_0.cso", 5, 1);
        BuildNoiseTable(_Device, _SRVHeap, _Noise, _AtmoTransmittanceSRV, _AtmoMultiScatterSRV);
        BuildCompositeRootSignature(_Device);
        BuildCompositePSO(_Device, _SampleCount, _RTFormat, _DSFormat);

        Initialized = true;
        LogInfo("Nuvens volumetricas inicializadas (B2: raymarch single-scatter + composite)");
    }

    void FVolumetricClouds::CreateRT(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _Width, u32 _Height) {
        if (_Width == 0 || _Height == 0) return;
        RTWidth = _Width; RTHeight = _Height;
        RTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CloudRT.Reset();

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, RTState, nullptr,
            IID_PPV_ARGS(&CloudRT)));

        if (RTSRVSlot == kInvalidSlot) RTSRVSlot = _SRVHeap.Allocate(1);
        if (RTUAVSlot == kInvalidSlot) RTUAVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels       = 1;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        _SRVHeap.CreateSRV(_Device, CloudRT.Get(), SRVDesc, RTSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
        UAVDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
        UAVDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
        UAVDesc.Texture2D.MipSlice = 0;
        _SRVHeap.CreateUAV(_Device, CloudRT.Get(), UAVDesc, RTUAVSlot);
    }

    void FVolumetricClouds::TransitionRT(ID3D12GraphicsCommandList* _CommandList,
                                         D3D12_RESOURCE_STATES _After) {
        if (RTState == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = CloudRT.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = RTState;
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        RTState = _After;
    }

    void FVolumetricClouds::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                   u32 _Width, u32 _Height) {
        if (!Initialized) return;
        CreateRT(_Device, _SRVHeap, _Width, _Height);
    }

    void FVolumetricClouds::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(CloudConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Ptr));
        MappedBase = reinterpret_cast<u8*>(Ptr);
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedBase + static_cast<size_t>(i) * sizeof(CloudConstants),
                        &CPUConstants, sizeof(CloudConstants));
    }

    void FVolumetricClouds::BuildNoiseTable(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                            FCloudNoise& _Noise, u32 _AtmoTransmittanceSRV,
                                            u32 _AtmoMultiScatterSRV) {
        NoiseTableStart = _SRVHeap.Allocate(5);
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(NoiseTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Srcs[5] = {
            _SRVHeap.CpuHandleStaging(_Noise.BaseNoiseSRV()),
            _SRVHeap.CpuHandleStaging(_Noise.DetailNoiseSRV()),
            _SRVHeap.CpuHandleStaging(_Noise.WeatherSRV()),
            _SRVHeap.CpuHandleStaging(_AtmoTransmittanceSRV),
            _SRVHeap.CpuHandleStaging(_AtmoMultiScatterSRV),
        };
        UINT DstCount = 5; UINT SrcCounts[5] = { 1, 1, 1, 1, 1 };
        _Device->CopyDescriptors(1, &Dst, &DstCount, 5, Srcs, SrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FVolumetricClouds::BuildCompositeRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 1; 
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParam{};
        RootParam.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParam.DescriptorTable.NumDescriptorRanges = 1;
        RootParam.DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParam.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = 1;
        Desc.pParameters       = &RootParam;
        Desc.NumStaticSamplers = 0;
        Desc.pStaticSamplers   = nullptr;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Cloud composite root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&CompositeRootSig)));
    }

    void FVolumetricClouds::BuildCompositePSO(ID3D12Device* _Device, u32 _SampleCount,
                                              DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        auto VS = LoadShaderBytecode("CloudComposite.vs_6_0.cso");
        auto PS = LoadShaderBytecode("CloudComposite.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].BlendEnable           = TRUE;
        Blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlend             = D3D12_BLEND_SRC_ALPHA;
        Blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
        Blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        Depth.DepthFunc      = kDepthFuncLessEqual; 
        Depth.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = CompositeRootSig.Get();
        Desc.VS                    = { VS.data(), VS.size() };
        Desc.PS                    = { PS.data(), PS.size() };
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

        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&CompositePSO)));
    }

    void FVolumetricClouds::RecreateComposite(ID3D12Device* _Device, u32 _SampleCount,
                                              DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        if (!Initialized) return;
        BuildCompositePSO(_Device, _SampleCount, _RTFormat, _DSFormat);
    }

    void FVolumetricClouds::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvVP, f32 _ViewHeightKm,
                                           const Vec3& _DirToSun, const Vec3& _SunColor,
                                           f32 _Time, u32 _Width, u32 _Height) {
        FrameSlot = _FrameSlot;
        Vec3 d = _DirToSun.NormalizedSafe(Vec3{ 0.0f, 0.6f, 0.8f }.Normalized());
        CPUConstants.InvViewProjNoTrans = _InvVP;
        CPUConstants.CameraPos = { 0.0f, _ViewHeightKm, 0.0f, _ViewHeightKm };
        CPUConstants.SunDir    = { d.X, d.Y, d.Z, CPUConstants.SunDir.W };
        CPUConstants.SunColor  = { _SunColor.X, _SunColor.Y, _SunColor.Z, 0.0f };
        CPUConstants.CloudParams.W = _Time;
        CPUConstants.ScreenParams  = { (f32)_Width, (f32)_Height,
                                       1.0f / (f32)_Width, 1.0f / (f32)_Height };
        if (MappedBase) *Mapped() = CPUConstants;
    }

    void FVolumetricClouds::RecordRaymarch(ID3D12GraphicsCommandList* _CommandList,
                                           FTextureSRVHeap& _SRVHeap) {
        if (!Initialized || !CloudRT) return;

        TransitionRT(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        RaymarchPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(NoiseTableStart));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(RTUAVSlot));
        _CommandList->Dispatch((RTWidth + 7) / 8, (RTHeight + 7) / 8, 1);
        TransitionRT(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void FVolumetricClouds::Composite(ID3D12GraphicsCommandList* _CommandList,
                                      FTextureSRVHeap& _SRVHeap) {
        if (!Initialized || !CloudRT) return;

        _CommandList->SetGraphicsRootSignature(CompositeRootSig.Get());
        _CommandList->SetPipelineState(CompositePSO.Get());
        _CommandList->SetGraphicsRootDescriptorTable(0, _SRVHeap.GpuHandle(RTSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
