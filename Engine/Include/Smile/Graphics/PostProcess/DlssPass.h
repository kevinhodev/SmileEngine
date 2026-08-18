#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RHI/TextureSRVHeap.h"
#include "Smile/Graphics/PostProcess/Upscaler.h"
#include <memory>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace Smile {
    // Wrapper do NVIDIA DLSS Super Resolution via NVIDIA Streamline (sl.*), em modo MANUAL HOOKING:
    // o SL nao intercepta a criacao de device/swapchain — o Renderer chama slInit(eUseManualHooking) +
    // slSetD3DDevice, e este pass avalia o DLSS inline no command list (slEvaluateFeature). Esconde os
    // headers do SL atras de um pimpl. So fica disponivel em NVIDIA com suporte a feature; em qualquer
    // outro caso (ou SDK ausente no CMake, SMILE_SL_ENABLED=0) compila como stub e Available()=false,
    // entao o Renderer cai no FSR/None. Implementa IUpscaler (ver Upscaler.h).
    //
    // IMPORTANTE: slInit/slSetD3DDevice/slShutdown sao responsabilidade do Renderer (orquestracao do SL
    // num lugar so); este pass so faz a checagem de suporte, o output e o dispatch por frame.
    class FDlssPass final : public IUpscaler {
    public:
        FDlssPass();
        ~FDlssPass() override;
        FDlssPass(const FDlssPass&)            = delete;
        FDlssPass& operator=(const FDlssPass&) = delete;

        // Orquestracao global do Streamline (mantem o SL escondido do Renderer). InitStreamline deve ser
        // chamado ANTES da criacao do device (manual hooking), SetDevice logo apos, ShutdownStreamline no
        // fim. Em build stub (SMILE_SL_ENABLED=0) sao no-ops. Retorno de InitStreamline: false se falhou.
        static bool InitStreamline();
        static void SetDevice(ID3D12Device* Device);
        static void ShutdownStreamline();

        // Checa suporte (NVIDIA + slIsFeatureSupported) e cria a textura de output (display-res, UAV+SRV).
        // Idempotente. Retorna false se nao suportado / stub — nesse caso Available()=false.
        bool Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) override;
        void Shutdown() override;
        bool IsInitialized() const override;
        bool Available() const override;

        // DLSS nao gera jitter proprio: sequencia de Halton (o mesmo padrao que a engine ja usa).
        void GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const override;

        // Avalia o DLSS no command list: slGetNewFrameToken + slSetConstants + slSetTagForFrame +
        // slDLSSSetOptions + slEvaluateFeature(kFeatureDLSS), e RESTAURA o estado do CL no fim
        // (eDisableCLStateTracking e default). Output em PIXEL_SHADER_RESOURCE no fim.
        void Dispatch(ID3D12GraphicsCommandList* Cmd, const FUpscaleParams& P) override;

        ID3D12Resource* OutputResource() const override;
        u32             OutputSRVSlot() const override;

        // Razoes nominais padrao do DLSS (iguais as do FSR): 1.0 / 1.5 / 1.7 / 2.0 / 3.0.
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
