#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DebugTargets.h"

namespace Smile {
    class FTextureSRVHeap;

    // Um retangulo do visualizador. ChannelWeight e o truque do r_ShowRenderTarget da Cry: um
    // Vec4 multiplicador cobre "isolar canal", "escalar valor" e "separar canais em tiles"
    // sem precisar de modo proprio p/ cada caso.
    struct FDebugTile {
        u32          SrvSlot       = 0xFFFFFFFFu;
        Vec4         ChannelWeight = Vec4{ 1.0f, 1.0f, 1.0f, 1.0f };
        EDebugDecode Decode        = EDebugDecode::Raw;
        u32          SubIndex      = 0;      // campo do G-buffer, quando Decode == GBufferField
        u32          Mip           = 0;
        u32          AtlasTilePx   = 0;     // > 0: reempacota tiles de atlas (DDGI) em grade
        f32          Exposure      = 1.0f;   // usado por Decode::HDR
        f32          NearZ         = 0.1f;    // usados por Decode::ReverseZ p/ linearizar
        f32          FarZ          = 4000.0f;
        bool         LinearFilter  = true;   // false preserva payload HDR/metadados por texel
        // TileAspect e calculado pelo passe (depende da grade), nao vem daqui.
    };

    // Visualizador generico de render targets. Substitui o FGBufferDebug: os 8 modos antigos
    // viram tiles com Decode::GBufferField + SubIndex, entao a logica de decode do G-buffer
    // MIGRA (continua vindo do GBuffer.hlsli), nao e reescrita.
    //
    // A API ja aceita N tiles e um numero de colunas mesmo que a fase 1 use um tile so em tela
    // cheia. E de proposito: a janela de debug com varios alvos simultaneos nao vai exigir
    // refatorar este passe — so passar mais tiles.
    class FDebugView {
    public:
        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        bool IsInitialized() const { return Initialized; }

        // Desenha os tiles no render target JA setado pelo chamador. Columns == 1 (ou Count == 1)
        // ocupa a tela toda; Columns > 1 monta grade, um triangulo fullscreen por tile com o
        // viewport recortado — sem matematica de tiling no shader.
        // GBufferTableStart/VelocitySRVSlot sao passados sempre: o decode GBufferField precisa
        // dos 3 MRTs e o Velocity precisa do seu proprio SRV, independente do alvo do tile.
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     const FDebugTile* Tiles, u32 Count, u32 Columns,
                     u32 GBufferTableStart, u32 VelocitySRVSlot,
                     const D3D12_VIEWPORT& FullViewport,
                     const Vec2& ScreenUvOffset, bool EncodeForDisplay);

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device, DXGI_FORMAT RTFormat);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        bool Initialized = false;
    };
}
