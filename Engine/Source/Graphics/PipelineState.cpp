#include "Smile/Graphics/PipelineState.h"
#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/SwapChain.h"
#include <d3d12.h>
#include <fstream>
#include <vector>
#include <string>

namespace Smile {
    void FPipelineState::Initialize(ID3D12Device* _Device) {
        D3D12_ROOT_PARAMETER RootParams[12]{};

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
        MaterialRange.NumDescriptors                    = 8;
        MaterialRange.BaseShaderRegister                = 0; 
        MaterialRange.RegisterSpace                     = 0;
        MaterialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &MaterialRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE IBLRange{};
        IBLRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        IBLRange.NumDescriptors                    = 3;
        IBLRange.BaseShaderRegister                = 8; 
        IBLRange.RegisterSpace                     = 0;
        IBLRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &IBLRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[4].Descriptor.ShaderRegister = 2;
        RootParams[4].Descriptor.RegisterSpace  = 0;
        RootParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        RootParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[5].Descriptor.ShaderRegister = 3;
        RootParams[5].Descriptor.RegisterSpace  = 0;
        RootParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE ShadowRange{};
        ShadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ShadowRange.NumDescriptors                    = 1;
        ShadowRange.BaseShaderRegister                = 11; 
        ShadowRange.RegisterSpace                     = 0;
        ShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[6].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[6].DescriptorTable.pDescriptorRanges   = &ShadowRange;
        RootParams[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE DDGIRanges[2]{};
        DDGIRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DDGIRanges[0].NumDescriptors                    = 2;
        DDGIRanges[0].BaseShaderRegister                = 12; 
        DDGIRanges[0].RegisterSpace                     = 0;
        DDGIRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        DDGIRanges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DDGIRanges[1].NumDescriptors                    = 1;
        DDGIRanges[1].BaseShaderRegister                = 15; 
        DDGIRanges[1].RegisterSpace                     = 0;
        DDGIRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[7].DescriptorTable.NumDescriptorRanges = 2;
        RootParams[7].DescriptorTable.pDescriptorRanges   = DDGIRanges;
        RootParams[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE AORange{};
        AORange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        AORange.NumDescriptors                    = 1;
        AORange.BaseShaderRegister                = 14; 
        AORange.RegisterSpace                     = 0;
        AORange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[8].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[8].DescriptorTable.pDescriptorRanges   = &AORange;
        RootParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // ReSTIR GI (t16): GITexture difusa por pixel. Tabela com 1 SRV; ligada so quando o ReSTIR
        // esta pronto (senao cai num fallback valido, como GI/AO). Os outros PSOs que compartilham
        // esta root sig (GBuffer/DepthNormal) nao referenciam t16 -> param extra e inofensivo.
        D3D12_DESCRIPTOR_RANGE ReSTIRGIRange{};
        ReSTIRGIRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ReSTIRGIRange.NumDescriptors                    = 1;
        ReSTIRGIRange.BaseShaderRegister                = 16;
        ReSTIRGIRange.RegisterSpace                     = 0;
        ReSTIRGIRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[9].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[9].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[9].DescriptorTable.pDescriptorRanges   = &ReSTIRGIRange;
        RootParams[9].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // Luzes puntuais (t17): StructuredBuffer<FGPULight> como ROOT SRV — endereco de GPU
        // direto, sem passar pelo descriptor heap. So o deferred lighting referencia t17; os
        // outros PSOs desta root sig nao acessam o registro, entao nao precisam do bind.
        RootParams[10].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        RootParams[10].Descriptor.ShaderRegister = 17;
        RootParams[10].Descriptor.RegisterSpace  = 0;
        RootParams[10].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // Sombras locais: t18 = atlas 2D dos spots (F3a), t19 = cube array dos points (F3b) —
        // descritores contiguos no heap (FLocalShadows aloca os 2 SRVs juntos).
        D3D12_DESCRIPTOR_RANGE LocalShadowRange{};
        LocalShadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        LocalShadowRange.NumDescriptors                    = 2;
        LocalShadowRange.BaseShaderRegister                = 18;
        LocalShadowRange.RegisterSpace                     = 0;
        LocalShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        RootParams[11].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[11].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[11].DescriptorTable.pDescriptorRanges   = &LocalShadowRange;
        RootParams[11].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC StaticSamplers[3]{};
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

        StaticSamplers[2].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        StaticSamplers[2].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSamplers[2].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSamplers[2].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        StaticSamplers[2].MaxAnisotropy    = 1;
        StaticSamplers[2].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        StaticSamplers[2].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        StaticSamplers[2].MinLOD           = 0.0f;
        StaticSamplers[2].MaxLOD           = D3D12_FLOAT32_MAX;
        StaticSamplers[2].ShaderRegister   = 2;
        StaticSamplers[2].RegisterSpace    = 0;
        StaticSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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

        RecreatePSO(_Device);
    }

    void FPipelineState::RecreatePSO(ID3D12Device* _Device) {
        auto VertexShaderBlob = LoadShaderBytecode("Triangle.vs_6_0.cso");

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

        D3D12_DEPTH_STENCIL_DESC DepthEqual{};
        DepthEqual.DepthEnable    = TRUE;
        DepthEqual.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        DepthEqual.DepthFunc      = D3D12_COMPARISON_FUNC_EQUAL;
        DepthEqual.StencilEnable  = FALSE;

        D3D12_DEPTH_STENCIL_DESC DepthLess{};
        DepthLess.DepthEnable    = TRUE;
        DepthLess.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthLess.DepthFunc      = kDepthFuncLess;
        DepthLess.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSignature.Get();
        PSODesc.VS                    = { VertexShaderBlob.data(), VertexShaderBlob.size() };
        PSODesc.PS                    = { nullptr, 0 };
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = RasterizerDesc;       // cull back
        PSODesc.InputLayout           = { InputLayout, _countof(InputLayout) };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        PSODesc.SampleDesc            = { 1, 0 };

        // Z-prepass (depth-only): escreve o depth opaco; o geometry pass usa depth EQUAL p/ matar
        // o overdraw no shader gordo do G-buffer (POM etc.). Sem cor (writemask 0, sem PS).
        D3D12_BLEND_DESC NoColor  = BlendDesc;
        NoColor.RenderTarget[0].RenderTargetWriteMask = 0;
        PSODesc.BlendState        = NoColor;
        PSODesc.DepthStencilState = DepthLess;                // escreve depth
        PSODesc.NumRenderTargets  = 1;
        PSODesc.RTVFormats[0]     = DXGI_FORMAT_R16G16B16A16_FLOAT;
        PSODesc.RTVFormats[1]     = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12PipelineState> NewPSODepth;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSODepth)));
        PipelineStateDepthOnly = NewPSODepth;

        auto NormalPSBlob = LoadShaderBytecode("DepthNormal.ps_6_0.cso");
        PSODesc.VS                = { VertexShaderBlob.data(), VertexShaderBlob.size() };
        PSODesc.PS                = { NormalPSBlob.data(), NormalPSBlob.size() };
        PSODesc.BlendState        = BlendDesc; 
        PSODesc.DepthStencilState = DepthLess; 
        PSODesc.RTVFormats[0]     = DXGI_FORMAT_R10G10B10A2_UNORM;
        PSODesc.SampleDesc        = { 1, 0 };
        ComPtr<ID3D12PipelineState> NewPSODepthNormal;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSODepthNormal)));
        PipelineStateDepthNormal = NewPSODepthNormal;

        // Variantes masked/two-sided do prepass (GTAO F4): cull NONE (cards de folhagem) + PS
        // com alpha-clip condicional (AlphaTest=0 em two-sided opaco -> so rasteriza os 2 lados).
        auto DepthNormalMaskedPSBlob = LoadShaderBytecode("DepthNormalMasked.ps_6_0.cso");
        auto DepthMaskedPSBlob       = LoadShaderBytecode("DepthMasked.ps_6_0.cso");

        D3D12_RASTERIZER_DESC RasterizerNone = RasterizerDesc;
        RasterizerNone.CullMode = D3D12_CULL_MODE_NONE;

        PSODesc.RasterizerState = RasterizerNone;
        PSODesc.PS              = { DepthNormalMaskedPSBlob.data(), DepthNormalMaskedPSBlob.size() };
        ComPtr<ID3D12PipelineState> NewPSODepthNormalMasked;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSODepthNormalMasked)));
        PipelineStateDepthNormalMasked = NewPSODepthNormalMasked;

        PSODesc.PS            = { DepthMaskedPSBlob.data(), DepthMaskedPSBlob.size() };
        PSODesc.BlendState    = NoColor;
        PSODesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ComPtr<ID3D12PipelineState> NewPSODepthOnlyMasked;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSODepthOnlyMasked)));
        PipelineStateDepthOnlyMasked = NewPSODepthOnlyMasked;

        // --- Geometry pass do deferred: escreve o G-buffer (MRT 3 RTs) -------------------------
        // Opaco: depth EQUAL (reusa o depth do prepass). Two-sided/alpha-test (folhagem): nao
        // entram no prepass, entao escrevem o proprio depth (depth LESS + write).
        // Dieta: +2 MRTs alem do G-buffer — velocity e o SceneColor HDR, onde o emissivo e
        // escrito DIRETO (o deferred lighting soma a luz por cima com blend aditivo).
        auto GBufferPSBlob = LoadShaderBytecode("GBuffer.ps_6_0.cso");
        PSODesc.VS                = { VertexShaderBlob.data(), VertexShaderBlob.size() };
        PSODesc.PS                = { GBufferPSBlob.data(), GBufferPSBlob.size() };
        PSODesc.BlendState        = BlendDesc;     // opaco, escreve todos os canais
        PSODesc.DepthStencilState = DepthEqual;    // le o depth do prepass, nao escreve
        PSODesc.NumRenderTargets  = FGBuffer::kTargetCount + 2; // +velocity +SceneColor (emissivo)
        PSODesc.RTVFormats[0]     = FGBuffer::kFormatA;
        PSODesc.RTVFormats[1]     = FGBuffer::kFormatB;
        PSODesc.RTVFormats[2]     = FGBuffer::kFormatC;
        PSODesc.RTVFormats[3]     = DXGI_FORMAT_R16G16_FLOAT; // SV_Target3 = motion vector (UV)
        PSODesc.RTVFormats[4]     = DXGI_FORMAT_R16G16B16A16_FLOAT; // SV_Target4 = emissivo -> HDR
        PSODesc.DSVFormat         = DXGI_FORMAT_D32_FLOAT;
        PSODesc.SampleDesc        = { 1, 0 };

        RasterizerDesc.CullMode   = D3D12_CULL_MODE_BACK;
        PSODesc.RasterizerState   = RasterizerDesc;
        ComPtr<ID3D12PipelineState> NewPSOGBuffer;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSOGBuffer)));
        PipelineStateGBuffer = NewPSOGBuffer;

        RasterizerDesc.CullMode   = D3D12_CULL_MODE_NONE; // folhagem / alpha-test two-sided
        PSODesc.RasterizerState   = RasterizerDesc;
        // Masked/two-sided agora TAMBEM entram no prepass (F4): o depth deles ja existe, entao
        // LESS_EQUAL p/ passar no empate (LESS estrito descartava tudo); write mantido p/ o
        // caso do prepass desligado.
        D3D12_DEPTH_STENCIL_DESC DepthLessEqual = DepthLess;
        DepthLessEqual.DepthFunc  = kDepthFuncLessEqual;
        PSODesc.DepthStencilState = DepthLessEqual;
        ComPtr<ID3D12PipelineState> NewPSOGBufferTwoSided;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSOGBufferTwoSided)));
        PipelineStateGBufferTwoSided = NewPSOGBufferTwoSided;

        // --- Forward de translucidos (materiais Blend: vidro): sobre o HDR ja iluminado. -------
        // Blend PREMULTIPLICADO (SrcBlend=ONE): o shader pesa o tinte por alpha e soma o especular
        // inteiro. Depth read-only (testa contra o opaco, nao escreve) e cull none (two-sided).
        auto ForwardBlendPSBlob = LoadShaderBytecode("ForwardBlend.ps_6_0.cso");

        D3D12_BLEND_DESC PremulBlend = BlendDesc;
        PremulBlend.RenderTarget[0].BlendEnable    = TRUE;
        PremulBlend.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
        PremulBlend.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        PremulBlend.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        PremulBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;

        D3D12_DEPTH_STENCIL_DESC DepthReadOnly = DepthLess;
        DepthReadOnly.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        PSODesc.PS                = { ForwardBlendPSBlob.data(), ForwardBlendPSBlob.size() };
        PSODesc.BlendState        = PremulBlend;
        PSODesc.DepthStencilState = DepthReadOnly;
        PSODesc.NumRenderTargets  = 1;
        PSODesc.RTVFormats[0]     = DXGI_FORMAT_R16G16B16A16_FLOAT;
        PSODesc.RTVFormats[1]     = DXGI_FORMAT_UNKNOWN;
        PSODesc.RTVFormats[2]     = DXGI_FORMAT_UNKNOWN;
        PSODesc.RTVFormats[3]     = DXGI_FORMAT_UNKNOWN;
        PSODesc.RTVFormats[4]     = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12PipelineState> NewPSOForwardBlend;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&NewPSOForwardBlend)));
        PipelineStateForwardBlend = NewPSOForwardBlend;

        // --- Deferred lighting: fullscreen (PostProcess.vs), le o G-buffer e ilumina -> HDR. ----
        // Usa a root signature principal (a tabela de material e religada p/ [A,B,C,Depth]).
        // Blend ADITIVO na cor (o emissivo ja esta no SceneColor via geometry pass) com alpha
        // sobrescrito p/ 1. A variante OPACA e usada nos modos de debug (SSAO/GI view), onde o
        // shader retorna a visualizacao inteira e nao pode somar com o emissivo por baixo.
        auto LightVSBlob = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto LightPSBlob = LoadShaderBytecode("DeferredLighting.ps_6_0.cso");

        D3D12_BLEND_DESC LightAdditive = BlendDesc;
        LightAdditive.RenderTarget[0].BlendEnable    = TRUE;
        LightAdditive.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
        LightAdditive.RenderTarget[0].DestBlend      = D3D12_BLEND_ONE;
        LightAdditive.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        LightAdditive.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

        D3D12_RASTERIZER_DESC LightRaster{};
        LightRaster.FillMode        = D3D12_FILL_MODE_SOLID;
        LightRaster.CullMode        = D3D12_CULL_MODE_NONE;
        LightRaster.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC LightDepth{};
        LightDepth.DepthEnable   = FALSE;
        LightDepth.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC LightDesc{};
        LightDesc.pRootSignature        = RootSignature.Get();
        LightDesc.VS                    = { LightVSBlob.data(), LightVSBlob.size() };
        LightDesc.PS                    = { LightPSBlob.data(), LightPSBlob.size() };
        LightDesc.BlendState            = LightAdditive;
        LightDesc.SampleMask            = UINT_MAX;
        LightDesc.RasterizerState       = LightRaster;
        LightDesc.DepthStencilState     = LightDepth;
        LightDesc.InputLayout           = { nullptr, 0 };
        LightDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        LightDesc.NumRenderTargets      = 1;
        LightDesc.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        LightDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        LightDesc.SampleDesc            = { 1, 0 };
        ComPtr<ID3D12PipelineState> NewPSODeferredLighting;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&LightDesc, IID_PPV_ARGS(&NewPSODeferredLighting)));
        PipelineStateDeferredLighting = NewPSODeferredLighting;

        LightDesc.BlendState = BlendDesc; // opaco: debug views substituem a tela
        ComPtr<ID3D12PipelineState> NewPSODeferredLightingDebug;
        SMILE_HR(_Device->CreateGraphicsPipelineState(&LightDesc, IID_PPV_ARGS(&NewPSODeferredLightingDebug)));
        PipelineStateDeferredLightingDebug = NewPSODeferredLightingDebug;
    }
}
