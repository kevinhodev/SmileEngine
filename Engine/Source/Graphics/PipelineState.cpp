#include "Smile/Graphics/PipelineState.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/SwapChain.h"
#include <d3d12.h>
#include <fstream>
#include <vector>
#include <string>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        std::vector<u8> LoadShaderBlob(const std::string& _Filename) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Filename;

            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError(std::string("Falha ao abrir shader: ") + FullPath);
                throw std::runtime_error("Shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }
    }

    void FPipelineState::Initialize(ID3D12Device* _Device) {
        // Root layout:
        //   [0] CBV  b0          FrameConstants (all)
        //   [1] CBV  b1          MaterialConstants (PS)
        //   [2] SRV table t0-t5  material textures (PS)
        //   [3] SRV table t6-t8  IBL: irradiance cube, prefiltered cube, BRDF LUT (PS)
        //   Static sampler s0    anisotropic wrap (materials)
        //   Static sampler s1    linear clamp     (cubemaps + LUT)
        D3D12_ROOT_PARAMETER RootParams[4]{};

        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[1].Descriptor.ShaderRegister = 1;
        RootParams[1].Descriptor.RegisterSpace  = 0;
        RootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE MaterialRange{};
        MaterialRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        MaterialRange.NumDescriptors                    = 6;
        MaterialRange.BaseShaderRegister                = 0; // t0..t5
        MaterialRange.RegisterSpace                     = 0;
        MaterialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &MaterialRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE IBLRange{};
        IBLRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        IBLRange.NumDescriptors                    = 3;
        IBLRange.BaseShaderRegister                = 6; // t6..t8
        IBLRange.RegisterSpace                     = 0;
        IBLRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &IBLRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC StaticSamplers[2]{};
        // s0 — material sampler (anisotropic wrap)
        StaticSamplers[0].Filter           = D3D12_FILTER_ANISOTROPIC;
        StaticSamplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        StaticSamplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        StaticSamplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        StaticSamplers[0].MaxAnisotropy    = 16;
        StaticSamplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        StaticSamplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        StaticSamplers[0].MinLOD           = 0.0f;
        StaticSamplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        StaticSamplers[0].ShaderRegister   = 0;
        StaticSamplers[0].RegisterSpace    = 0;
        StaticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // s1 — IBL sampler (trilinear clamp)
        StaticSamplers[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        StaticSamplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSamplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSamplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSamplers[1].MaxAnisotropy    = 1;
        StaticSamplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        StaticSamplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        StaticSamplers[1].MinLOD           = 0.0f;
        StaticSamplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
        StaticSamplers[1].ShaderRegister   = 1;
        StaticSamplers[1].RegisterSpace    = 0;
        StaticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc{};
        RootSignatureDesc.NumParameters     = _countof(RootParams);
        RootSignatureDesc.pParameters       = RootParams;
        RootSignatureDesc.NumStaticSamplers = _countof(StaticSamplers);
        RootSignatureDesc.pStaticSamplers   = StaticSamplers;
        RootSignatureDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> RootBlob;
        ComPtr<ID3DBlob> ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&RootSignatureDesc,
                                                  D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &RootBlob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Root signature error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }

        SMILE_HR(_Device->CreateRootSignature(0,
                                              RootBlob->GetBufferPointer(),
                                              RootBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));

        RecreatePSO(_Device, 1);
    }

    void FPipelineState::RecreatePSO(ID3D12Device* _Device, u32 _SampleCount) {
        auto VertexShaderBlob = LoadShaderBlob("Triangle.vs_6_0.cso");
        auto PixelShaderBlob  = LoadShaderBlob("Triangle.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC RasterizerDesc{};
        RasterizerDesc.FillMode              = D3D12_FILL_MODE_SOLID;
        RasterizerDesc.CullMode              = D3D12_CULL_MODE_BACK;
        RasterizerDesc.FrontCounterClockwise = FALSE;
        RasterizerDesc.DepthClipEnable       = TRUE;

        D3D12_BLEND_DESC BlendDesc{};
        BlendDesc.RenderTarget[0].BlendEnable           = FALSE;
        BlendDesc.RenderTarget[0].LogicOpEnable         = FALSE;
        BlendDesc.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        BlendDesc.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
        BlendDesc.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        BlendDesc.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        BlendDesc.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
        BlendDesc.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        BlendDesc.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
        BlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC DepthDesc{};
        DepthDesc.DepthEnable    = TRUE;
        DepthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        DepthDesc.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSignature.Get();
        PSODesc.VS                    = { VertexShaderBlob.data(), VertexShaderBlob.size() };
        PSODesc.PS                    = { PixelShaderBlob.data(), PixelShaderBlob.size() };
        PSODesc.BlendState            = BlendDesc;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = RasterizerDesc;
        PSODesc.DepthStencilState     = DepthDesc;
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 1;
        PSODesc.RTVFormats[0]         = FSwapChain::kFormat;
        PSODesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        PSODesc.SampleDesc            = { _SampleCount, 0 };

        ComPtr<ID3D12PipelineState> NewPSO;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSO)));

        PipelineState = NewPSO;
    }
}
