#include "Smile/Graphics/SunShaftBloom.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Smile {
    void FSunShaftBloom::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                    DXGI_FORMAT _HDRFormat, u32 _RenderWidth,
                                    u32 _RenderHeight) {
        if (Initialized) return;
        BuildRootSignature(_Device);
        BuildPSOs(_Device, _HDRFormat);
        CreateConstantBuffer(_Device);
        CreateTargets(_Device, _SRVHeap, _RenderWidth, _RenderHeight);
        Initialized = true;
        LogInfo("Sun shafts F3: bloom radial HDR quarter-res inicializado");
    }

    void FSunShaftBloom::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                u32 _RenderWidth, u32 _RenderHeight) {
        if (!Initialized) return;
        CreateTargets(_Device, _SRVHeap, _RenderWidth, _RenderHeight);
    }

    void FSunShaftBloom::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SourceRange{};
        SourceRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SourceRange.NumDescriptors                    = 1;
        SourceRange.BaseShaderRegister                = 0;
        SourceRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE AuxiliaryRange = SourceRange;
        AuxiliaryRange.BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER Params[3]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 0;
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1].DescriptorTable.NumDescriptorRanges = 1;
        Params[1].DescriptorTable.pDescriptorRanges   = &SourceRange;
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &AuxiliaryRange;
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

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
        Samplers[1]                  = Samplers[0];
        Samplers[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        Samplers[1].ShaderRegister   = 1;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(Params);
        Desc.pParameters       = Params;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob;
        Microsoft::WRL::ComPtr<ID3DBlob> Errors;
        const HRESULT Hr = D3D12SerializeRootSignature(
            &Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Errors);
        if (FAILED(Hr)) {
            if (Errors)
                LogError(std::string("Sun shaft bloom root sig: ") +
                         static_cast<const char*>(Errors->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               IID_PPV_ARGS(&RootSignature)));
    }

    void FSunShaftBloom::BuildPSOs(ID3D12Device* _Device, DXGI_FORMAT _HDRFormat) {
        const auto VS = LoadShaderBytecode("FogFullscreen.vs_6_0.cso");
        const auto MaskPS = LoadShaderBytecode("SunShaftBloomMask.ps_6_0.cso");
        const auto BlurPS = LoadShaderBytecode("SunShaftBloomBlur.ps_6_0.cso");
        const auto CompositePS = LoadShaderBytecode("SunShaftBloomComposite.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable   = FALSE;
        Depth.StencilEnable = FALSE;

        D3D12_BLEND_DESC Opaque{};
        Opaque.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = RootSignature.Get();
        Desc.VS                    = { VS.data(), VS.size() };
        Desc.BlendState            = Opaque;
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.DepthStencilState     = Depth;
        Desc.InputLayout           = { nullptr, 0 };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.NumRenderTargets      = 1;
        Desc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Desc.SampleDesc            = { 1, 0 };

        Desc.PS = { MaskPS.data(), MaskPS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&MaskPSO)));
        Desc.PS = { BlurPS.data(), BlurPS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&BlurPSO)));

        D3D12_BLEND_DESC Additive{};
        auto& RT = Additive.RenderTarget[0];
        RT.BlendEnable           = TRUE;
        RT.SrcBlend              = D3D12_BLEND_ONE;
        RT.DestBlend             = D3D12_BLEND_ONE;
        RT.BlendOp               = D3D12_BLEND_OP_ADD;
        RT.SrcBlendAlpha         = D3D12_BLEND_ZERO;
        RT.DestBlendAlpha        = D3D12_BLEND_ONE;
        RT.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        RT.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        Desc.BlendState    = Additive;
        Desc.RTVFormats[0] = _HDRFormat;
        Desc.PS            = { CompositePS.data(), CompositePS.size() };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&CompositePSO)));
    }

    void FSunShaftBloom::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                                kCBRegions * sizeof(SunShaftBloomConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));
        D3D12_RANGE NoRead{ 0, 0 };
        void* Pointer = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Pointer));
        MappedBase = reinterpret_cast<u8*>(Pointer);
        std::memset(MappedBase, 0, static_cast<size_t>(Desc.Width));
    }

    void FSunShaftBloom::CreateTargets(ID3D12Device* _Device,
                                       FTextureSRVHeap& _SRVHeap,
                                       u32 _RenderWidth, u32 _RenderHeight) {
        Width  = std::max(1u, (_RenderWidth + 3u) / 4u);
        Height = std::max(1u, (_RenderHeight + 3u) / 4u);
        Targets[0].Reset();
        Targets[1].Reset();

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = Width;
        Desc.Height           = Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE Clear{};
        Clear.Format = Desc.Format;
        for (u32 i = 0; i < 2; ++i) {
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Clear,
                IID_PPV_ARGS(&Targets[i])));
            States[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        if (!RTVHeap.Native())
            RTVHeap.Initialize(_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);
        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = Desc.Format;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (u32 i = 0; i < 2; ++i)
            _Device->CreateRenderTargetView(Targets[i].Get(), &RTVDesc, RTVHeap.CpuHandle(i));

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = Desc.Format;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        for (u32 i = 0; i < 2; ++i) {
            if (SRVSlots[i] == kInvalidSlot) SRVSlots[i] = _SRVHeap.Allocate();
            _SRVHeap.CreateSRV(_Device, Targets[i].Get(), SRVDesc, SRVSlots[i]);
        }
    }

    SunShaftBloomConstants* FSunShaftBloom::Region(u32 _Pass) const {
        return reinterpret_cast<SunShaftBloomConstants*>(
            MappedBase + (static_cast<size_t>(FrameSlot) * kCBRegions + _Pass) *
                         sizeof(SunShaftBloomConstants));
    }

    D3D12_GPU_VIRTUAL_ADDRESS FSunShaftBloom::RegionAddress(u32 _Pass) const {
        return ConstantBuffer->GetGPUVirtualAddress() +
               (static_cast<UINT64>(FrameSlot) * kCBRegions + _Pass) *
               sizeof(SunShaftBloomConstants);
    }

    void FSunShaftBloom::UpdatePerFrame(u32 _FrameSlot,
                                        const Mat44& _ViewProjUnjittered,
                                        const Mat44& _InvViewProj,
                                        const Vec3& _CameraWorldPos,
                                        const Vec3& _DirectionToLight,
                                        const Vec3& _LightColor,
                                        f32 _LightIntensity, u32 _FrameIndex,
                                        bool _Active) {
        FrameSlot = _FrameSlot;
        Active = _Active && Enabled && _LightIntensity > 1e-4f;
        Fade = 0.0f;
        if (!MappedBase || !Active) return;

        const Vec3 Direction = _DirectionToLight.NormalizedSafe(Vec3{ 0.0f, 1.0f, 0.0f });
        const Vec3 LightWorld = _CameraWorldPos + Direction * 10000.0f;
        const f32 ClipX = LightWorld.X * _ViewProjUnjittered.M[0][0] +
                          LightWorld.Y * _ViewProjUnjittered.M[1][0] +
                          LightWorld.Z * _ViewProjUnjittered.M[2][0] +
                          _ViewProjUnjittered.M[3][0];
        const f32 ClipY = LightWorld.X * _ViewProjUnjittered.M[0][1] +
                          LightWorld.Y * _ViewProjUnjittered.M[1][1] +
                          LightWorld.Z * _ViewProjUnjittered.M[2][1] +
                          _ViewProjUnjittered.M[3][1];
        const f32 ClipW = LightWorld.X * _ViewProjUnjittered.M[0][3] +
                          LightWorld.Y * _ViewProjUnjittered.M[1][3] +
                          LightWorld.Z * _ViewProjUnjittered.M[2][3] +
                          _ViewProjUnjittered.M[3][3];
        if (ClipW <= 1e-4f) return;

        const Vec2 SunUV{ 0.5f + 0.5f * ClipX / ClipW,
                          0.5f - 0.5f * ClipY / ClipW };
        const f32 Offscreen = std::max(
            std::max(std::abs(SunUV.X - 0.5f) - 0.5f, 0.0f),
            std::max(std::abs(SunUV.Y - 0.5f) - 0.5f, 0.0f));
        Fade = 1.0f - std::clamp(Offscreen / 0.25f, 0.0f, 1.0f);
        if (Fade <= 0.001f) return;

        const f32 MaxColor = std::max({ _LightColor.X, _LightColor.Y, _LightColor.Z, 1e-4f });
        const Vec3 Tint{ _LightColor.X / MaxColor, _LightColor.Y / MaxColor,
                         _LightColor.Z / MaxColor };
        const f32 W = static_cast<f32>(Width);
        const f32 H = static_cast<f32>(Height);

        SunShaftBloomConstants Base{};
        Base.SunPosParams   = { SunUV.X, SunUV.Y, Fade, H / W };
        Base.BloomParams    = { std::max(Threshold, 0.0f), std::max(SoftKnee, 1e-3f),
                                std::max(MaxBrightness, 1.0f), std::max(Intensity, 0.0f) };
        Base.BlurParams     = { std::clamp(RayLength, 0.001f, 1.0f),
                                std::clamp(SourceRadius, 0.05f, 1.5f),
                                std::fmod(static_cast<f32>(_FrameIndex) * 0.61803398875f, 1.0f),
                                0.0f };
        Base.TintDepth      = { Tint.X, Tint.Y, Tint.Z,
                                1.0f / std::max(OcclusionDistance, 1.0f) };
        Base.ScreenParams   = { W, H, 1.0f / W, 1.0f / H };
        Base.InvViewProj    = _InvViewProj;
        Base.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y,
                                _CameraWorldPos.Z, 0.0f };
        *Region(0) = Base;

        const f32 Distances[kBlurPasses] = {
            std::clamp(RayLength, 0.001f, 1.0f),
            std::clamp(RayLength * 4.0f, 0.001f, 1.0f),
            std::clamp(RayLength * 12.0f, 0.001f, 1.0f)
        };
        for (u32 i = 0; i < kBlurPasses; ++i) {
            SunShaftBloomConstants Pass = Base;
            Pass.BlurParams.X = Distances[i];
            Pass.BlurParams.Z = std::fmod(Base.BlurParams.Z + 0.37f * static_cast<f32>(i), 1.0f);
            *Region(1 + i) = Pass;
        }
        *Region(1 + kBlurPasses) = Base;
    }

    void FSunShaftBloom::Transition(ID3D12GraphicsCommandList* _CommandList,
                                    u32 _Index, D3D12_RESOURCE_STATES _After) {
        if (States[_Index] == _After) return;
        D3D12_RESOURCE_BARRIER Barrier{};
        Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource   = Targets[_Index].Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = States[_Index];
        Barrier.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &Barrier);
        States[_Index] = _After;
    }

    void FSunShaftBloom::RecordMaskAndBlur(ID3D12GraphicsCommandList* _CommandList,
                                           FTextureSRVHeap& _SRVHeap,
                                           u32 _HDRSRVSlot, u32 _DepthSRVSlot) {
        if (!HasWork()) return;

        D3D12_VIEWPORT Viewport{};
        Viewport.Width    = static_cast<FLOAT>(Width);
        Viewport.Height   = static_cast<FLOAT>(Height);
        Viewport.MinDepth = 0.0f;
        Viewport.MaxDepth = 1.0f;
        D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };

        _CommandList->SetGraphicsRootSignature(RootSignature.Get());
        _CommandList->RSSetViewports(1, &Viewport);
        _CommandList->RSSetScissorRects(1, &Scissor);
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);

        Transition(_CommandList, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto MaskRTV = RTVHeap.CpuHandle(0);
        _CommandList->OMSetRenderTargets(1, &MaskRTV, FALSE, nullptr);
        _CommandList->SetPipelineState(MaskPSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, RegionAddress(0));
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_HDRSRVSlot));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_DepthSRVSlot));
        _CommandList->DrawInstanced(3, 1, 0, 0);
        Transition(_CommandList, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        _CommandList->SetPipelineState(BlurPSO.Get());
        u32 Source = 0;
        for (u32 i = 0; i < kBlurPasses; ++i) {
            const u32 Destination = 1u - Source;
            Transition(_CommandList, Destination, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto RTV = RTVHeap.CpuHandle(Destination);
            _CommandList->OMSetRenderTargets(1, &RTV, FALSE, nullptr);
            _CommandList->SetGraphicsRootConstantBufferView(0, RegionAddress(1 + i));
            _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(SRVSlots[Source]));
            _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(SRVSlots[Source]));
            _CommandList->DrawInstanced(3, 1, 0, 0);
            Transition(_CommandList, Destination, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Source = Destination;
        }
        FinalTarget = Source;
    }

    void FSunShaftBloom::Composite(ID3D12GraphicsCommandList* _CommandList,
                                   FTextureSRVHeap& _SRVHeap,
                                   u32 _FullWidth, u32 _FullHeight) {
        if (!HasWork()) return;
        auto* Constants = Region(1 + kBlurPasses);
        const f32 W = static_cast<f32>(_FullWidth);
        const f32 H = static_cast<f32>(_FullHeight);
        Constants->ScreenParams = { W, H, 1.0f / W, 1.0f / H };

        D3D12_VIEWPORT Viewport{};
        Viewport.Width    = W;
        Viewport.Height   = H;
        Viewport.MinDepth = 0.0f;
        Viewport.MaxDepth = 1.0f;
        D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(_FullWidth), static_cast<LONG>(_FullHeight) };

        _CommandList->SetGraphicsRootSignature(RootSignature.Get());
        _CommandList->SetPipelineState(CompositePSO.Get());
        _CommandList->RSSetViewports(1, &Viewport);
        _CommandList->RSSetScissorRects(1, &Scissor);
        _CommandList->SetGraphicsRootConstantBufferView(0, RegionAddress(1 + kBlurPasses));
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(SRVSlots[FinalTarget]));
        _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(SRVSlots[FinalTarget]));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
