#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Upscaler.h"
#include <memory>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace Smile {
    // Wrapper do NVIDIA DLSS Ray Reconstruction (RR, sl::kFeatureDLSS_RR) via NVIDIA Streamline, em modo
    // MANUAL HOOKING (mesma orquestracao do FDlssPass — ver DlssPass.h). O RR e um denoiser NEURAL que
    // SUBSTITUI o NRD (denoise do ReSTIR GI difuso + reflexao especular) E o passe de Super Resolution: um
    // unico slEvaluateFeature(kFeatureDLSS_RR) faz denoise + upscale, guiado por buffers de material
    // (albedo difuso/especular, normal-roughness, hitDist especular). Ganho e QUALIDADE (mata o borrao do
    // NRD), nao fps.
    //
    // Implementa IUpscaler p/ reusar todo o plumbing "output display-res -> post chain" que o FSR/DLSS-SR ja
    // usam. A diferenca fica no Renderer: quando o RR e o denoiser ativo, o GI/reflexao entram RUIDOSOS na
    // cor e o NRD e pulado (ver Renderer EDenoiser::DLSS_RR). Os guides chegam pelos campos extras de
    // FUpscaleParams (DiffuseAlbedo/SpecularAlbedo/NormalRoughness/SpecHitDist + WorldToView).
    //
    // slInit (carregando kFeatureDLSS_RR) / slSetD3DDevice / slShutdown sao do Renderer via o FDlssPass
    // (InitStreamline/SetDevice/ShutdownStreamline) — um lugar so p/ o SL. So fica disponivel em NVIDIA RTX
    // com suporte a feature; sem SDK (SMILE_SL_ENABLED=0) compila como stub e Available()=false.
    class FDlssRRPass final : public IUpscaler {
    public:
        FDlssRRPass();
        ~FDlssRRPass() override;
        FDlssRRPass(const FDlssRRPass&)            = delete;
        FDlssRRPass& operator=(const FDlssRRPass&) = delete;

        // Checa suporte (NVIDIA + slIsFeatureSupported(kFeatureDLSS_RR)) e cria a textura de output
        // (display-res, UAV+SRV). Idempotente. Retorna false se nao suportado / stub — Available()=false.
        bool Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) override;
        void Shutdown() override;
        bool IsInitialized() const override;
        bool Available() const override;

        // RR nao gera jitter proprio: sequencia de Halton (o mesmo padrao do DLSS-SR / da engine).
        void GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const override;

        // Avalia o RR no command list: slGetNewFrameToken + slSetConstants + slDLSSDSetOptions +
        // slSetTagForFrame (cor ruidosa + depth + mvec + guides) + slEvaluateFeature(kFeatureDLSS_RR).
        // O SL pode mexer no estado do CL (eDisableCLStateTracking) — o Renderer rebinda os heaps depois.
        // Le os guides de FUpscaleParams (P.DiffuseAlbedo/SpecularAlbedo/NormalRoughness/SpecHitDist).
        void Dispatch(ID3D12GraphicsCommandList* Cmd, const FUpscaleParams& P) override;

        ID3D12Resource* OutputResource() const override;
        u32             OutputSRVSlot() const override;

        // Razoes nominais padrao do DLSS (iguais as do SR): 1.0 / 1.5 / 1.7 / 2.0 / 3.0. RR nao suporta DRS.
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
