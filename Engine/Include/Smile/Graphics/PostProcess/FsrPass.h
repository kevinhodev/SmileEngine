#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RHI/TextureSRVHeap.h"
#include "Smile/Graphics/PostProcess/Upscaler.h"
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
    // (SMILE_FSR_ENABLED=0) e a engine cai no TAA custom. Implementa IUpscaler (ver Upscaler.h).
    class FFsrPass final : public IUpscaler {
    public:
        FFsrPass();
        ~FFsrPass() override;
        FFsrPass(const FFsrPass&)            = delete;
        FFsrPass& operator=(const FFsrPass&) = delete;

        // Cria (ou recria) o contexto do upscaler e a textura de output (display-res, UAV+SRV no heap da
        // engine). RenderW/H = resolucao de entrada; DisplayW/H = saida.
        // Idempotente: destroi o contexto anterior. Retorna false quando compilado como stub.
        bool Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) override;
        void Shutdown() override;
        bool IsInitialized() const override;
        bool Available() const override { return IsInitialized(); }

        // Offset de jitter do upscaler (em pixels, faixa ~[-0.5,0.5]) para alimentar a matriz de
        // projecao E o dispatch. Substitui o Halton da engine. FrameIndex monotonico crescente.
        void GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const override;

        // Roda o upscaler: le Color/Depth/Velocity (que o CHAMADOR deve ter posto em
        // NON_PIXEL_SHADER_RESOURCE = COMPUTE_READ) e escreve na textura de output, que e
        // gerenciada aqui (UAV durante o dispatch, PIXEL_SHADER_RESOURCE no fim p/ o post chain).
        void Dispatch(ID3D12GraphicsCommandList* Cmd, const FUpscaleParams& P) override;

        ID3D12Resource* OutputResource() const override; // cor reconstruida (display-res, em PIXEL_SHADER_RESOURCE)
        u32             OutputSRVSlot() const override;   // slot no heap da engine p/ o post chain ler

        // Razoes fixas do FSR: 1.0 / 1.5x / 1.7x / 2.0x / 3.0x (Native..UltraPerf).
        f32 RenderRatioForQuality(int Quality) const override;

        u32 RenderW()  const override;
        u32 RenderH()  const override;
        u32 DisplayW() const override;
        u32 DisplayH() const override;

    private:
        struct Impl;
        std::unique_ptr<Impl> P;
    };
}
