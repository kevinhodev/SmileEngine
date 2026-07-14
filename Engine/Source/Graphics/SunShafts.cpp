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
                                u32 _Width, u32 _Height) {
        if (Initialized) return;
        BuildVolRootSignature(_Device);
        BuildTemporalRootSignature(_Device);
        BuildPSOs(_Device);
        CreateConstantBuffers(_Device);
        CreateRTs(_Device, _SRVHeap, _Width, _Height);
        Initialized = true;
        LogInfo("Sun shafts (raymarch CSM + temporal) inicializado");
    }

    void FSunShafts::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                            u32 _Width, u32 _Height) {
        if (!Initialized) return;
        CreateRTs(_Device, _SRVHeap, _Width, _Height);
        ResetHistory();
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

        D3D12_DESCRIPTOR_RANGE CloudRange = DepthRange;
        CloudRange.BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER RootParams[5]{};
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

        RootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[4].DescriptorTable.pDescriptorRanges   = &CloudRange;
        RootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samplers[3]{};
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

        // linear clamp p/ o shadow map de nuvens (transmitância suave, igual ao deferred)
        Samplers[2] = Samplers[0];
        Samplers[2].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[2].ShaderRegister = 0;

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

    void FSunShafts::BuildTemporalRootSignature(ID3D12Device* _Device) {
        // b0 proprio, t0 = raw do raymarch, t1 = historia anterior (tabelas separadas:
        // os SRVs nao sao contiguos no heap), s0 linear clamp.
        D3D12_DESCRIPTOR_RANGE RawRange{};
        RawRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        RawRange.NumDescriptors                    = 1;
        RawRange.BaseShaderRegister                = 0;
        RawRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE HistRange = RawRange;
        HistRange.BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &RawRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &HistRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.MaxAnisotropy    = 1;
        Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Sampler.MinLOD           = 0.0f;
        Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        Sampler.ShaderRegister   = 0;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("SunShafts temporal root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&TemporalRootSig)));
    }

    void FSunShafts::BuildPSOs(ID3D12Device* _Device) {
        auto VS         = LoadShaderBytecode("FogFullscreen.vs_6_0.cso");
        auto PSVol      = LoadShaderBytecode("SunShaftsVolumetric.ps_6_0.cso");
        auto PSTemporal = LoadShaderBytecode("SunShaftsTemporal.ps_6_0.cso");

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
        PSODesc.pRootSignature        = VolRootSig.Get();
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

        PSODesc.PS = { PSVol.data(), PSVol.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&VolPSO)));

        PSODesc.pRootSignature = TemporalRootSig.Get();
        PSODesc.PS             = { PSTemporal.data(), PSTemporal.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&TemporalPSO)));
    }

    void FSunShafts::CreateConstantBuffers(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(VolConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_RANGE NoRead{ 0, 0 };

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&VolCB)));
        void* VolPtr = nullptr;
        SMILE_HR(VolCB->Map(0, &NoRead, &VolPtr));
        VolMappedBase = reinterpret_cast<u8*>(VolPtr);
        std::memset(VolMappedBase, 0,
                    static_cast<size_t>(FCommandQueue::kFramesInFlight) * sizeof(VolConstants));

        Desc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(TemporalConstants);
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&TemporalCB)));
        void* TempPtr = nullptr;
        SMILE_HR(TemporalCB->Map(0, &NoRead, &TempPtr));
        TemporalMappedBase = reinterpret_cast<u8*>(TempPtr);
        std::memset(TemporalMappedBase, 0,
                    static_cast<size_t>(FCommandQueue::kFramesInFlight) * sizeof(TemporalConstants));
    }

    void FSunShafts::CreateRTs(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                               u32 _Width, u32 _Height) {
        RTWidth  = std::max(1u, _Width / 2);
        RTHeight = std::max(1u, _Height / 2);

        VolRT.Reset();
        for (u32 i = 0; i < 2; ++i) HistoryRT[i].Reset();

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

        SMILE_HR(_Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&VolRT)));
        VolState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        for (u32 i = 0; i < 2; ++i) {
            SMILE_HR(_Device->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
                IID_PPV_ARGS(&HistoryRT[i])));
            HistState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        if (!RTVHeap.Native())
            RTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        _Device->CreateRenderTargetView(VolRT.Get(), &RTVDesc, RTVHeap.CpuHandle(0));
        for (u32 i = 0; i < 2; ++i)
            _Device->CreateRenderTargetView(HistoryRT[i].Get(), &RTVDesc, RTVHeap.CpuHandle(1 + i));

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels       = 1;
        if (VolSRVSlot == kInvalidSlot) VolSRVSlot = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, VolRT.Get(), SRVDesc, VolSRVSlot);
        for (u32 i = 0; i < 2; ++i) {
            if (HistSRVSlot[i] == kInvalidSlot) HistSRVSlot[i] = _SRVHeap.Allocate(1);
            _SRVHeap.CreateSRV(_Device, HistoryRT[i].Get(), SRVDesc, HistSRVSlot[i]);
        }
    }

    void FSunShafts::UpdateVolumetric(u32 _FrameSlot, const Vec3& _DirToSun,
                                      const Vec3& _SunColorTimesIntensity,
                                      const Vec4& _CollapsedFogParams, f32 _NoiseFrame,
                                      const Mat44& _InvViewProjFull, const Vec3& _CameraWorldPos,
                                      const Mat44& _ViewProjUnjittered,
                                      const Vec4& _CloudShadowParams,
                                      const Vec4& _CloudShadowParams2) {
        FrameSlot = _FrameSlot;
        if (!VolMappedBase || !TemporalMappedBase) return;

        const f32 W = static_cast<f32>(RTWidth), H = static_cast<f32>(RTHeight);

        VolConstants v{};
        v.SunDirPhase    = { _DirToSun.X, _DirToSun.Y, _DirToSun.Z, VolPhaseG };
        v.SunColorInt    = { _SunColorTimesIntensity.X, _SunColorTimesIntensity.Y,
                             _SunColorTimesIntensity.Z, VolIntensity };
        v.FogDensityP    = _CollapsedFogParams;
        v.MarchParams    = { VolSteps, VolMaxDist, _NoiseFrame, VolDust };
        v.ScreenParams   = { W, H, 1.0f / W, 1.0f / H };
        v.CloudShadowParams  = _CloudShadowParams;
        v.CloudShadowParams2 = _CloudShadowParams2;
        v.InvViewProj    = _InvViewProjFull;
        v.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, 0.0f };
        std::memcpy(VolMappedBase + static_cast<size_t>(FrameSlot) * sizeof(VolConstants),
                    &v, sizeof(VolConstants));

        TemporalConstants t{};
        t.ScreenParams   = { W, H, 1.0f / W, 1.0f / H };
        t.TemporalParams = { kTemporalBlend, (HistoryValid && HasPrevVP) ? 1.0f : 0.0f, 0.0f, 0.0f };
        t.InvViewProj    = _InvViewProjFull;
        t.PrevViewProj   = HasPrevVP ? StoredVP : _ViewProjUnjittered;
        t.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, 0.0f };
        std::memcpy(TemporalMappedBase + static_cast<size_t>(FrameSlot) * sizeof(TemporalConstants),
                    &t, sizeof(TemporalConstants));

        StoredVP  = _ViewProjUnjittered;
        HasPrevVP = true;
    }

    void FSunShafts::TransitionTo(ID3D12GraphicsCommandList* _CommandList, ID3D12Resource* _Resource,
                                  D3D12_RESOURCE_STATES& _Tracked, D3D12_RESOURCE_STATES _After) {
        if (_Tracked == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _Resource;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = _Tracked;
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        _Tracked = _After;
    }

    void FSunShafts::RecordVolumetric(ID3D12GraphicsCommandList* _CommandList,
                                      FTextureSRVHeap& _SRVHeap, u32 _DepthSRVSlot,
                                      D3D12_GPU_VIRTUAL_ADDRESS _CSMConstantsAddr,
                                      u32 _CSMShadowSRVSlot, u32 _CloudShadowSRVSlot) {
        if (!Initialized) return;

        D3D12_VIEWPORT VP{};
        VP.Width    = static_cast<FLOAT>(RTWidth);
        VP.Height   = static_cast<FLOAT>(RTHeight);
        VP.MinDepth = 0.0f;
        VP.MaxDepth = 1.0f;
        D3D12_RECT Scissor{};
        Scissor.right  = static_cast<LONG>(RTWidth);
        Scissor.bottom = static_cast<LONG>(RTHeight);
        _CommandList->RSSetViewports(1, &VP);
        _CommandList->RSSetScissorRects(1, &Scissor);
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);

        // raymarch: depth + CSM -> VolRT
        TransitionTo(_CommandList, VolRT.Get(), VolState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto VolRTV = RTVHeap.CpuHandle(0);
        _CommandList->OMSetRenderTargets(1, &VolRTV, FALSE, nullptr);
        _CommandList->SetGraphicsRootSignature(VolRootSig.Get());
        _CommandList->SetPipelineState(VolPSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, VolCBAddr());
        _CommandList->SetGraphicsRootConstantBufferView(1, _CSMConstantsAddr);
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_CSMShadowSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(_CloudShadowSRVSlot));
        _CommandList->DrawInstanced(3, 1, 0, 0);
        TransitionTo(_CommandList, VolRT.Get(), VolState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (!VolTemporal) return; // fog consome o raw direto (VolumetricSRVSlot)

        // temporal: raw + historia[prev] -> historia[cur]
        const u32 Cur  = NextHistory;
        const u32 Prev = 1 - Cur;
        TransitionTo(_CommandList, HistoryRT[Cur].Get(), HistState[Cur],
                     D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto HistRTV = RTVHeap.CpuHandle(1 + Cur);
        _CommandList->OMSetRenderTargets(1, &HistRTV, FALSE, nullptr);
        _CommandList->SetGraphicsRootSignature(TemporalRootSig.Get());
        _CommandList->SetPipelineState(TemporalPSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, TemporalCBAddr());
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(VolSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(HistSRVSlot[Prev]));
        _CommandList->DrawInstanced(3, 1, 0, 0);
        TransitionTo(_CommandList, HistoryRT[Cur].Get(), HistState[Cur],
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        LastWritten  = Cur;
        NextHistory  = Prev;
        HistoryValid = true;
    }
}
