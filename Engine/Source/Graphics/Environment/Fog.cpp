#include "Smile/Graphics/Environment/Fog.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/Backend/D3D12/ShaderUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <iterator>

namespace Smile {

    void FFogPass::Initialize(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        if (Initialized) return;
        RTFormat = _RTFormat; // cacheado p/ o OnRecreatePipelines rebuildar com o mesmo alvo
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

        // Sky-view LUT: o inscatter do height fog converge p/ a cor do ceu naquela direcao.
        D3D12_DESCRIPTOR_RANGE SkyViewRange = DepthRange;
        SkyViewRange.BaseShaderRegister = 4;

        D3D12_ROOT_PARAMETER RootParams[6]{};
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

        RootParams[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[5].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[5].DescriptorTable.pDescriptorRanges   = &SkyViewRange;
        RootParams[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

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
        static_assert(sizeof(FogConstants) % 256 == 0,
                      "o indexador do CB usa sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(FogConstants), FCommandQueue::kFramesInFlight);
        ConstantBuffer = Upload.Resource;
        MappedBase     = Upload.Mapped;
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
                                  f32 _AerialDepthKm, f32 _AerialSlices, bool _VolumetricShafts,
                                  bool _VolFogOn, f32 _VolFogMaxDist,
                                  const Vec4& _VolFogGridZ, const Vec3& _CamForward,
                                  const Vec3& _DirToSunTrue, f32 _SkyViewHeightKm,
                                  f32 _SkyBottomRKm, f32 _SkyContribution,
                                  const FVolumetricMatchParams& _VolumetricMatch) {
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

        // Sem froxel, shafts continuam substituindo o lobo analitico legado. Com froxel,
        // eles substituem somente o trecho 0..MaxDistance e o analitico MATCHED continua
        // dali para fora, sem buraco nem energia dupla.
        const bool AnalyticDir = DirEnabled && (!_VolumetricShafts || _VolFogOn);
        const Vec3 SunN = _DirToSun.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
        c.InscatteringLightDirection = { SunN.X, SunN.Y, SunN.Z, AnalyticDir ? DirStartDistance : -1.0f };

        c.InvViewProj    = _InvViewProjFull;
        c.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, _KmPerWorldUnit };
        c.AerialParams   = { _AerialDepthKm,
                             _UseAerial ? std::max(_AerialSlices, 1.0f) : 0.0f,
                             _UseHeightFog ? 1.0f : 0.0f,
                             _VolumetricShafts ? 1.0f : 0.0f };
        const f32 W = static_cast<f32>(_Width), H = static_cast<f32>(_Height);
        c.ScreenParams   = { W, H, W > 0 ? 1.0f / W : 0.0f, H > 0 ? 1.0f / H : 0.0f };
        c.DepthParams    = { _NearZ, _FarZ, 0.0f, 0.0f };
        c.VolFogParams   = _VolFogGridZ;
        c.VolFogParams2  = { _VolFogMaxDist, _VolFogOn ? 1.0f : 0.0f, 0.0f, 0.0f };
        c.CamForwardVF   = { _CamForward.X, _CamForward.Y, _CamForward.Z, 0.0f };

        // Sem view height valido (atmosfera desligada/nao inicializada) a contribuicao cai a
        // zero: o LUT bindado nesse caso e um placeholder e nao pode ser amostrado.
        const bool SkyOk = _SkyViewHeightKm > _SkyBottomRKm && _SkyBottomRKm > 0.0f;
        const Vec3 SunTrue = _DirToSunTrue.NormalizedSafe(Vec3{ 0.0f, 1.0f, 0.0f });
        c.SkyFogParams = { _SkyViewHeightKm, _SkyBottomRKm,
                           SkyOk ? std::clamp(_SkyContribution, 0.0f, 1.0f) : 0.0f, 0.0f };
        c.SkyFogSunDir = { SunTrue.X, SunTrue.Y, SunTrue.Z, 0.0f };

        const bool MatchVolFog = _VolFogOn && _VolumetricMatch.Enabled;
        if (MatchVolFog) {
            const Vec3 A{
                std::max(_VolumetricMatch.Albedo.X, 0.0f),
                std::max(_VolumetricMatch.Albedo.Y, 0.0f),
                std::max(_VolumetricMatch.Albedo.Z, 0.0f)
            };
            const f32 PhaseG = std::clamp(_VolumetricMatch.DirectionalPhaseG, -0.95f, 0.95f);
            const f32 SunScale = std::max(_VolumetricMatch.DirectionalScatteringScale, 0.0f);
            c.VolFogMatchMediumPhase = {
                std::max(_VolumetricMatch.ExtinctionScale, 0.0f), 0.0f, 0.0f, PhaseG
            };
            c.VolFogMatchSun = {
                _VolumetricMatch.SunRadiance.X * A.X * SunScale,
                _VolumetricMatch.SunRadiance.Y * A.Y * SunScale,
                _VolumetricMatch.SunRadiance.Z * A.Z * SunScale,
                1.0f
            };
            c.VolFogMatchAmbient = {
                _VolumetricMatch.AmbientRadiance.X * A.X,
                _VolumetricMatch.AmbientRadiance.Y * A.Y,
                _VolumetricMatch.AmbientRadiance.Z * A.Z,
                std::max(_VolumetricMatch.AmbientTransitionDistance, 1.0f)
            };
        }

        *Mapped() = c;
    }

    void FFogPass::Execute(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                           u32 _DepthSRVSlot, u32 _AerialVolumeSRVSlot, u32 _VolShaftsSRVSlot,
                           u32 _VolFogSRVSlot, u32 _SkyViewSRVSlot) {
        if (!Initialized) return;
        _CommandList->SetGraphicsRootSignature(RootSig.Get());
        _CommandList->SetPipelineState(PSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, CBAddr());
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_AerialVolumeSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_VolShaftsSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(_VolFogSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(5, _SRVHeap.GpuHandle(_SkyViewSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }

    FPassShaderStems FFogPass::ShaderStems() const {
        static const char* const kStems[] = { "FogFullscreen.vs", "FogFullscreen.ps" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FFogPass::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Initialized) BuildPSOs(_Ctx.Device, RTFormat);
    }

}
