#include "Smile/Graphics/Skybox.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <fstream>
#include <vector>
#include <stdexcept>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        std::vector<u8> LoadShader(const std::string& _Name) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Name;
            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError("Falha ao abrir shader skybox: " + FullPath);
                throw std::runtime_error("Skybox shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }
    }

    void FSkybox::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 1;
        SRVRange.BaseShaderRegister                = 0; // t0
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[2]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; // b0
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC StaticSampler{};
        StaticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        StaticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSampler.MaxAnisotropy    = 1;
        StaticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        StaticSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        StaticSampler.MinLOD           = 0.0f;
        StaticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
        StaticSampler.ShaderRegister   = 0; // s0
        StaticSampler.RegisterSpace    = 0;
        StaticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &StaticSampler;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Skybox root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));
    }

    void FSkybox::BuildPSO(ID3D12Device* _Device, u32 _SampleCount,
                            DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        auto VS = LoadShader("Skybox.vs_6_0.cso");
        auto PS = LoadShader("Skybox.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode              = D3D12_FILL_MODE_SOLID;
        Raster.CullMode              = D3D12_CULL_MODE_NONE;
        Raster.FrontCounterClockwise = FALSE;
        Raster.DepthClipEnable       = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // don't pollute depth buffer
        Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Depth.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = RootSignature.Get();
        Desc.VS                    = { VS.data(), VS.size() };
        Desc.PS                    = { PS.data(), PS.size() };
        Desc.BlendState            = Blend;
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.DepthStencilState     = Depth;
        Desc.InputLayout           = { nullptr, 0 }; // VS uses SV_VertexID
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.NumRenderTargets      = 1;
        Desc.RTVFormats[0]         = _RTFormat;
        Desc.DSVFormat             = _DSFormat;
        Desc.SampleDesc            = { _SampleCount, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&PSO)));
    }

    void FSkybox::Initialize(ID3D12Device* _Device, u32 _SampleCount,
                              DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildRootSignature(_Device);
        BuildPSO(_Device, _SampleCount, _RTFormat, _DSFormat);

        // Upload heap CBV (mapped persistent).
        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = sizeof(SkyboxConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CBV)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CBV->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCBV)));
    }

    void FSkybox::Recreate(ID3D12Device* _Device, u32 _SampleCount,
                            DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        BuildPSO(_Device, _SampleCount, _RTFormat, _DSFormat);
    }

    void FSkybox::Render(ID3D12GraphicsCommandList* _CommandList,
                          FTextureSRVHeap& _SRVHeap,
                          u32 _EnvCubeSRVSlot,
                          const Mat44& _InvViewProjNoTranslation,
                          f32 _IBLIntensity, f32 _IBLRotation) {
        MappedCBV->InvViewProjNoTranslation = _InvViewProjNoTranslation;
        MappedCBV->IBLIntensity             = _IBLIntensity;
        MappedCBV->IBLRotation              = _IBLRotation;

        _CommandList->SetGraphicsRootSignature(RootSignature.Get());
        _CommandList->SetPipelineState(PSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, CBV->GetGPUVirtualAddress());
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(_EnvCubeSRVSlot));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
