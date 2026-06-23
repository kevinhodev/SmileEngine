#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Debug view: heatmap de flicker temporal (variancia por pixel) em tempo real.
    //
    // Porte do que era feito offline (std temporal por pixel num video) p/ a engine. Mantem uma
    // EMA de luma e luma^2 num buffer persistente (Stats); std = sqrt(E[x^2]-E[x]^2) -> colormap.
    // Pass fullscreen que LE a cor HDR (saida do TAA) via SRV e ESCREVE o heatmap num Output RT
    // proprio (que vira a entrada do bloom/tonemap quando ligado). So roda quando Mode > 0.
    class FFlickerHeatmap {
    public:
        struct alignas(256) FlickerConstants {
            f32 Alpha;     // peso do frame atual na EMA
            f32 HeatScale; // std -> [0,1]
            f32 Mode;      // 1 = overlay, 2 = puro
            f32 Reset;     // 1 = reinicia a EMA
            u32 ScrW;
            u32 ScrH;
            u32 Pad0, Pad1;
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        // Le a cor de entrada (InputSRVSlot, em PIXEL_SHADER_RESOURCE) e escreve o heatmap no
        // Output (deixado em PIXEL_SHADER_RESOURCE p/ o bloom). Reset reinicia a EMA (1 frame).
        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     u32 InputSRVSlot, f32 Mode, f32 HeatScale, f32 Alpha, bool Reset,
                     u32 Width, u32 Height);

        ID3D12Resource* OutputResource() const { return Output.Get(); }
        u32             OutputSRVSlot() const  { return OutputSRVSlot_; }
        bool IsInitialized() const { return Initialized; }

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device);
        void CreateBuffers(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;

        // Output RGBA16F (RTV + SRV) — recebe o heatmap, vira a entrada do post-process.
        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        FDescriptorHeap                        OutputRTVHeap;
        u32 OutputSRVSlot_ = kInvalidSlot;
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // Stats RG32F (UAV) — EMA persistente de (mean, meanSq) de luma. Fica sempre em UAV.
        Microsoft::WRL::ComPtr<ID3D12Resource> Stats;
        u32 StatsUAVSlot = kInvalidSlot;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;

        bool Initialized = false;
    };
}
