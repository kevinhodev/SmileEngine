#include "Smile/Graphics/Fog.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace Smile {

    void FFogPass::Initialize(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        if (Initialized) return;
        BuildRootSignature(_Device);
        BuildPSOs(_Device, _RTFormat);
        CreateConstantBuffer(_Device);
        Initialized = true;
        LogDebug("Fog deferido (aerial perspective + height fog) inicializado");
    }

    void FFogPass::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE DepthRange{};
        DepthRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DepthRange.NumDescriptors                    = 1;
        DepthRange.BaseShaderRegister                = 0; 
        DepthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE AerialRange = DepthRange;
        AerialRange.BaseShaderRegister = 1;

        D3D12_DESCRIPTOR_RANGE VolShaftsRange = DepthRange;
        VolShaftsRange.BaseShaderRegister = 2;

        D3D12_DESCRIPTOR_RANGE VolFogRange = DepthRange;
        VolFogRange.BaseShaderRegister = 3;

        D3D12_ROOT_PARAMETER RootParams[5]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; 
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &DepthRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &AerialRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &VolShaftsRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[4].DescriptorTable.pDescriptorRanges   = &VolFogRange;
        RootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

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
                LogError(std::string("Fog root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FFogPass::BuildPSOs(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        auto VS   = LoadShaderBytecode("FogFullscreen.vs_6_0.cso");
        auto PS   = LoadShaderBytecode("FogFullscreen.ps_6_0.cso");

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
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = _RTFormat;
        PSODesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        PSODesc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PSO)));
    }

    void FFogPass::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(FogConstants);
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
        FogConstants Zero{};
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedBase + static_cast<size_t>(i) * sizeof(FogConstants), &Zero, sizeof(FogConstants));
    }

    Vec4 FFogPass::CollapsedFogParams(f32 _ObserverHeight) const {
        auto Collapse = [](f32 Density, f32 Falloff, f32 ObserverHeight, f32 FogH) -> f32 {
            f32 Exponent = -Falloff * (ObserverHeight - FogH);
            Exponent = std::max(-126.0f, std::min(126.0f, Exponent));
            return Density * std::pow(2.0f, Exponent);
        };
        return { Collapse(Density,  HeightFalloff,  _ObserverHeight, FogHeight),  HeightFalloff,
                 Collapse(Density2, HeightFalloff2, _ObserverHeight, FogHeight2), HeightFalloff2 };
    }

    void FFogPass::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProjFull,
                                  const Vec3& _CameraWorldPos, f32 _KmPerWorldUnit,
                                  const Vec3& _DirToSun, f32 _NearZ, f32 _FarZ,
                                  u32 _Width, u32 _Height, bool _UseAerial, bool _UseHeightFog,
                                  f32 _AerialDepthKm, bool _VolumetricShafts,
                                  bool _VolFogOn, f32 _VolFogMaxDist,
                                  const Vec4& _VolFogGridZ, const Vec3& _CamForward) {
        FrameSlot = _FrameSlot;
        if (!MappedBase) return;

        const f32  ObserverH  = _CameraWorldPos.Y;
        const Vec4 Coll       = CollapsedFogParams(ObserverH);
        const f32  Collapsed1 = Coll.X;
        const f32  Collapsed2 = Coll.Z;

        FogConstants c{};
        c.ExponentialFogParameters  = { Collapsed1, HeightFalloff, ObserverH, StartDistance };
        c.ExponentialFogParameters2 = { Collapsed2, HeightFalloff2, Density2, FogHeight2 };
        c.ExponentialFogParameters3 = { Density, FogHeight, 0.0f, CutoffDistance };
        c.FogInscatteringColor      = { FogColor.X, FogColor.Y, FogColor.Z, 1.0f - MaxOpacity };
        c.DirectionalInscatteringColor = { DirColor.X, DirColor.Y, DirColor.Z, DirExponent };

        // shafts volumetricos substituem o analitico (manter os dois dobraria a energia)
        const bool AnalyticDir = DirEnabled && !_VolumetricShafts;
        const Vec3 SunN = _DirToSun.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
        c.InscatteringLightDirection = { SunN.X, SunN.Y, SunN.Z, AnalyticDir ? DirStartDistance : -1.0f };

        c.InvViewProj    = _InvViewProjFull;
        c.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, _KmPerWorldUnit };
        c.AerialParams   = { _AerialDepthKm, _UseAerial ? 1.0f : 0.0f, _UseHeightFog ? 1.0f : 0.0f,
                             _VolumetricShafts ? 1.0f : 0.0f };
        const f32 W = static_cast<f32>(_Width), H = static_cast<f32>(_Height);
        c.ScreenParams   = { W, H, W > 0 ? 1.0f / W : 0.0f, H > 0 ? 1.0f / H : 0.0f };
        c.DepthParams    = { _NearZ, _FarZ, 0.0f, 0.0f };
        c.VolFogParams   = _VolFogGridZ;
        c.VolFogParams2  = { _VolFogMaxDist, _VolFogOn ? 1.0f : 0.0f, 0.0f, 0.0f };
        c.CamForwardVF   = { _CamForward.X, _CamForward.Y, _CamForward.Z, 0.0f };

        *Mapped() = c;
    }

    void FFogPass::Execute(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                           u32 _DepthSRVSlot, u32 _AerialVolumeSRVSlot, u32 _VolShaftsSRVSlot,
                           u32 _VolFogSRVSlot) {
        if (!Initialized) return;
        _CommandList->SetGraphicsRootSignature(RootSig.Get());
        _CommandList->SetPipelineState(PSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, CBAddr());
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_AerialVolumeSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_VolShaftsSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(_VolFogSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
