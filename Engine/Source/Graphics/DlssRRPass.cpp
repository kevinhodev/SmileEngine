#include "Smile/Graphics/DlssRRPass.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/Logger.h"

#if SMILE_SL_ENABLED
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#endif

namespace Smile {

#if SMILE_SL_ENABLED

    namespace {
        constexpr u32         kInvalidSlot = 0xFFFFFFFFu;
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        void Transition(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After) {
            if (Before == After) return;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = Res;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = Before;
            B.Transition.StateAfter  = After;
            Cmd->ResourceBarrier(1, &B);
        }

        // Mat44 (row-major, row-vector) -> sl::float4x4 (row-major). Mesma convencao do FDlssPass; se a
        // reprojecao sair errada e o SL for column-vector, transpor aqui (ver Risks do plano).
        sl::float4x4 ToSL(const Mat44& M) {
            sl::float4x4 R;
            for (int i = 0; i < 4; ++i)
                R.row[i] = sl::float4(M.M[i][0], M.M[i][1], M.M[i][2], M.M[i][3]);
            return R;
        }
        sl::float3 ToSL(const Vec3& V) { return sl::float3(V.X, V.Y, V.Z); }

        const char* SlResultName(sl::Result R) {
            switch (R) {
                case sl::Result::eOk:                           return "eOk";
                case sl::Result::eErrorDriverOutOfDate:         return "eErrorDriverOutOfDate";
                case sl::Result::eErrorOSDisabledHWS:           return "eErrorOSDisabledHWS";
                case sl::Result::eErrorDeviceNotCreated:        return "eErrorDeviceNotCreated";
                case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
                case sl::Result::eErrorAdapterNotSupported:     return "eErrorAdapterNotSupported";
                case sl::Result::eErrorNoPlugins:               return "eErrorNoPlugins";
                case sl::Result::eErrorNotInitialized:          return "eErrorNotInitialized";
                case sl::Result::eErrorInitNotCalled:           return "eErrorInitNotCalled";
                case sl::Result::eErrorMissingOrInvalidAPI:     return "eErrorMissingOrInvalidAPI";
                case sl::Result::eErrorFeatureMissing:          return "eErrorFeatureMissing";
                case sl::Result::eErrorFeatureNotSupported:     return "eErrorFeatureNotSupported";
                case sl::Result::eErrorFeatureFailedToLoad:     return "eErrorFeatureFailedToLoad";
                case sl::Result::eErrorFeatureMissingHooks:     return "eErrorFeatureMissingHooks";
                case sl::Result::eErrorFeatureMissingDependency:return "eErrorFeatureMissingDependency";
                case sl::Result::eErrorInvalidIntegration:      return "eErrorInvalidIntegration";
                case sl::Result::eErrorInvalidState:            return "eErrorInvalidState";
                default:                                        return "(outro)";
            }
        }

        bool IsExpectedUnsupportedResult(sl::Result R) {
            return R == sl::Result::eErrorNoSupportedAdapterFound ||
                   R == sl::Result::eErrorAdapterNotSupported ||
                   R == sl::Result::eErrorFeatureNotSupported;
        }

        // Mapa qualidade (0=Native..4=UltraPerf) -> DLSSMode. RR roda no MESMO perf mode do SR.
        sl::DLSSMode ModeForQuality(int Q) {
            switch (Q) {
                case 1:  return sl::DLSSMode::eMaxQuality;       // 1.5x
                case 2:  return sl::DLSSMode::eBalanced;         // 1.7x
                case 3:  return sl::DLSSMode::eMaxPerformance;   // 2.0x
                case 4:  return sl::DLSSMode::eUltraPerformance; // 3.0x
                default: return sl::DLSSMode::eDLAA;             // 0 = nativo (1.0x)
            }
        }
    }

    struct FDlssRRPass::Impl {
        sl::ViewportHandle Viewport{ 0 };
        bool Supported = false;               // NVIDIA + slIsFeatureSupported(kFeatureDLSS_RR) ok
        bool Created   = false;               // output criado
        u32 RW = 0, RH = 0, SW = 0, SH = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        u32  OutputSRV = kInvalidSlot;
        bool FirstDispatch = true;

        static f32 Halton(u32 i, u32 b) {
            f32 f = 1.0f, r = 0.0f;
            while (i > 0) { f /= static_cast<f32>(b); r += f * static_cast<f32>(i % b); i /= b; }
            return r;
        }

        void Destroy() {
            Created = false;
            Output.Reset();
        }
    };

    FDlssRRPass::FDlssRRPass() : P(std::make_unique<Impl>()) {}
    FDlssRRPass::~FDlssRRPass() { Shutdown(); }

    bool FDlssRRPass::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                 u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) {
        if (!Device || RenderW == 0 || RenderH == 0 || DisplayW == 0 || DisplayH == 0) return false;

        // Re-init (resize da janela / troca de render scale): o plugin do SL SO recria a feature NGX
        // quando mode, outputWidth/Height, normalRoughnessMode ou presets mudam (dlss_dEntry.cpp:347) —
        // a resolucao de ENTRADA nao entra nessa conta. Sem liberar aqui, mudar apenas a render res
        // (ex.: o slider de render scale do editor, que nao passa por ApplyUpscalerScale) deixa a
        // feature dimensionada pra entrada velha, e o subrect que tagamos deixa de casar com ela.
        // O caller (Renderer::Resize / SetRenderScale) ja fez CommandQueue.Flush().
        if (P->Created) slFreeResources(sl::kFeatureDLSS_RR, P->Viewport);
        P->Destroy();

        // Suporte: SL ja inicializado pelo Renderer (slInit carregou kFeatureDLSS + kFeatureDLSS_RR).
        LUID Luid = Device->GetAdapterLuid();
        sl::AdapterInfo Adapter{};
        Adapter.deviceLUID            = reinterpret_cast<uint8_t*>(&Luid);
        Adapter.deviceLUIDSizeInBytes = sizeof(LUID);
        const sl::Result SupportRc = slIsFeatureSupported(sl::kFeatureDLSS_RR, Adapter);
        P->Supported = (SupportRc == sl::Result::eOk);
        if (!P->Supported) {
            const std::string Message = std::string("DLSS-RR: slIsFeatureSupported falhou -> ") +
                SlResultName(SupportRc) + " (codigo " +
                std::to_string(static_cast<int>(SupportRc)) +
                ") — denoiser Ray Reconstruction indisponivel";
            if (IsExpectedUnsupportedResult(SupportRc))
                LogDebug(Message);
            else
                LogWarning(Message);
            return false;
        }

        // Textura de output (display-res, UAV+SRV no heap da engine) — igual ao FsrPass/DlssPass.
        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC TexDesc{};
        TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TexDesc.Width            = DisplayW;
        TexDesc.Height           = DisplayH;
        TexDesc.DepthOrArraySize = 1;
        TexDesc.MipLevels        = 1;
        TexDesc.Format           = kOutputFormat;
        TexDesc.SampleDesc       = { 1, 0 };
        TexDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        TexDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        P->OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        HRESULT Hr = Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &TexDesc, P->OutputState, nullptr, IID_PPV_ARGS(&P->Output));
        if (FAILED(Hr)) {
            LogError("DLSS-RR: CreateCommittedResource (output) falhou");
            return false;
        }
        VramTracker::Register(P->Output.Get(), EVramCategory::RenderTargets);

        if (P->OutputSRV == kInvalidSlot) P->OutputSRV = SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = kOutputFormat;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device, P->Output.Get(), SRVDesc, P->OutputSRV);

        P->Created = true;
        P->FirstDispatch = true;
        P->RW = RenderW; P->RH = RenderH; P->SW = DisplayW; P->SH = DisplayH;
        LogDebug("DLSS-RR (Streamline) inicializado (render " + std::to_string(RenderW) + "x" +
                std::to_string(RenderH) + " -> display " + std::to_string(DisplayW) + "x" +
                std::to_string(DisplayH) + ")");
        return true;
    }

    void FDlssRRPass::Shutdown() {
        if (!P) return;
        if (P->Created) slFreeResources(sl::kFeatureDLSS_RR, P->Viewport);
        P->Destroy();
    }

    bool FDlssRRPass::IsInitialized() const { return P && P->Created; }
    bool FDlssRRPass::Available()     const { return P && P->Supported; }

    void FDlssRRPass::GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const {
        OutX = OutY = 0.0f;
        if (!P || !P->Created || P->RW == 0) return;
        const f32 Ratio = static_cast<f32>(P->SW) / static_cast<f32>(P->RW);
        u32 Phase = static_cast<u32>(8.0f * Ratio * Ratio + 0.5f);
        // DLSS-RR Integration Guide §3.6 recomenda pelo menos 32 posicoes, inclusive em DLAA.
        // Mantem a formula do SR caso uma razao futura produza uma sequencia ainda maior.
        if (Phase < 32u) Phase = 32u;
        const u32 Idx = (FrameIndex % Phase) + 1;   // Halton e 1-based
        OutX = Impl::Halton(Idx, 2) - 0.5f;
        OutY = Impl::Halton(Idx, 3) - 0.5f;
    }

    void FDlssRRPass::Dispatch(ID3D12GraphicsCommandList* Cmd, const FUpscaleParams& In) {
        // RR exige, alem de cor/depth/vel, os 4 guides. Sem qualquer um, aborta sem tocar o output.
        if (!P || !P->Created || !Cmd || !In.Color || !In.Depth || !In.Velocity ||
            !In.DiffuseAlbedo || !In.SpecularAlbedo || !In.NormalRoughness || !In.SpecHitDist) {
            LogError("DLSS-RR: dispatch sem cor/depth/vel/guides — pulado");
            return;
        }

        Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        P->OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        auto* CmdBuf = reinterpret_cast<sl::CommandBuffer*>(Cmd);

        sl::FrameToken* Frame = nullptr;
        if (slGetNewFrameToken(Frame, nullptr) != sl::Result::eOk || !Frame) {
            LogError("DLSS-RR: slGetNewFrameToken falhou");
            Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            P->OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return;
        }

        // --- Common Constants (matrizes row-major SEM jitter; iguais ao DLSS-SR) ---
        sl::Constants C{};
        C.cameraViewToClip = ToSL(In.ViewToClip);
        C.clipToCameraView = ToSL(In.ViewToClip.Inverse());
        C.clipToPrevClip   = ToSL(In.ClipToPrevClip);
        C.prevClipToClip   = ToSL(In.ClipToPrevClip.Inverse());
        C.jitterOffset        = { In.JitterX, In.JitterY };
        C.cameraPinholeOffset = { 0.0f, 0.0f };   // pinhole convencional; sem isto fica INVALID_FLOAT (o
                                                  // validador do SL reclama "should not be left as invalid")
        // Velocity da engine = curUV-prevUV (espaco UV). O plugin do SL faz NGX MV_Scale = mvecScale*W/H
        // (dlss_dEntry.cpp:858) — ou seja, o NGX ja converte p/ pixels. Logo o deslocamento vira
        // (curUV-prevUV)*mvecScale*W; p/ casar (prevUV-curUV)*W (convencao do NGX) => mvecScale = -1.
        // Mesma convencao do NRD nesse buffer. {-2,-2} DOBRA a reprojecao (overshoot). NAO conserta o
        // rastro do ceu (velocity la e ZERO; 2*0=0) — isso e MV de background ausente, nao escala.
        C.mvecScale        = { -1.0f, -1.0f };
        C.cameraPos        = ToSL(In.CamPos);
        C.cameraUp         = ToSL(In.CamUp);
        C.cameraRight      = ToSL(In.CamRight);
        C.cameraFwd        = ToSL(In.CamFwd);
        C.cameraNear       = In.NearZ;
        C.cameraFar        = In.FarZ;
        C.cameraFOV        = In.FovYRadians;
        C.cameraAspectRatio = In.AspectRatio;
        C.depthInverted        = sl::Boolean::eTrue;   // engine usa reverse-Z
        C.cameraMotionIncluded = sl::Boolean::eTrue;
        C.motionVectors3D      = sl::Boolean::eFalse;
        C.reset            = (In.Reset || P->FirstDispatch) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        slSetConstants(C, *Frame, P->Viewport);

        // --- RR Options (DLSSDOptions, nao DLSSOptions) ---
        sl::DLSSDOptions Opt{};
        Opt.mode                = ModeForQuality(In.Quality);
        Opt.outputWidth         = P->SW;
        Opt.outputHeight        = P->SH;
        Opt.colorBuffersHDR     = sl::Boolean::eTrue;       // RR so suporta HDR
        Opt.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked; // roughness linear no .a do NormalRoughness
        // specHitDist -> reflection MV interno; RR precisa das matrizes world<->view.
        Opt.worldToCameraView   = ToSL(In.WorldToView);
        Opt.cameraViewToWorld   = ToSL(In.WorldToView.Inverse());
        // Preset E = transformer mais recente no Streamline 2.12.
        Opt.dlaaPreset             = sl::DLSSDPreset::ePresetE;
        Opt.qualityPreset          = sl::DLSSDPreset::ePresetE;
        Opt.balancedPreset         = sl::DLSSDPreset::ePresetE;
        Opt.performancePreset      = sl::DLSSDPreset::ePresetE;
        Opt.ultraPerformancePreset = sl::DLSSDPreset::ePresetE;
        if (slDLSSDSetOptions(P->Viewport, Opt) != sl::Result::eOk)
            LogError("DLSS-RR: slDLSSDSetOptions falhou");

        // Sanidade da 1a frame (a feature NGX e criada neste evaluate): o plugin cria com a res OTIMA
        // do modo e usa a NOSSA extent como render subrect. Se a render res passar do otimo, o subrect
        // extrapola o buffer criado — acontece se a razao nao vier do SDK ou se a render scale for
        // dirigida a mao (o RR nao suporta DRS: a res de entrada e ditada pelo modo). Barato e uma vez.
        if (P->FirstDispatch) {
            sl::DLSSDOptimalSettings Chk{};
            if (slDLSSDGetOptimalSettings(Opt, Chk) == sl::Result::eOk && Chk.optimalRenderWidth > 0 &&
                (P->RW > Chk.optimalRenderWidth || P->RH > Chk.optimalRenderHeight)) {
                LogWarning("DLSS-RR: render " + std::to_string(P->RW) + "x" + std::to_string(P->RH) +
                           " PASSA da resolucao otima do modo (" + std::to_string(Chk.optimalRenderWidth) +
                           "x" + std::to_string(Chk.optimalRenderHeight) + ") — o render subrect extrapola"
                           " a feature NGX; confira 'Created DLSSDContext feature' vs 'color_in extents'"
                           " no log do Streamline");
            }
        }

        // --- Tags (estado ATUAL de cada recurso; o caller poe tudo em NON_PIXEL_SHADER_RESOURCE) ---
        constexpr auto kReadState = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource ColorRes  (sl::ResourceType::eTex2d, In.Color,          kReadState);
        sl::Resource DepthRes  (sl::ResourceType::eTex2d, In.Depth,          kReadState);
        sl::Resource MvecRes   (sl::ResourceType::eTex2d, In.Velocity,       kReadState);
        sl::Resource DiffAlbRes(sl::ResourceType::eTex2d, In.DiffuseAlbedo,  kReadState);
        sl::Resource SpecAlbRes(sl::ResourceType::eTex2d, In.SpecularAlbedo, kReadState);
        sl::Resource NrmRoughRes(sl::ResourceType::eTex2d, In.NormalRoughness, kReadState);
        sl::Resource SpecHitRes(sl::ResourceType::eTex2d, In.SpecHitDist,    kReadState);
        sl::Resource OutputRes (sl::ResourceType::eTex2d, P->Output.Get(),
                                static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

        const sl::Extent RenderExtent{ 0, 0, P->RW, P->RH };
        const sl::Extent OutputExtent{ 0, 0, P->SW, P->SH };
        sl::ResourceTag Tags[] = {
            sl::ResourceTag(&ColorRes,   sl::kBufferTypeScalingInputColor,     sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&DepthRes,   sl::kBufferTypeDepth,                 sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&MvecRes,    sl::kBufferTypeMotionVectors,         sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&DiffAlbRes, sl::kBufferTypeAlbedo,                sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&SpecAlbRes, sl::kBufferTypeSpecularAlbedo,        sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&NrmRoughRes,sl::kBufferTypeNormalRoughness,       sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&SpecHitRes, sl::kBufferTypeSpecularHitDistance,   sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&OutputRes,  sl::kBufferTypeScalingOutputColor,    sl::ResourceLifecycle::eValidUntilEvaluate, &OutputExtent),
        };
        slSetTagForFrame(*Frame, P->Viewport, Tags, static_cast<uint32_t>(std::size(Tags)), CmdBuf);

        // --- Evaluate ---
        const sl::BaseStructure* Inputs[] = { &P->Viewport };
        sl::Result Rc = slEvaluateFeature(sl::kFeatureDLSS_RR, *Frame, Inputs,
                                          static_cast<uint32_t>(std::size(Inputs)), CmdBuf);
        if (Rc != sl::Result::eOk)
            LogError("DLSS-RR: slEvaluateFeature falhou (codigo " + std::to_string(static_cast<int>(Rc)) + ")");

        Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        P->OutputState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        P->FirstDispatch = false;
    }

    ID3D12Resource* FDlssRRPass::OutputResource() const { return P ? P->Output.Get() : nullptr; }
    u32             FDlssRRPass::OutputSRVSlot() const { return P ? P->OutputSRV : kInvalidSlot; }
    f32 FDlssRRPass::RenderRatioForQuality(int Quality) const {
        // Razoes NOMINAIS do DLSS — fallback. Nao sao as razoes reais: o Balanced do NGX e 58,0%, e
        // 1/1.7 = 58,8%. Essa diferenca importa porque o plugin cria a feature NGX com a resolucao
        // OTIMA do driver (dlss_dEntry.cpp:471) e passa a extent que tagamos como
        // DLSS_Render_Subrect_Dimensions (dlss_dEntry.cpp:865) — com a razao nominal o subrect fica
        // MAIOR que o buffer criado (1129 vs 1114 num display de 1920). Por isso perguntamos a razao
        // ao SDK (§3.0 do ProgrammingGuideDLSS_RR.md) e so caimos no nominal sem SL/output conhecido.
        static const f32 kNominal[] = { 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 1.0f / 2.0f, 1.0f / 3.0f };
        const int Q = Quality < 0 ? 0 : (Quality > 4 ? 4 : Quality);
        if (!P || !P->Supported || P->SW == 0 || P->SH == 0) return kNominal[Q];

        sl::DLSSDOptions Opt{};
        Opt.mode         = ModeForQuality(Q);
        Opt.outputWidth  = P->SW;
        Opt.outputHeight = P->SH;
        sl::DLSSDOptimalSettings Settings{};
        if (slDLSSDGetOptimalSettings(Opt, Settings) != sl::Result::eOk ||
            Settings.optimalRenderWidth == 0 || Settings.optimalRenderHeight == 0)
            return kNominal[Q];

        // O Renderer tem UMA escala escalar p/ os dois eixos (RenderScale), entao pega a MENOR das
        // duas razoes: garante que nem a largura nem a altura derivadas passem do otimo — o
        // arredondamento do RenderWidth() (+0.5) no pior caso empata com o otimo, nunca o excede.
        const f32 RatioW = static_cast<f32>(Settings.optimalRenderWidth)  / static_cast<f32>(P->SW);
        const f32 RatioH = static_cast<f32>(Settings.optimalRenderHeight) / static_cast<f32>(P->SH);
        return RatioW < RatioH ? RatioW : RatioH;
    }
    u32 FDlssRRPass::RenderW()  const { return P ? P->RW : 0; }
    u32 FDlssRRPass::RenderH()  const { return P ? P->RH : 0; }
    u32 FDlssRRPass::DisplayW() const { return P ? P->SW : 0; }
    u32 FDlssRRPass::DisplayH() const { return P ? P->SH : 0; }

#else
    struct FDlssRRPass::Impl {};
    FDlssRRPass::FDlssRRPass()  = default;
    FDlssRRPass::~FDlssRRPass() = default;
    bool FDlssRRPass::Initialize(ID3D12Device*, FTextureSRVHeap&, u32, u32, u32, u32) { return false; }
    void FDlssRRPass::Shutdown() {}
    bool FDlssRRPass::IsInitialized() const { return false; }
    bool FDlssRRPass::Available()     const { return false; }
    void FDlssRRPass::GetJitter(u32, f32& OutX, f32& OutY) const { OutX = OutY = 0.0f; }
    void FDlssRRPass::Dispatch(ID3D12GraphicsCommandList*, const FUpscaleParams&) {}
    ID3D12Resource* FDlssRRPass::OutputResource() const { return nullptr; }
    u32             FDlssRRPass::OutputSRVSlot() const { return 0xFFFFFFFFu; }
    f32 FDlssRRPass::RenderRatioForQuality(int) const { return 1.0f; }
    u32 FDlssRRPass::RenderW()  const { return 0; }
    u32 FDlssRRPass::RenderH()  const { return 0; }
    u32 FDlssRRPass::DisplayW() const { return 0; }
    u32 FDlssRRPass::DisplayH() const { return 0; }

#endif
}
