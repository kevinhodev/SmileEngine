#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <memory>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace Smile {
    // Wrapper do AMD FidelityFX Super Resolution (upscaler/AA temporal) sobre o ffx-api unificado do
    // FidelityFX SDK (upscaler FSR 3.1). Esconde os headers do SDK (ffx_api/*) atras de um pimpl para
    // nao vazar para o resto da engine. O ffx-api e uma C-ABI atraves de fronteira de DLL
    // (amd_fidelityfx_dx12.dll + import lib), entao roda igual em Debug e Release — sem o antigo
    // conflito de CRT que forcava stub em Debug. Se o SDK nao for achado no CMake, compila como stub
    // (SMILE_FSR_ENABLED=0) e a engine cai no TAA custom.
    class FFsrPass {
    public:
        FFsrPass();
        ~FFsrPass();
        FFsrPass(const FFsrPass&)            = delete;
        FFsrPass& operator=(const FFsrPass&) = delete;

        // Cria (ou recria) o contexto do upscaler e a textura de output (display-res, UAV+SRV no heap da
        // engine). RenderW/H = resolucao de entrada; DisplayW/H = saida.
        // Idempotente: destroi o contexto anterior. Retorna false quando compilado como stub.
        bool Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH);
        void Shutdown();
        bool IsInitialized() const;

        // Offset de jitter do upscaler (em pixels, faixa ~[-0.5,0.5]) para alimentar a matriz de
        // projecao E o dispatch. Substitui o Halton da engine. FrameIndex monotonico crescente.
        void GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const;

        // Roda o upscaler: le Color/Depth/Velocity (que o CHAMADOR deve ter posto em
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
