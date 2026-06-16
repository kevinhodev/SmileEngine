#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <memory>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace Smile {
    // Wrapper do AMD FidelityFX-FSR2 (upscaler/AA temporal). Esconde os headers do SDK (ffx_*)
    // atras de um pimpl para nao vazar para o resto da engine. Linkado apenas em Release (o SDK
    // foi buildado com CRT /MD); em Debug compila como stub (SMILE_FSR2_ENABLED=0), entao a engine
    // Debug continua usando o TAA custom. Ver memory/ref-fsr2-integration.md.
    class FFsr2Pass {
    public:
        FFsr2Pass();
        ~FFsr2Pass();
        FFsr2Pass(const FFsr2Pass&)            = delete;
        FFsr2Pass& operator=(const FFsr2Pass&) = delete;

        // Cria (ou recria) o contexto FSR2 e a textura de output (display-res, UAV+SRV no heap da
        // engine). RenderW/H = resolucao de entrada; DisplayW/H = saida. Fase 2: iguais (sem upscale).
        // Idempotente: destroi o contexto anterior. Retorna false em Debug/stub.
        bool Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH);
        void Shutdown();
        bool IsInitialized() const;

        // Offset de jitter do FSR2 (em pixels, faixa ~[-0.5,0.5]) para alimentar a matriz de
        // projecao E o dispatch. Substitui o Halton da engine. FrameIndex monotonico crescente.
        void GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const;

        // Roda o FSR2: le Color/Depth/Velocity (que o CHAMADOR deve ter posto em
        // NON_PIXEL_SHADER_RESOURCE = COMPUTE_READ) e escreve na textura de output, que e
        // gerenciada aqui (UAV durante o dispatch, PIXEL_SHADER_RESOURCE no fim p/ o post chain).
        // JitterX/Y = o MESMO jitter aplicado a projecao. Reset = corte de camera / 1o frame.
        void Dispatch(ID3D12GraphicsCommandList* Cmd,
                      ID3D12Resource* Color, ID3D12Resource* Depth, ID3D12Resource* Velocity,
                      f32 JitterX, f32 JitterY,
                      f32 NearZ, f32 FarZ, f32 FovYRadians,
                      f32 DeltaTimeSec, bool Reset);

        ID3D12Resource* OutputResource() const; // cor reconstruida (display-res, em PIXEL_SHADER_RESOURCE)
        u32             OutputSRVSlot() const;  // slot no heap da engine p/ o post chain ler

        u32 RenderW()  const;
        u32 RenderH()  const;
        u32 DisplayW() const;
        u32 DisplayH() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> P;
    };
}
