#include "Smile/Graphics/DebugView.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace Smile {

    namespace {
        constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
    }

    void FDebugView::Initialize(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        if (Initialized) return;
        BuildRootSignature(_Device);
        BuildPSO(_Device, _RTFormat);
        Initialized = true;
        LogDebug("Visualizador de render targets (FDebugView) inicializado");
    }

    void FDebugView::BuildRootSignature(ID3D12Device* _Device) {
        // t0 = alvo generico do tile (o que se esta olhando).
        D3D12_DESCRIPTOR_RANGE TargetRange{};
        TargetRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        TargetRange.NumDescriptors                    = 1;
        TargetRange.BaseShaderRegister                = 0;
        TargetRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t1..t3 = os 3 MRTs contiguos do G-buffer (so o decode GBufferField usa).
        D3D12_DESCRIPTOR_RANGE GBufRange{};
        GBufRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        GBufRange.NumDescriptors                    = 3;
        GBufRange.BaseShaderRegister                = 1;
        GBufRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t4 = motion vector (slot proprio, nao contiguo ao G-buffer).
        D3D12_DESCRIPTOR_RANGE VelRange{};
        VelRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        VelRange.NumDescriptors                    = 1;
        VelRange.BaseShaderRegister                = 4;
        VelRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // t5 = o MESMO descritor de t0, visto como Texture2DArray (decode ArraySlice). Duas
        // entradas para um descritor so porque a dimensao da view faz parte da declaracao no
        // HLSL — nao existe SRV de dimensao TEXTURE2D que enderece array slice, entao alvo de
        // array precisa de uma declaracao propria. Cada tile le exatamente uma das duas: o
        // shader ramifica no Decode antes de tocar em qualquer das texturas.
        D3D12_DESCRIPTOR_RANGE ArrayRange{};
        ArrayRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ArrayRange.NumDescriptors                    = 1;
        ArrayRange.BaseShaderRegister                = 5;
        ArrayRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[5]{};
        // b0: decode/tile | pesos | exposicao/aspecto | offset UV/display
        RootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        RootParams[0].Constants.ShaderRegister = 0;
        RootParams[0].Constants.Num32BitValues = 16;
        RootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &TargetRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &GBufRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &VelRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[4].DescriptorTable.pDescriptorRanges   = &ArrayRange;
        RootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

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
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("DebugView root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&RootSig)));
    }

    void FDebugView::BuildPSO(ID3D12Device* _Device, DXGI_FORMAT _RTFormat) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PS = LoadShaderBytecode("DebugView.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
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

    void FDebugView::Execute(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap,
                             const FDebugTile* _Tiles, u32 _Count, u32 _Columns,
                             u32 _GBufferTableStart, u32 _VelocitySRVSlot,
                             const D3D12_VIEWPORT& _FullViewport,
                             const Vec2& _ScreenUvOffset, bool _EncodeForDisplay) {
        if (!Initialized || !_Tiles || _Count == 0) return;

        _CommandList->SetGraphicsRootSignature(RootSig.Get());
        _CommandList->SetPipelineState(PSO.Get());
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);

        if (_GBufferTableStart != kInvalidSlot)
            _CommandList->SetGraphicsRootDescriptorTable(2, _SRVHeap.GpuHandle(_GBufferTableStart));
        if (_VelocitySRVSlot != kInvalidSlot)
            _CommandList->SetGraphicsRootDescriptorTable(3, _SRVHeap.GpuHandle(_VelocitySRVSlot));

        // Grade quadrada o suficiente p/ caber Count tiles; Columns=0 deixa o passe decidir.
        const u32 Cols = _Columns > 0 ? _Columns
                       : static_cast<u32>(std::ceil(std::sqrt(static_cast<f32>(_Count))));
        const u32 Rows = (_Count + Cols - 1) / Cols;
        const f32 TileW = _FullViewport.Width  / static_cast<f32>(Cols);
        const f32 TileH = _FullViewport.Height / static_cast<f32>(Rows);

        for (u32 I = 0; I < _Count; ++I) {
            const FDebugTile& T = _Tiles[I];
            if (T.SrvSlot == kInvalidSlot) continue;

            // Um triangulo fullscreen por tile, com o viewport recortado no retangulo do tile.
            // O shader continua enxergando UV [0,1] — nenhuma matematica de grade nele.
            D3D12_VIEWPORT VP{};
            VP.TopLeftX = _FullViewport.TopLeftX + static_cast<f32>(I % Cols) * TileW;
            VP.TopLeftY = _FullViewport.TopLeftY + static_cast<f32>(I / Cols) * TileH;
            VP.Width    = TileW;
            VP.Height   = TileH;
            VP.MinDepth = 0.0f;
            VP.MaxDepth = 1.0f;

            D3D12_RECT Scissor{};
            Scissor.left   = static_cast<LONG>(VP.TopLeftX);
            Scissor.top    = static_cast<LONG>(VP.TopLeftY);
            Scissor.right  = static_cast<LONG>(VP.TopLeftX + VP.Width);
            Scissor.bottom = static_cast<LONG>(VP.TopLeftY + VP.Height);

            _CommandList->RSSetViewports(1, &VP);
            _CommandList->RSSetScissorRects(1, &Scissor);

            struct {
                u32 Decode, SubIndex, Mip, AtlasTilePx;
                f32 Cw[4];
                f32 Exposure, NearZ, FarZ, TileAspect;
                f32 ScreenUvOffset[2];
                u32 EncodeForDisplay, LinearFilter;
            } K{};
            K.Decode   = static_cast<u32>(T.Decode);
            K.SubIndex = T.SubIndex;
            K.Mip      = T.Mip;
            K.AtlasTilePx = T.AtlasTilePx;
            K.Cw[0] = T.ChannelWeight.X; K.Cw[1] = T.ChannelWeight.Y;
            K.Cw[2] = T.ChannelWeight.Z; K.Cw[3] = T.ChannelWeight.W;
            K.Exposure = T.Exposure;
            K.NearZ    = T.NearZ;
            K.FarZ     = T.FarZ;
            K.TileAspect = TileH > 0.0f ? TileW / TileH : 1.0f;
            K.ScreenUvOffset[0] = _ScreenUvOffset.X;
            K.ScreenUvOffset[1] = _ScreenUvOffset.Y;
            K.EncodeForDisplay  = _EncodeForDisplay ? 1u : 0u;
            K.LinearFilter      = T.LinearFilter ? 1u : 0u;

            _CommandList->SetGraphicsRoot32BitConstants(0, 16, &K, 0);
            // t0 e t5 recebem o MESMO descritor: qual dos dois o shader le e decidido pelo
            // Decode. Ligar os dois sempre (em vez de so o que sera usado) evita ter de manter
            // um recurso dummy vivo aqui so para o outro slot nao ficar sem binding — a tabela
            // nao lida nunca e acessada, porque a ramificacao acontece antes.
            _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(T.SrvSlot));
            _CommandList->SetGraphicsRootDescriptorTable(4, _SRVHeap.GpuHandle(T.SrvSlot));
            _CommandList->DrawInstanced(3, 1, 0, 0);
        }

        // Devolve o viewport cheio: o resto do frame conta com ele.
        D3D12_RECT FullScissor{
            static_cast<LONG>(_FullViewport.TopLeftX),
            static_cast<LONG>(_FullViewport.TopLeftY),
            static_cast<LONG>(_FullViewport.TopLeftX + _FullViewport.Width),
            static_cast<LONG>(_FullViewport.TopLeftY + _FullViewport.Height)
        };
        _CommandList->RSSetViewports(1, &_FullViewport);
        _CommandList->RSSetScissorRects(1, &FullScissor);
    }
}
