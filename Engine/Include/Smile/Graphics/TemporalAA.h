#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Temporal Anti-Aliasing (Fase 1: reprojecao so-camera, sem motion vectors).
    //
    // Acumula a cor HDR resolvida (pre-tonemap) ao longo dos frames, com jitter de
    // projecao (Halton) aplicado no Renderer. Mata o serrilhado geometrico E o shimmer
    // temporal da DDGI (o blend tonemap-weighted do shader pesa firefly/probe-noise por
    // 1/(1+luma)). Plugado entre o fog e o bloom; o output vira a entrada do post-process.
    //
    // Ping-pong de 2 history buffers RGBA16F: o frame N le history[N-1] e escreve history[N].
    // Seguro com 2 frames-in-flight pelas transicoes PSR<->RT (a leitura do frame anterior
    // termina antes do write deste frame por causa do barrier PSR->RT).
    class FTemporalAA {
    public:
        struct alignas(256) TAAConstants {
            Mat44 InvViewProj;   // atual, FULL (com translacao)
            Mat44 PrevViewProj;  // frame anterior, FULL
            Vec4  Params0;       // xy = 1/screenSize, z = history blend (0..1), w = history valido (0/1)
            Vec4  Params1;       // x = variance gamma, y = sharpness, z = debug mode, w = motion blend
            Vec4  Params2;       // x = anti-flicker (clamp do atual a caixa), yzw = reservado
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        // Recria os history buffers + RTVs + SRVs individuais (chamado no resize).
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        // (Re)constroi as tabelas de entrada do resolve [HDR(t0), history-prev(t1), depth(t2)].
        // Chamar apos (re)criar o HDR color buffer e o depth (Initialize/Resize do Renderer).
        void SetupInputs(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                         ID3D12Resource* HDRInput, ID3D12Resource* Depth);

        // Roda o resolve: escreve history[curr] a partir do HDR atual + history[prev] + depth.
        // O HDR e o depth precisam estar legiveis (PIXEL_SHADER_RESOURCE) na entrada.
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     u32 FrameSlot, const Mat44& InvViewProj, const Mat44& PrevViewProj,
                     f32 HistoryBlend, bool HistoryValid, f32 VarianceGamma, f32 Sharpness,
                     f32 MotionBlend, f32 AntiFlicker, u32 DebugMode);

        // Output do ultimo Execute (history[curr]) — entrada do post-process. Fica em
        // PIXEL_SHADER_RESOURCE apos Execute.
        ID3D12Resource* OutputResource() const { return History[LastOutputIndex].Get(); }
        u32             OutputSRVSlot() const  { return HistorySRVBase + LastOutputIndex; }

        bool IsInitialized() const { return Initialized; }

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateTextures(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ResolvePSO;

        // 2 history buffers RGBA16F (ping-pong). Estado rastreado p/ as transicoes PSR<->RT.
        Microsoft::WRL::ComPtr<ID3D12Resource> History[2];
        D3D12_RESOURCE_STATES                  HistoryState[2] = {
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        FDescriptorHeap                        HistoryRTVHeap; // 2 RTVs

        // SRV individual por history buffer (entrada do post-process). 2 slots contiguos.
        u32 HistorySRVBase = kInvalidSlot;
        // Tabelas do resolve, por paridade de curr: [HDR(t0), history[1-curr](t1), depth(t2)].
        // 2 tabelas x 3 slots contiguos. ResolveTableBase[i] usado quando curr == i.
        u32 ResolveTableBase = kInvalidSlot; // bloco de 6; tabela i comeca em base + i*3

        Microsoft::WRL::ComPtr<ID3D12Resource> CB; // kFramesInFlight copias de TAAConstants
        u8* MappedCB = nullptr;

        u32  Width = 0, Height = 0;
        u32  HistoryIndex   = 0; // curr deste frame (alterna a cada Execute)
        u32  LastOutputIndex = 0; // history escrito no ultimo Execute (output p/ o bloom)
        bool Initialized = false;
    };
}
