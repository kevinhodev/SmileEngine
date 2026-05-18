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
        D3D12_ROOT_PARAMETER RootParam{};
        RootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParam.Descriptor.ShaderRegister = 0;
        RootParam.Descriptor.RegisterSpace  = 0;
        RootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc{};
        RootSignatureDesc.NumParameters     = 1;
        RootSignatureDesc.pParameters       = &RootParam;
        RootSignatureDesc.NumStaticSamplers = 0;
        RootSignatureDesc.pStaticSamplers   = nullptr;
        RootSignatureDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> RootBlob;
        ComPtr<ID3DBlob> ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&RootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &RootBlob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob) {
                LogError(std::string("Root signature error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            }
            SMILE_HR(Hr);
        }

        SMILE_HR(_Device->CreateRootSignature(0,
                                              RootBlob->GetBufferPointer(),
                                              RootBlob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSignature)));

        RecreatePSO(_Device, 1);
    }

    void FPipelineState::RecreatePSO(ID3D12Device* _Device, u32 _SampleCount) {
        PipelineState.Reset();

        auto VertexShaderBlob = LoadShaderBlob("Triangle.vs_6_0.cso");
        auto PixelShaderBlob = LoadShaderBlob("Triangle.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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

        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&PipelineState)));
    }
}
