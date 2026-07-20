#include "Smile/Graphics/VolumetricClouds.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CloudNoise.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>

namespace Smile {

    void FVolumetricClouds::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                       FCloudNoise& _Noise, u32 _AtmoTransmittanceSRV,
                                       u32 _AtmoMultiScatterSRV,
                                       DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat,
                                       u32 _Width, u32 _Height) {
        if (Initialized) return;

        CPUConstants.PlanetRadii  = { 6360.0f, 6362.0f, 6365.0f, 0.0f };
        // Densidade 4/km: com 1.6 o disco do sol/lua atravessava a nuvem quase sem atenuar
        // (T alto demais) e ainda saturava no tonemap = "astro na frente da nuvem".
        CPUConstants.CloudParams  = { 0.45f, 4.0f, 0.15f, 0.0f };
        CPUConstants.CloudParams2 = { 0.010f, 0.45f, 6.0f, 0.0f }; 
        CPUConstants.WindParams   = { 0.01f, 0.0f, 0.004f, 0.0f };
        CPUConstants.MarchParams  = { 128.0f, 6.0f, 0.35f, 40.0f }; 
        CPUConstants.SunColor     = { 1.0f, 1.0f, 0.96f, 0.0f };
        CPUConstants.SunDir       = { 0.0f, 0.6f, 0.8f, 6.0f };    
        CPUConstants.PhaseParams  = { 0.80f, -0.30f, 0.5f, 0.5f }; 
        CPUConstants.AtmoLink     = { 6460.0f, 3.0f, 1.0f, 1.0f }; // w = peak variation
        CPUConstants.InvViewProjNoTrans = Mat44::Identity();
        CPUConstants.PrevVPWorld      = Mat44::Identity();
        CPUConstants.InvViewProjWorld = Mat44::Identity();
        CPUConstants.TemporalParams = { 1.0f / 16.0f, 0.0f, 0.0f, 0.0f };

        CreateConstantBuffer(_Device);
        CreateRT(_Device, _SRVHeap, _Width, _Height);

        BuildRaymarchPipeline(_Device);
        TemporalPSO.Initialize(_Device, "CloudTemporal.cs_6_0.cso", 2, 1);
        ShadowPSO.Initialize(_Device, "CloudShadowMap.cs_6_0.cso", 5, 1);

        // Shadow map das nuvens: resolucao fixa (independe da tela), criado uma vez.
        {
            D3D12_RESOURCE_DESC SDesc{};
            SDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            SDesc.Width            = kShadowRes;
            SDesc.Height           = kShadowRes;
            SDesc.DepthOrArraySize = 1;
            SDesc.MipLevels        = 1;
            SDesc.Format           = DXGI_FORMAT_R16_FLOAT;
            SDesc.SampleDesc       = { 1, 0 };
            SDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            SDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES SHeap{};
            SHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ShadowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            SMILE_HR(_Device->CreateCommittedResource(
                &SHeap, D3D12_HEAP_FLAG_NONE, &SDesc, ShadowState, nullptr,
                IID_PPV_ARGS(&ShadowTex)));
            VramTracker::Register(ShadowTex.Get(), EVramCategory::Sky);

            ShadowSRVSlot = _SRVHeap.Allocate(1);
            ShadowUAVSlot = _SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC SSrv{};
            SSrv.Format                    = DXGI_FORMAT_R16_FLOAT;
            SSrv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            SSrv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SSrv.Texture2D.MipLevels       = 1;
            _SRVHeap.CreateSRV(_Device, ShadowTex.Get(), SSrv, ShadowSRVSlot);
            D3D12_UNORDERED_ACCESS_VIEW_DESC SUav{};
            SUav.Format        = DXGI_FORMAT_R16_FLOAT;
            SUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            _SRVHeap.CreateUAV(_Device, ShadowTex.Get(), SUav, ShadowUAVSlot);
        }
        BuildNoiseTable(_Device, _SRVHeap, _Noise, _AtmoTransmittanceSRV, _AtmoMultiScatterSRV);
        BuildCompositeRootSignature(_Device);
        BuildCompositePSO(_Device, _RTFormat, _DSFormat);

        Initialized = true;
        LogInfo("Nuvens volumetricas inicializadas (B2: raymarch single-scatter + composite)");
    }

    void FVolumetricClouds::CreateRT(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _Width, u32 _Height) {
        if (_Width == 0 || _Height == 0) return;
        FullWidth = _Width; FullHeight = _Height;
        const u32 Div = HalfRes ? 2u : 1u;
        RTWidth  = (_Width  + Div - 1) / Div;
        RTHeight = (_Height + Div - 1) / Div;
        RTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CloudRT.Reset();

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = RTWidth;
        Desc.Height           = RTHeight;
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
        VramTracker::Register(CloudRT.Get(), EVramCategory::Sky);

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

        // Historico ping-pong da acumulacao temporal, mesmas dimensoes/formato do RT.
        for (u32 i = 0; i < 2; ++i) {
            HistoryRT[i].Reset();
            HistState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, HistState[i], nullptr,
                IID_PPV_ARGS(&HistoryRT[i])));
            VramTracker::Register(HistoryRT[i].Get(), EVramCategory::Sky);

            if (HistSRVSlot[i] == kInvalidSlot) HistSRVSlot[i] = _SRVHeap.Allocate(1);
            if (HistUAVSlot[i] == kInvalidSlot) HistUAVSlot[i] = _SRVHeap.Allocate(1);
            _SRVHeap.CreateSRV(_Device, HistoryRT[i].Get(), SRVDesc, HistSRVSlot[i]);
            _SRVHeap.CreateUAV(_Device, HistoryRT[i].Get(), UAVDesc, HistUAVSlot[i]);
        }

        // Tabelas contiguas [raw, hist[i]] p/ o passe temporal; recopiadas a cada resize
        // porque descriptor copiado nao acompanha a recriacao da SRV de origem.
        for (u32 i = 0; i < 2; ++i) {
            if (TemporalSRVTable[i] == kInvalidSlot) TemporalSRVTable[i] = _SRVHeap.Allocate(2);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TemporalSRVTable[i]);
            D3D12_CPU_DESCRIPTOR_HANDLE Srcs[2] = {
                _SRVHeap.CpuHandleStaging(RTSRVSlot),
                _SRVHeap.CpuHandleStaging(HistSRVSlot[i]),
            };
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Srcs, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        HistIndex    = 0;
        HistoryValid = false;
        CompositeSRVSlot = RTSRVSlot;
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

    void FVolumetricClouds::BuildRaymarchPipeline(ID3D12Device* _Device) {
        // b0 + t0-t4 (noise/weather/LUTs) + t5 (depth da cena, tabela propria com handle
        // direto do slot — nao envelhece quando o depth e recriado no resize) + u0.
        D3D12_DESCRIPTOR_RANGE NoiseRange{};
        NoiseRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        NoiseRange.NumDescriptors     = 5;
        NoiseRange.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE DepthRange{};
        DepthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DepthRange.NumDescriptors     = 1;
        DepthRange.BaseShaderRegister = 5;

        D3D12_DESCRIPTOR_RANGE UAVRange{};
        UAVRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRange.NumDescriptors     = 1;
        UAVRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER Params[4]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 0;
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1].DescriptorTable.NumDescriptorRanges = 1;
        Params[1].DescriptorTable.pDescriptorRanges   = &NoiseRange;
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &DepthRange;
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        Params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[3].DescriptorTable.NumDescriptorRanges = 1;
        Params[3].DescriptorTable.pDescriptorRanges   = &UAVRange;
        Params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].ShaderRegister   = 1;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(Params);
        Desc.pParameters       = Params;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Cloud raymarch root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RaymarchRootSig)));

        auto CS = LoadShaderBytecode("CloudRaymarch.cs_6_0.cso");
        D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature = RaymarchRootSig.Get();
        PSODesc.CS             = { CS.data(), CS.size() };
        SMILE_HR(_Device->CreateComputePipelineState(&PSODesc, IID_PPV_ARGS(&RaymarchPSOState)));
    }

    void FVolumetricClouds::SetWeatherSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                          u32 _Slot) {
        if (!Initialized) return;
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(NoiseTableStart + 2);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_Slot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        InvalidateHistory();
    }

    void FVolumetricClouds::BuildCompositeRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 1; 
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE DepthRange{};
        DepthRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DepthRange.NumDescriptors                    = 1;
        DepthRange.BaseShaderRegister                = 1;
        DepthRange.RegisterSpace                     = 0;
        DepthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[0].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        // b0: xy = 1/resolucao de saida, zw = dimensoes do RT half-res (upsample bilateral).
        RootParams[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        RootParams[1].Constants.ShaderRegister = 0;
        RootParams[1].Constants.RegisterSpace  = 0;
        RootParams[1].Constants.Num32BitValues = 4;
        RootParams[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
        // t1: depth da cena (tabela propria, handle direto do slot).
        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &DepthRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ShaderRegister   = 0;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = 3;
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
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

    void FVolumetricClouds::BuildCompositePSO(ID3D12Device* _Device,
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

        // Sem depth test fixo: a oclusao por geometria agora acontece no proprio raymarch
        // (marcha clampada pelo depth da cena) + upsample bilateral no PS. O depth vira
        // SRV do composite, entao nenhum DSV fica bindado neste draw.
        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

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
        Desc.DSVFormat             = DXGI_FORMAT_UNKNOWN; // depth desligado (ver acima)
        Desc.SampleDesc            = { 1, 0 };
        (void)_DSFormat;

        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&CompositePSO)));
    }

    void FVolumetricClouds::RecreateComposite(ID3D12Device* _Device,
                                              DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        if (!Initialized) return;
        BuildCompositePSO(_Device, _RTFormat, _DSFormat);
    }

    void FVolumetricClouds::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvVP,
                                           const Mat44& _InvViewProjWorld,
                                           const Mat44& _ViewProjWorldUnjittered,
                                           const Vec3& _CamWorldPos, f32 _KmPerWorldUnit,
                                           f32 _GroundRadiusKm,
                                           const Vec3& _DirToSun, const Vec3& _SunColor,
                                           const Vec3& _SkyAmbient, const Vec3& _GroundAmbient,
                                           f32 _Time, u32 _FrameIndex) {
        FrameSlot = _FrameSlot;
        Vec3 d = _DirToSun.NormalizedSafe(Vec3{ 0.0f, 0.6f, 0.8f }.Normalized());
        CPUConstants.InvViewProjNoTrans = _InvVP;
        CPUConstants.InvViewProjWorld   = _InvViewProjWorld;
        CPUConstants.PrevVPWorld        = HasPrevVP ? StoredVPWorld : _ViewProjWorldUnjittered;
        CPUConstants.TemporalParams.Y   = (HistoryValid && HasPrevVP) ? 1.0f : 0.0f;
        StoredVPWorld = _ViewProjWorldUnjittered;
        HasPrevVP     = true;

        // Camera REAL no frame do planeta (km): XZ do mundo + raio no nivel do chao + Y do
        // mundo. Andar/voar agora move as nuvens (paralaxe) e permite entrar na camada.
        const f32 Km = _KmPerWorldUnit;
        CPUConstants.CamWorld  = { _CamWorldPos.X, _CamWorldPos.Y, _CamWorldPos.Z, Km };
        CPUConstants.CloudMisc = { (f32)FullWidth, (f32)FullHeight, 0.0f, 0.0f };

        // Shadow map: grade centrada na camera, snapada ao texel (senao a sombra "nada"
        // ao andar — mesmo truque do scrolling de CSM).
        const f32 Texel = kShadowExtentKm / (f32)kShadowRes;
        CPUConstants.CloudShadowCB = { std::floor(_CamWorldPos.X * Km / Texel) * Texel,
                                       std::floor(_CamWorldPos.Z * Km / Texel) * Texel,
                                       kShadowExtentKm, (f32)kShadowRes };
        CPUConstants.CameraPos = { _CamWorldPos.X * Km,
                                   _GroundRadiusKm + _CamWorldPos.Y * Km,
                                   _CamWorldPos.Z * Km,
                                   _GroundRadiusKm + _CamWorldPos.Y * Km };
        CPUConstants.SunDir    = { d.X, d.Y, d.Z, CPUConstants.SunDir.W };
        CPUConstants.SunColor         = { _SunColor.X, _SunColor.Y, _SunColor.Z, 0.0f };
        CPUConstants.SkyAmbientCol    = { _SkyAmbient.X, _SkyAmbient.Y, _SkyAmbient.Z, 0.0f };
        CPUConstants.GroundAmbientCol = { _GroundAmbient.X, _GroundAmbient.Y, _GroundAmbient.Z, 0.0f };
        CPUConstants.CloudParams.W = _Time;
        CPUConstants.WindParams.W  = static_cast<f32>(_FrameIndex % 64u);
        // O raymarch cobre o RT (potencialmente half-res), nao a resolucao de render.
        if (RTWidth > 0 && RTHeight > 0)
            CPUConstants.ScreenParams = { (f32)RTWidth, (f32)RTHeight,
                                          1.0f / (f32)RTWidth, 1.0f / (f32)RTHeight };
        if (MappedBase) *Mapped() = CPUConstants;
    }

    void FVolumetricClouds::TransitionHist(ID3D12GraphicsCommandList* _CommandList,
                                           u32 _Index, D3D12_RESOURCE_STATES _After) {
        if (HistState[_Index] == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = HistoryRT[_Index].Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = HistState[_Index];
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        HistState[_Index] = _After;
    }

    void FVolumetricClouds::RecordShadowMap(ID3D12GraphicsCommandList* _CommandList,
                                            FTextureSRVHeap& _SRVHeap) {
        if (!Initialized || !ShadowTex || !ShadowsEnabled) return;

        if (ShadowState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = ShadowTex.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = ShadowState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            _CommandList->ResourceBarrier(1, &B);
            ShadowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        ShadowPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(NoiseTableStart));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ShadowUAVSlot));
        _CommandList->Dispatch((kShadowRes + 7) / 8, (kShadowRes + 7) / 8, 1);

        {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = ShadowTex.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            // PIXEL | NON_PIXEL: alem do deferred/shafts (PS), o volumetric fog le o
            // shadow map das nuvens em compute.
            const D3D12_RESOURCE_STATES ReadBoth =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            B.Transition.StateBefore = ShadowState;
            B.Transition.StateAfter  = ReadBoth;
            _CommandList->ResourceBarrier(1, &B);
            ShadowState = ReadBoth;
        }
    }

    void FVolumetricClouds::RecordRaymarch(ID3D12GraphicsCommandList* _CommandList,
                                           FTextureSRVHeap& _SRVHeap, u32 _DepthSRVSlot) {
        if (!Initialized || !CloudRT) return;

        TransitionRT(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _CommandList->SetComputeRootSignature(RaymarchRootSig.Get());
        _CommandList->SetPipelineState(RaymarchPSOState.Get());
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(NoiseTableStart));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->SetComputeRootDescriptorTable(3, _SRVHeap.GpuHandle(RTUAVSlot));
        _CommandList->Dispatch((RTWidth + 7) / 8, (RTHeight + 7) / 8, 1);

        if (!UseTemporal) {
            TransitionRT(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            CompositeSRVSlot = RTSRVSlot;
            return;
        }

        // Resolve temporal: raw + historico anterior -> historico atual (ping-pong).
        const u32 Cur = HistIndex, Prev = 1 - HistIndex;
        TransitionRT(_CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionHist(_CommandList, Prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionHist(_CommandList, Cur,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        TemporalPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TemporalSRVTable[Prev]));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(HistUAVSlot[Cur]));
        _CommandList->Dispatch((RTWidth + 7) / 8, (RTHeight + 7) / 8, 1);

        TransitionHist(_CommandList, Cur, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CompositeSRVSlot = HistSRVSlot[Cur];
        HistIndex    = Prev;
        HistoryValid = true;
    }

    void FVolumetricClouds::Composite(ID3D12GraphicsCommandList* _CommandList,
                                      FTextureSRVHeap& _SRVHeap, u32 _DepthSRVSlot) {
        if (!Initialized || !CloudRT) return;

        _CommandList->SetGraphicsRootSignature(CompositeRootSig.Get());
        _CommandList->SetPipelineState(CompositePSO.Get());
        _CommandList->SetGraphicsRootDescriptorTable(0, _SRVHeap.GpuHandle(CompositeSRVSlot));
        const f32 Consts[4] = { 1.0f / (f32)FullWidth, 1.0f / (f32)FullHeight,
                                (f32)RTWidth, (f32)RTHeight };
        _CommandList->SetGraphicsRoot32BitConstants(1, 4, Consts, 0);
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
