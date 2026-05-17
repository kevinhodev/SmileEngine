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

} // anon

void PipelineState::Initialize(ID3D12Device* _Device) {
    // Root CBV para o Constant Buffer de transformacao (register b0)
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0; // b0
    rootParam.Descriptor.RegisterSpace  = 0;
    rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = 1;
    rootDesc.pParameters       = &rootParam;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers   = nullptr;
    rootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LogError(std::string("Root signature error: ") +
                     static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        SMILE_HR(hr);
    }

    SMILE_HR(_Device->CreateRootSignature(0,
                                          rootBlob->GetBufferPointer(),
                                          rootBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&m_rootSignature)));

    RecreatePSO(_Device, 1);
}

void PipelineState::RecreatePSO(ID3D12Device* _Device, u32 _SampleCount) {
    PipelineState.Reset();

    auto vsBlob = LoadShaderBlob("Triangle.vs_6_0.cso");
    auto psBlob = LoadShaderBlob("Triangle.ps_6_0.cso");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode              = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode              = D3D12_CULL_MODE_BACK;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthClipEnable       = TRUE;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable           = FALSE;
    blendDesc.RenderTarget[0].LogicOpEnable         = FALSE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable    = TRUE;
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    depthDesc.StencilEnable  = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_rootSignature.Get();
    psoDesc.VS                    = { vsBlob.data(), vsBlob.size() };
    psoDesc.PS                    = { psBlob.data(), psBlob.size() };
    psoDesc.BlendState            = blendDesc;
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.DepthStencilState     = depthDesc;
    psoDesc.InputLayout           = { inputLayout, _countof(inputLayout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = SwapChain::kFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { _SampleCount, 0 };

    SMILE_HR(_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&PipelineState)));
}

} // namespace Smile
