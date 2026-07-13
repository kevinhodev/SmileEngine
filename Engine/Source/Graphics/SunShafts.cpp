#include "Smile/Graphics/SunShafts.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Smile {

    void FSunShafts::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                DXGI_FORMAT _HDRFormat, u32 _Width, u32 _Height) {
        if (Initialized) return;
        BuildRootSignature(_Device);
        BuildVolRootSignature(_Device);
        BuildPSOs(_Device, _HDRFormat);
        CreateConstantBuffer(_Device);
        CreateRTs(_Device, _SRVHeap, _Width, _Height);
        Initialized = true;
        LogInfo("Sun shafts (radial blur + volumetrico CSM) inicializado");
    }

    void FSunShafts::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                            u32 _Width, u32 _Height) {
        if (!Initialized) return;
        CreateRTs(_Device, _SRVHeap, _Width, _Height);
    }

    void FSunShafts::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE Range0{};
        Range0.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Range0.NumDescriptors                    = 1;
        Range0.BaseShaderRegister                = 0;
        Range0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE Range1 = Range0;
        Range1.BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &Range0;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &Range1;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        Samplers[1] = Samplers[0];
        Samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
        Samplers[1].ShaderRegister = 1;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("SunShafts root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FSunShafts::BuildVolRootSignature(ID3D12Device* _Device) {
        // Registers casam com o CSMCommon.hlsli: CSMCB em b3, SunShadowMap em t11,
        // ShadowCmp em s2. Depth da cena em t0, ponto em s1, constantes proprias em b0.
        D3D12_DESCRIPTOR_RANGE DepthRange{};
        DepthRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DepthRange.NumDescriptors                    = 1;
        DepthRange.BaseShaderRegister                = 0;
        DepthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE ShadowRange = DepthRange;
        ShadowRange.BaseShaderRegister = 11;

        D3D12_ROOT_PARAMETER RootParams[4]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[1].Descriptor.ShaderRegister = 3;
        RootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &DepthRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &ShadowRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 1;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // identico ao s2 do deferred lighting (PipelineState.cpp): LESS_EQUAL + borda branca
        Samplers[1] = Samplers[0];
        Samplers[1].Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        Samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Samplers[1].BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        Samplers[1].ShaderRegister = 2;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("SunShafts vol root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&VolRootSig)));
    }

    void FSunShafts::BuildPSOs(ID3D12Device* _Device, DXGI_FORMAT _HDRFormat) {
        auto VS      = LoadShaderBytecode("FogFullscreen.vs_6_0.cso");
        auto PSMask  = LoadShaderBytecode("SunShaftsMask.ps_6_0.cso");
        auto PSBlur  = LoadShaderBytecode("SunShaftsBlur.ps_6_0.cso");
        auto PSApply = LoadShaderBytecode("SunShaftsApply.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Opaque{};
        Opaque.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.BlendState            = Opaque;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        PSODesc.PS = { PSMask.data(), PSMask.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&MaskPSO)));

        PSODesc.PS = { PSBlur.data(), PSBlur.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&BlurPSO)));

        // Apply: aditivo ONE/ONE por cima do HDR ja fogado
        D3D12_BLEND_DESC Additive{};
        Additive.RenderTarget[0].BlendEnable           = TRUE;
        Additive.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        Additive.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        Additive.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        Additive.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ZERO;
        Additive.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ONE;
        Additive.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        Additive.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        PSODesc.BlendState    = Additive;
        PSODesc.RTVFormats[0] = _HDRFormat;
        PSODesc.PS            = { PSApply.data(), PSApply.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&ApplyPSO)));

        // F2 — raymarch volumetrico (root sig propria, RT meia-res opaco)
        auto PSVol = LoadShaderBytecode("SunShaftsVolumetric.ps_6_0.cso");
        PSODesc.pRootSignature = VolRootSig.Get();
        PSODesc.BlendState     = Opaque;
        PSODesc.RTVFormats[0]  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        PSODesc.PS             = { PSVol.data(), PSVol.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&VolPSO)));

        auto PSVolBlur = LoadShaderBytecode("SunShaftsVolBlur.ps_6_0.cso");
        PSODesc.PS = { PSVolBlur.data(), PSVolBlur.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&VolBlurPSO)));
    }

    void FSunShafts::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                                kCBRegions * sizeof(ShaftConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Ptr));
        MappedBase = reinterpret_cast<u8*>(Ptr);
        std::memset(MappedBase, 0,
                    static_cast<size_t>(FCommandQueue::kFramesInFlight) * kCBRegions *
                        sizeof(ShaftConstants));

        // CB do volumetrico (1 regiao por frame-slot)
        Desc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(VolConstants);
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&VolCB)));
        void* VolPtr = nullptr;
        SMILE_HR(VolCB->Map(0, &NoRead, &VolPtr));
        VolMappedBase = reinterpret_cast<u8*>(VolPtr);
        std::memset(VolMappedBase, 0,
                    static_cast<size_t>(FCommandQueue::kFramesInFlight) * sizeof(VolConstants));
    }

    void FSunShafts::CreateRTs(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                               u32 _Width, u32 _Height) {
        RTWidth  = std::max(1u, _Width / 2);
        RTHeight = std::max(1u, _Height / 2);

        for (u32 i = 0; i < 2; ++i) ShaftRT[i].Reset();

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = RTWidth;
        Desc.Height           = RTHeight;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        for (u32 i = 0; i < 2; ++i) {
            SMILE_HR(_Device->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
                IID_PPV_ARGS(&ShaftRT[i])));
            RTState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // F2 — RT do inscatter volumetrico (mesma meia-res)
        VolRT.Reset();
        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&VolRT)));
        VolState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!RTVHeap.Native())
            RTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (u32 i = 0; i < 2; ++i)
            _Device->CreateRenderTargetView(ShaftRT[i].Get(), &RTVDesc, RTVHeap.CpuHandle(i));
        _Device->CreateRenderTargetView(VolRT.Get(), &RTVDesc, RTVHeap.CpuHandle(2));

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        for (u32 i = 0; i < 2; ++i) {
            if (RTSRVSlot[i] == kInvalidSlot) RTSRVSlot[i] = _SRVHeap.Allocate(1);
            _SRVHeap.CreateSRV(_Device, ShaftRT[i].Get(), SRVDesc, RTSRVSlot[i]);
        }
        if (VolSRVSlot == kInvalidSlot) VolSRVSlot = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, VolRT.Get(), SRVDesc, VolSRVSlot);
    }

    void FSunShafts::UpdatePerFrame(u32 _FrameSlot, const Vec2& _SunUV, f32 _Fade,
                                    const Mat44& _InvViewProjFull, const Vec3& _CameraWorldPos,
                                    const Vec3& _Tint) {
        FrameSlot       = _FrameSlot;
        LastFade        = _Fade;
        LastInvViewProj = _InvViewProjFull;
        LastCameraPos   = _CameraWorldPos;
        if (!MappedBase) return;

        const f32 Aspect = RTWidth > 0 ? static_cast<f32>(RTHeight) / static_cast<f32>(RTWidth)
                                       : 1.0f;

        ShaftConstants Base{};
        Base.SunPosParams   = { _SunUV.X, _SunUV.Y, _Fade, Aspect };
        Base.ShaftParams    = { Threshold, FirstPassDistance, 1.0f, Intensity };
        Base.ShaftTint      = { _Tint.X, _Tint.Y, _Tint.Z, 1.0f / std::max(OcclusionDepthRange, 1.0f) };
        const f32 W = static_cast<f32>(RTWidth), H = static_cast<f32>(RTHeight);
        Base.ScreenParams   = { W, H, 1.0f / W, 1.0f / H };
        Base.InvViewProj    = _InvViewProjFull;
        Base.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, 0.0f };

        *Region(0) = Base; // mascara

        // blurs: distancia exponencial por passe (UE: pow(0.4 * NUM_SAMPLES, pass))
        for (u32 i = 0; i < kBlurPasses; ++i) {
            ShaftConstants c = Base;
            c.ShaftParams.Z  = std::pow(4.8f, static_cast<f32>(i));
            *Region(1 + i) = c;
        }

        // apply: ScreenParams full-res e preenchido no Composite (dims do chamador)
        *Region(1 + kBlurPasses) = Base;
    }

    void FSunShafts::Transition(ID3D12GraphicsCommandList* _CommandList, u32 _Index,
                                D3D12_RESOURCE_STATES _After) {
        if (RTState[_Index] == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = ShaftRT[_Index].Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = RTState[_Index];
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        RTState[_Index] = _After;
    }

    void FSunShafts::RecordMaskAndBlur(ID3D12GraphicsCommandList* _CommandList,
                                       FTextureSRVHeap& _SRVHeap,
                                       u32 _HDRSRVSlot, u32 _DepthSRVSlot) {
        if (!Initialized) return;

        D3D12_VIEWPORT VP{};
        VP.Width    = static_cast<FLOAT>(RTWidth);
        VP.Height   = static_cast<FLOAT>(RTHeight);
        VP.MinDepth = 0.0f;
        VP.MaxDepth = 1.0f;
        D3D12_RECT Scissor{};
        Scissor.right  = static_cast<LONG>(RTWidth);
        Scissor.bottom = static_cast<LONG>(RTHeight);

        _CommandList->SetGraphicsRootSignature(RootSig.Get());
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &Scissor);
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);

        // mascara: HDR + depth -> RT0
        Transition(_CommandList, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto RTV0 = RTVHeap.CpuHandle(0);
        _CommandList->OMSetRenderTargets(1, &RTV0, FALSE, nullptr);
        _CommandList->SetPipelineState(MaskPSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, RegionAddr(0));
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_HDRSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->DrawInstanced(3, 1, 0, 0);
        Transition(_CommandList, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // blur radial ping-pong (0 -> 1 -> 0 -> 1)
        _CommandList->SetPipelineState(BlurPSO.Get());
        u32 Src = 0;
        for (u32 i = 0; i < kBlurPasses; ++i) {
            const u32 Dst = 1 - Src;
            Transition(_CommandList, Dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto RTV = RTVHeap.CpuHandle(Dst);
            _CommandList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);
            _CommandList->SetGraphicsRootConstantBufferView(0, RegionAddr(1 + i));
            _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(RTSRVSlot[Src]));
            _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(RTSRVSlot[Src]));
            _CommandList->DrawInstanced(3, 1, 0, 0);
            Transition(_CommandList, Dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Src = Dst;
        }
        FinalRT = Src;
    }

    void FSunShafts::Composite(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                               u32 _FullWidth, u32 _FullHeight) {
        if (!Initialized || !MappedBase) return;

        // regiao do apply precisa das dims full-res do RTV do chamador
        ShaftConstants* Apply = Region(1 + kBlurPasses);
        const f32 W = static_cast<f32>(_FullWidth), H = static_cast<f32>(_FullHeight);
        Apply->ScreenParams = { W, H, 1.0f / W, 1.0f / H };

        D3D12_VIEWPORT VP{};
        VP.Width    = static_cast<FLOAT>(_FullWidth);
        VP.Height   = static_cast<FLOAT>(_FullHeight);
        VP.MinDepth = 0.0f;
        VP.MaxDepth = 1.0f;
        D3D12_RECT Scissor{};
        Scissor.right  = static_cast<LONG>(_FullWidth);
        Scissor.bottom = static_cast<LONG>(_FullHeight);

        _CommandList->SetGraphicsRootSignature(RootSig.Get());
        _CommandList->SetPipelineState(ApplyPSO.Get());
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &Scissor);
        _CommandList->SetGraphicsRootConstantBufferView(0, RegionAddr(1 + kBlurPasses));
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(RTSRVSlot[FinalRT]));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(RTSRVSlot[FinalRT]));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }

    void FSunShafts::UpdateVolumetric(const Vec3& _DirToSun, const Vec3& _SunColorTimesIntensity,
                                      const Vec4& _CollapsedFogParams, f32 _NoiseFrame) {
        if (!VolMappedBase) return;
        auto* c = reinterpret_cast<VolConstants*>(
            VolMappedBase + static_cast<size_t>(FrameSlot) * sizeof(VolConstants));

        const Vec3 SunN = _DirToSun.NormalizedSafe(Vec3{ 0.0f, 1.0f, 0.0f });
        c->SunDirPhase = { SunN.X, SunN.Y, SunN.Z, VolPhaseG };
        c->SunColorInt = { _SunColorTimesIntensity.X, _SunColorTimesIntensity.Y,
                           _SunColorTimesIntensity.Z, VolIntensity };
        c->FogDensityP = _CollapsedFogParams;
        c->MarchParams = { VolSteps, VolMaxDist, _NoiseFrame, 0.0f };
        const f32 W = static_cast<f32>(RTWidth), H = static_cast<f32>(RTHeight);
        c->ScreenParams   = { W, H, 1.0f / W, 1.0f / H };
        c->InvViewProj    = LastInvViewProj;
        c->CameraWorldPos = { LastCameraPos.X, LastCameraPos.Y, LastCameraPos.Z, 0.0f };
    }

    void FSunShafts::RecordVolumetric(ID3D12GraphicsCommandList* _CommandList,
                                      FTextureSRVHeap& _SRVHeap, u32 _DepthSRVSlot,
                                      D3D12_GPU_VIRTUAL_ADDRESS _CSMConstantsAddr,
                                      u32 _CSMShadowSRVSlot) {
        if (!Initialized) return;

        if (VolState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = VolRT.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = VolState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            _CommandList->ResourceBarrier(1, &B);
            VolState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        D3D12_VIEWPORT VP{};
        VP.Width    = static_cast<FLOAT>(RTWidth);
        VP.Height   = static_cast<FLOAT>(RTHeight);
        VP.MinDepth = 0.0f;
        VP.MaxDepth = 1.0f;
        D3D12_RECT Scissor{};
        Scissor.right  = static_cast<LONG>(RTWidth);
        Scissor.bottom = static_cast<LONG>(RTHeight);

        _CommandList->SetGraphicsRootSignature(VolRootSig.Get());
        _CommandList->SetPipelineState(VolPSO.Get());
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &Scissor);

        auto RTV = RTVHeap.CpuHandle(2);
        _CommandList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);
        _CommandList->SetGraphicsRootConstantBufferView(
            0, VolCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(VolConstants));
        _CommandList->SetGraphicsRootConstantBufferView(1, _CSMConstantsAddr);
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_CSMShadowSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = VolRT.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        _CommandList->ResourceBarrier(1, &B);
        VolState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // blur 3x3: VolRT -> ShaftRT[0] (o fog le o resultado borrado; o radial blur
        // so reusa o ShaftRT[0] depois do fog — ordem de execucao da GPU garante)
        Transition(_CommandList, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto BlurRTV = RTVHeap.CpuHandle(0);
        _CommandList->OMSetRenderTargets(1, &BlurRTV, FALSE, nullptr);
        _CommandList->SetPipelineState(VolBlurPSO.Get());
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(VolSRVSlot));
        _CommandList->DrawInstanced(3, 1, 0, 0);
        Transition(_CommandList, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
