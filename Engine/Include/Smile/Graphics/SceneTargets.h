#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/ComputePipeline.h"

// Render targets da CENA — os que vivem em RenderWidth/Height e sao recriados juntos no
// resize e na troca de render scale. Nao entram aqui: os alvos dos subsistemas (que cada um
// possui e redimensiona por conta propria, via Resize/SetupForResize) nem os do visualizador
// de debug, que sao ferramenta e tem lifecycle proprio.
//
// Deliberadamente um AGREGADO de membros publicos, nao uma classe com getters. Isto e um saco
// de recursos, e a gravacao do frame precisa dos ComPtr e dos D3D12_RESOURCE_STATES crus para
// montar barreira. Envolver cada um num acessor trocaria ~300 usos diretos por ~300 chamadas
// sem ganhar invariante nenhuma — o estado de recurso e, por natureza, escrito de fora, pelo
// codigo que emite a transicao.
namespace Smile {

    class FTextureSRVHeap;

    struct FSceneTargets {
        static constexpr u32 kInvalidSlot      = 0xFFFFFFFFu;
        static constexpr u32 kSceneColorMipMax = 16;

        // Individuais, e nao um Recreate() unico, porque a ORDEM difere entre os dois chamadores:
        // no Initialize o depth e as normais nascem antes do G-buffer; no RecreateInternalTargets
        // o HDR vem primeiro e o G-buffer se intercala. Bundlar aqui exigiria escolher uma das
        // duas ordens, e esta etapa e de extracao, nao de mudanca de comportamento. Unificar
        // depois, se alguem verificar que a ordem e livre.
        void CreateDepthBuffer(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H);
        void CreateNormalBuffer(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H);
        void CreateHDRBuffers(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H);
        void CreateVelocityBuffer(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H);
        void CreateUpscaleMasks(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H);
        void CreateSceneCopies(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H);

        // --- Profundidade ---------------------------------------------------------------
        ComPtr<ID3D12Resource> DepthBuffer;
        FDescriptorHeap        DSVHeap;
        u32                    DepthSRVSlot = kInvalidSlot;

        // --- Normais (prepass) ----------------------------------------------------------
        ComPtr<ID3D12Resource> NormalBuffer;
        FDescriptorHeap        NormalRTVHeap;
        u32                    NormalSRVSlot     = kInvalidSlot;
        D3D12_RESOURCE_STATES  NormalBufferState = D3D12_RESOURCE_STATE_COMMON;

        // --- Motion vectors -------------------------------------------------------------
        // RG16F escrito no geometry pass (SV_Target3), lido pelo TAA. RT proprio (lifecycle
        // desacoplado das transicoes do GBuffer, que fazem ping-pong p/ as reflexoes).
        // Velocidade em UV = curUV(sem jitter) - prevUV.
        ComPtr<ID3D12Resource> VelocityBuffer;
        FDescriptorHeap        VelocityRTVHeap;   // 1 RTV
        u32                    VelocitySRVSlot = kInvalidSlot;
        u32                    VelocityUavSlot = kInvalidSlot; // passe de velocity do background
        D3D12_RESOURCE_STATES  VelocityState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // --- Mascaras do upscaler -------------------------------------------------------
        // FSR 3.1: sinais render-res de conteudo sem historico confiavel. Sao MRTs da agua e do
        // forward transparente; permanecem RT durante a cena e viram COMPUTE_READ no dispatch.
        ComPtr<ID3D12Resource> UpscaleReactiveMask;
        ComPtr<ID3D12Resource> UpscaleCompositionMask;
        FDescriptorHeap        UpscaleMaskRTVHeap; // [0]=reactive, [1]=composition
        D3D12_RESOURCE_STATES  UpscaleReactiveState    = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_STATES  UpscaleCompositionState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        // --- Cor HDR da cena ------------------------------------------------------------
        ComPtr<ID3D12Resource> HDRColorBuffer;
        FDescriptorHeap        HDRRTVHeap;
        u32                    HDRSRVSlot = kInvalidSlot;

        // --- Copias p/ refracao e SSR de contato ----------------------------------------
        // Mip 0 preserva a cena HDR sem agua; os demais formam a piramide de radiancia usada
        // pelo SSR de contato para integrar o footprint do lobo especular.
        ComPtr<ID3D12Resource> SceneColorCopy;
        ComPtr<ID3D12Resource> SceneDepthCopy;
        u32                    SceneCopyTableStart   = kInvalidSlot;
        u32                    SceneColorMipCount    = 1;
        u32                    SceneColorMipSRVStart = kInvalidSlot;
        u32                    SceneColorMipUAVStart = kInvalidSlot;
        D3D12_RESOURCE_STATES  SceneColorMipStates[kSceneColorMipMax]{};
        FComputePipeline       SceneColorMipPSO;
        D3D12_RESOURCE_STATES  SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    };
}
