#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/Barriers.h"
#include "Smile/Graphics/DescriptorHeap.h"

namespace Smile {
    class FTextureSRVHeap;

    // G-Buffer do deferred shading: 3 render targets + depth (externo).
    // Layout pos-dieta (ver Shaders/GBuffer.hlsli, manter em sincronia):
    //   A  R8G8B8A8_UNORM     .rgb = BaseColor   .a = AO
    //   B  R10G10B10A2_UNORM  .rg  = OctNormal   .b = Roughness  .a = ShadingModelID (2 bits)
    //   C  R8G8B8A8_UNORM     .r   = Metallic    .gba = livres (subsurface/specular futuros)
    // Emissivo NAO mora aqui: o geometry pass escreve direto no SceneColor HDR (5o MRT) e o
    // deferred lighting soma a luz por cima (blend aditivo).
    //
    // Os 3 SRVs sao alocados CONTIGUOS no heap compartilhado (SRVTableStart()) p/ bind do passe
    // de iluminacao como uma unica descriptor table. O depth fica com o Renderer.
    class FGBuffer {
    public:
        static constexpr u32 kTargetCount = 3;

        static constexpr DXGI_FORMAT kFormatA = DXGI_FORMAT_R8G8B8A8_UNORM;     // BaseColor + AO
        static constexpr DXGI_FORMAT kFormatB = DXGI_FORMAT_R10G10B10A2_UNORM; // OctNormal + Rough + ID
        static constexpr DXGI_FORMAT kFormatC = DXGI_FORMAT_R8G8B8A8_UNORM;    // Metallic + canais livres

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Shutdown();

        bool IsInitialized() const { return Targets[0] != nullptr; }

        static DXGI_FORMAT Format(u32 Index) {
            return Index == 0 ? kFormatA : (Index == 1 ? kFormatB : kFormatC);
        }

        // RTVs contiguos num heap proprio (nao shader-visivel). Use RTVHandle(0) como base do array.
        D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle(u32 Index) const { return RTVHeap.CpuHandle(Index); }

        // Primeiro slot SRV; A,B,C ficam contiguos, e o depth (escrito por WriteDepthSRV) ocupa o
        // 4o slot logo apos — assim o deferred lighting liga [A,B,C,Depth] como UMA tabela (t0-t3).
        u32 SRVTableStart() const   { return SRVSlotBase; }
        u32 SRVSlot(u32 Index) const { return SRVSlotBase + Index; }
        u32 DepthSRVSlot() const     { return SRVSlotBase + kTargetCount; }

        // Cria/atualiza o SRV (R32_FLOAT) do depth no 4o slot contiguo. Chamar apos (re)criar o
        // depth buffer, p/ a tabela do lighting ficar [A,B,C,Depth].
        void WriteDepthSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, ID3D12Resource* Depth);

        ID3D12Resource* Resource(u32 Index) const { return Targets[Index].Get(); }

        // Transicoes (rastreiam o estado interno por-target). Read = combinacao de estados de leitura.
        // AppendTransitions empilha num batch compartilhado (transicoes vizinhas de outros
        // recursos saem no MESMO ResourceBarrier); os To* emitem na hora, num batch proprio.
        void AppendTransitions(FBarrierBatch& Batch, D3D12_RESOURCE_STATES Target);
        void TransitionToRead(ID3D12GraphicsCommandList* Cmd, D3D12_RESOURCE_STATES ReadState);
        void TransitionToWrite(ID3D12GraphicsCommandList* Cmd);

        // Limpa os 3 targets com os valores canonicos do layout (chame apos OMSetRenderTargets).
        void Clear(ID3D12GraphicsCommandList* Cmd) const;

    private:
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        void CreateTargets(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        ComPtr<ID3D12Resource> Targets[kTargetCount];
        FDescriptorHeap        RTVHeap;
        u32                    SRVSlotBase = kInvalidSlot;
        D3D12_RESOURCE_STATES  States[kTargetCount] = {};
        u32                    Width  = 0;
        u32                    Height = 0;
    };
}
