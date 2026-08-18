#include "Smile/Graphics/PostProcess/DlssPass.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/Barriers.h"
#include "Smile/Core/Logger.h"

#if SMILE_SL_ENABLED
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>   // kFeatureDLSS_RR — o FDlssRRPass avalia a feature; carregada aqui no init comum
#endif

namespace Smile {

#if SMILE_SL_ENABLED

    namespace {
        constexpr u32         kInvalidSlot = 0xFFFFFFFFu;
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;


        // Mat44 (row-major, row-vector) -> sl::float4x4 (row-major). Copia direta; se a reprojecao
        // sair errada e a convencao do SL for column-vector, transpor aqui (ver Risks no plano).
        sl::float4x4 ToSL(const Mat44& M) {
            sl::float4x4 R;
            for (int i = 0; i < 4; ++i)
                R.row[i] = sl::float4(M.M[i][0], M.M[i][1], M.M[i][2], M.M[i][3]);
            return R;
        }
        sl::float3 ToSL(const Vec3& V) { return sl::float3(V.X, V.Y, V.Z); }

        // Nome legivel dos sl::Result que mais aparecem no gate do DLSS. O SDK nao expoe
        // conversor p/ string, entao so os relevantes ficam mapeados (o resto sai como codigo).
        const char* SlResultName(sl::Result R) {
            switch (R) {
                case sl::Result::eOk:                         return "eOk";
                case sl::Result::eErrorDriverOutOfDate:       return "eErrorDriverOutOfDate";
                case sl::Result::eErrorOSDisabledHWS:         return "eErrorOSDisabledHWS";
                case sl::Result::eErrorDeviceNotCreated:      return "eErrorDeviceNotCreated";
                case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
                case sl::Result::eErrorAdapterNotSupported:   return "eErrorAdapterNotSupported";
                case sl::Result::eErrorNoPlugins:             return "eErrorNoPlugins";
                case sl::Result::eErrorNotInitialized:        return "eErrorNotInitialized";
                case sl::Result::eErrorInitNotCalled:         return "eErrorInitNotCalled";
                case sl::Result::eErrorMissingOrInvalidAPI:   return "eErrorMissingOrInvalidAPI";
                case sl::Result::eErrorFeatureMissing:        return "eErrorFeatureMissing";
                case sl::Result::eErrorFeatureNotSupported:   return "eErrorFeatureNotSupported";
                case sl::Result::eErrorFeatureFailedToLoad:   return "eErrorFeatureFailedToLoad";
                case sl::Result::eErrorFeatureMissingHooks:   return "eErrorFeatureMissingHooks";
                case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
                case sl::Result::eErrorInvalidIntegration:    return "eErrorInvalidIntegration";
                case sl::Result::eErrorInvalidState:          return "eErrorInvalidState";
                default:                                      return "(outro)";
            }
        }

        bool IsExpectedUnsupportedResult(sl::Result R) {
            return R == sl::Result::eErrorNoSupportedAdapterFound ||
                   R == sl::Result::eErrorAdapterNotSupported ||
                   R == sl::Result::eErrorFeatureNotSupported;
        }

        // Mapa qualidade (0=Native..4=UltraPerf) -> DLSSMode.
        sl::DLSSMode ModeForQuality(int Q) {
            switch (Q) {
                case 1:  return sl::DLSSMode::eMaxQuality;       // 1.5x
                case 2:  return sl::DLSSMode::eBalanced;         // 1.7x
                case 3:  return sl::DLSSMode::eMaxPerformance;   // 2.0x
                case 4:  return sl::DLSSMode::eUltraPerformance; // 3.0x
                default: return sl::DLSSMode::eDLAA;             // 0 = nativo (DLSS AA, 1.0x)
            }
        }
    }

    struct FDlssPass::Impl {
        sl::ViewportHandle Viewport{ 0 };
        bool Supported = false;               // NVIDIA + slIsFeatureSupported ok
        bool Created   = false;               // output criado
        u32 RW = 0, RH = 0, SW = 0, SH = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        u32  OutputSRV = kInvalidSlot;
        bool FirstDispatch = true;

        // Halton radical-inverse (base b), como a engine ja faz p/ o jitter do TAA.
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

    FDlssPass::FDlssPass() : P(std::make_unique<Impl>()) {}
    FDlssPass::~FDlssPass() { Shutdown(); }

    bool FDlssPass::InitStreamline() {
        sl::Preferences Pref{};
        // Mantem os defaults (eDisableCLStateTracking|eAllowOTA|eLoadDownloadedPlugins) e adiciona
        // manual hooking + tagging por frame.
        Pref.flags |= sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        // Carrega SR (kFeatureDLSS) e RR (kFeatureDLSS_RR). O RR e um denoiser neural que substitui o NRD
        // E o passe de SR (ver FDlssRRPass); os plugins production sao sl.dlss(_d).dll + nvngx_dlss(d).dll.
        static const sl::Feature kFeats[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
        Pref.featuresToLoad    = kFeats;
        Pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(kFeats));
        Pref.engine            = sl::EngineType::eCustom;
        Pref.engineVersion     = "SmileEngine";
        // OBRIGATORIO p/ o NGX: o SL so dispensa o applicationId (que a NVIDIA emite p/ titulos
        // publicados) quando engineVersion E projectId estao preenchidos — ai ele usa
        // NVSDK_NGX_D3D12_Init_with_ProjectID. Sem projectId cai no appId temporario e o proprio
        // SL desliga o NGX ("Please provide correct application id"), derrubando o DLSS inteiro
        // (slIsFeatureSupported passa a devolver eErrorFeatureNotSupported). GUID estavel do projeto:
        Pref.projectId         = "53806d86-abf0-4057-a4ea-904ed2161af2";
        Pref.renderAPI         = sl::RenderAPI::eD3D12;
    #ifdef _DEBUG
        // Em Debug pede o log verboso do proprio SL (sl.log ao lado do exe): e ele que revela
        // falha de carregamento/assinatura dos plugins (sl.dlss.dll / nvngx_dlss.dll).
        static const std::wstring kSlLogDir = L".";
        Pref.logLevel          = sl::LogLevel::eVerbose;
        Pref.pathToLogsAndData = kSlLogDir.c_str();
    #else
        Pref.logLevel          = sl::LogLevel::eDefault;
        Pref.pathToLogsAndData = nullptr;
    #endif
        sl::Result R = slInit(Pref);
        if (R != sl::Result::eOk) {
            LogWarning("Streamline: slInit falhou (codigo " + std::to_string(static_cast<int>(R)) +
                       ") — DLSS indisponivel");
            return false;
        }
        LogDebug("Streamline inicializado (manual hooking, feature DLSS)");
        return true;
    }
    void FDlssPass::SetDevice(ID3D12Device* Device) { if (Device) slSetD3DDevice(Device); }
    void FDlssPass::ShutdownStreamline() { slShutdown(); }

    bool FDlssPass::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) {
        if (!Device || RenderW == 0 || RenderH == 0 || DisplayW == 0 || DisplayH == 0) return false;
        P->Destroy();

        // Suporte: SL ja foi inicializado pelo Renderer (slInit + slSetD3DDevice). Checa a feature p/
        // este adapter (retorna eOk so em NVIDIA RTX com driver/feature ok — serve de gate de vendor).
        LUID Luid = Device->GetAdapterLuid();
        sl::AdapterInfo Adapter{};
        Adapter.deviceLUID            = reinterpret_cast<uint8_t*>(&Luid);
        Adapter.deviceLUIDSizeInBytes = sizeof(LUID);
        const sl::Result SupportRc = slIsFeatureSupported(sl::kFeatureDLSS, Adapter);
        P->Supported = (SupportRc == sl::Result::eOk);
        if (!P->Supported) {
            // Loga o codigo real: distingue "GPU/driver nao suporta" (eErrorAdapterNotSupported,
            // eErrorDriverOutOfDate) de falha de integracao (eErrorFeatureFailedToLoad = plugin
            // sl.dlss.dll/nvngx_dlss.dll nao carregou; eErrorInitNotCalled = ordem do slInit).
            const std::string Message = std::string("DLSS: slIsFeatureSupported falhou -> ") +
                SlResultName(SupportRc) + " (codigo " +
                std::to_string(static_cast<int>(SupportRc)) + ") — upscaler DLSS indisponivel";
            if (IsExpectedUnsupportedResult(SupportRc))
                LogDebug(Message);
            else
                LogWarning(Message);
            return false;
        }

        // Textura de output (display-res, UAV+SRV no heap da engine) — igual ao FsrPass.
        P->OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const HRESULT Hr = GpuResources::TryCreateTex2D(
            Device, P->Output, DisplayW, DisplayH, kOutputFormat,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, P->OutputState,
            EVramCategory::RenderTargets, nullptr, 1, 1, "Output do DLSS");
        if (FAILED(Hr)) {
            LogError("DLSS: criacao da textura de output falhou");
            return false;
        }

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
        LogDebug("DLSS (Streamline) inicializado (render " + std::to_string(RenderW) + "x" +
                std::to_string(RenderH) + " -> display " + std::to_string(DisplayW) + "x" +
                std::to_string(DisplayH) + ")");
        return true;
    }

    void FDlssPass::Shutdown() {
        if (!P) return;
        // Libera os recursos internos do DLSS p/ este viewport (o slInit/slShutdown fica no Renderer).
        if (P->Created) slFreeResources(sl::kFeatureDLSS, P->Viewport);
        P->Destroy();
    }

    bool FDlssPass::IsInitialized() const { return P && P->Created; }
    bool FDlssPass::Available()     const { return P && P->Supported; }

    void FDlssPass::GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const {
        OutX = OutY = 0.0f;
        if (!P || !P->Created || P->RW == 0) return;
        // Phase count recomendado p/ DLSS: 8 * (display/render)^2, minimo 1.
        const f32 Ratio = static_cast<f32>(P->SW) / static_cast<f32>(P->RW);
        u32 Phase = static_cast<u32>(8.0f * Ratio * Ratio + 0.5f);
        if (Phase == 0) Phase = 1;
        const u32 Idx = (FrameIndex % Phase) + 1;   // Halton e 1-based
        OutX = Impl::Halton(Idx, 2) - 0.5f;
        OutY = Impl::Halton(Idx, 3) - 0.5f;
    }

    void FDlssPass::Dispatch(ID3D12GraphicsCommandList* Cmd, const FUpscaleParams& _UpscaleParams) {
        if (!P || !P->Created || !Cmd || !_UpscaleParams.Color || !_UpscaleParams.Depth || !_UpscaleParams.Velocity) return;

        TransitionResource(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        P->OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        auto* CmdBuf = reinterpret_cast<sl::CommandBuffer*>(Cmd);

        sl::FrameToken* Frame = nullptr;
        const uint32_t  FrameIdx = _UpscaleParams.Reset ? 0u : 0u; // token por frame (indice opcional)
        if (slGetNewFrameToken(Frame, nullptr) != sl::Result::eOk || !Frame) {
            LogError("DLSS: slGetNewFrameToken falhou");
            TransitionResource(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            P->OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return;
        }
        (void)FrameIdx;

        // --- Constants (matrizes row-major SEM jitter; o Renderer preencheu ViewToClip/ClipToPrevClip) ---
        sl::Constants C{};
        C.cameraViewToClip = ToSL(_UpscaleParams.ViewToClip);
        C.clipToCameraView = ToSL(_UpscaleParams.ViewToClip.Inverse());
        C.clipToPrevClip   = ToSL(_UpscaleParams.ClipToPrevClip);
        C.prevClipToClip   = ToSL(_UpscaleParams.ClipToPrevClip.Inverse());
        C.jitterOffset        = { _UpscaleParams.JitterX, _UpscaleParams.JitterY };
        C.cameraPinholeOffset = { 0.0f, 0.0f };   // pinhole convencional (senao fica INVALID_FLOAT)
        // Velocity da engine = curUV-prevUV; o plugin faz NGX MV_Scale = mvecScale*W/H (dlss*Entry.cpp), ou
        // seja o NGX ja converte p/ pixels => mvecScale=-1 casa (prevUV-curUV)*W. Confirmado na fonte do SL.
        C.mvecScale        = { -1.0f, -1.0f };
        C.cameraPos        = ToSL(_UpscaleParams.CamPos);
        C.cameraUp         = ToSL(_UpscaleParams.CamUp);
        C.cameraRight      = ToSL(_UpscaleParams.CamRight);
        C.cameraFwd        = ToSL(_UpscaleParams.CamFwd);
        C.cameraNear       = _UpscaleParams.NearZ;
        C.cameraFar        = _UpscaleParams.FarZ;
        C.cameraFOV        = _UpscaleParams.FovYRadians;
        C.cameraAspectRatio = _UpscaleParams.AspectRatio;
        C.depthInverted        = sl::Boolean::eTrue;   // engine usa reverse-Z
        C.cameraMotionIncluded = sl::Boolean::eTrue;
        C.motionVectors3D      = sl::Boolean::eFalse;
        C.reset            = (_UpscaleParams.Reset || P->FirstDispatch) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        slSetConstants(C, *Frame, P->Viewport);

        // --- Options (modo por qualidade; HDR; auto-exposure como no FSR) ---
        sl::DLSSOptions DLSSOptions{};
        DLSSOptions.mode            = ModeForQuality(_UpscaleParams.Quality);
        DLSSOptions.outputWidth     = P->SW;
        DLSSOptions.outputHeight    = P->SH;
        DLSSOptions.colorBuffersHDR = sl::Boolean::eTrue;
        DLSSOptions.useAutoExposure = sl::Boolean::eTrue;
        slDLSSSetOptions(P->Viewport, DLSSOptions);

        // --- Tags (estado ATUAL de cada recurso; o chamador poe cor/depth/vel em COMPUTE_READ) ---
        sl::Resource ColorRes (sl::ResourceType::eTex2d, _UpscaleParams.Color,       static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sl::Resource DepthRes (sl::ResourceType::eTex2d, _UpscaleParams.Depth,       static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sl::Resource MvecRes  (sl::ResourceType::eTex2d, _UpscaleParams.Velocity,    static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sl::Resource OutputRes(sl::ResourceType::eTex2d, P->Output.Get(),            static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

        const sl::Extent RenderExtent{ 0, 0, P->RW, P->RH };
        const sl::Extent OutputExtent{ 0, 0, P->SW, P->SH };
        sl::ResourceTag Tags[] = {
            sl::ResourceTag(&ColorRes,  sl::kBufferTypeScalingInputColor,  sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&DepthRes,  sl::kBufferTypeDepth,              sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&MvecRes,   sl::kBufferTypeMotionVectors,      sl::ResourceLifecycle::eValidUntilEvaluate, &RenderExtent),
            sl::ResourceTag(&OutputRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &OutputExtent),
        };
        slSetTagForFrame(*Frame, P->Viewport, Tags, static_cast<uint32_t>(std::size(Tags)), CmdBuf);

        // --- Evaluate ---
        const sl::BaseStructure* Inputs[] = { &P->Viewport };
        sl::Result Rc = slEvaluateFeature(sl::kFeatureDLSS, *Frame, Inputs,
                                          static_cast<uint32_t>(std::size(Inputs)), CmdBuf);
        if (Rc != sl::Result::eOk)
            LogError("DLSS: slEvaluateFeature falhou (codigo " + std::to_string(static_cast<int>(Rc)) + ")");

        // eDisableCLStateTracking e default: o SL pode ter mexido no estado do CL. O Renderer rebinda os
        // descriptor heaps antes do post chain (o post chain seta o proprio root sig/PSO).

        TransitionResource(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        P->OutputState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        P->FirstDispatch = false;
    }

    ID3D12Resource* FDlssPass::OutputResource() const { return P ? P->Output.Get() : nullptr; }
    u32             FDlssPass::OutputSRVSlot() const { return P ? P->OutputSRV : kInvalidSlot; }
    f32 FDlssPass::RenderRatioForQuality(int Quality) const {
        // Razoes nominais padrao do DLSS (iguais as do FSR). Se quiser a res otima exata / dynamic-res,
        // usar slDLSSGetOptimalSettings.
        static const f32 R[] = { 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 1.0f / 2.0f, 1.0f / 3.0f };
        return R[Quality < 0 ? 0 : (Quality > 4 ? 4 : Quality)];
    }
    u32 FDlssPass::RenderW()  const { return P ? P->RW : 0; }
    u32 FDlssPass::RenderH()  const { return P ? P->RH : 0; }
    u32 FDlssPass::DisplayW() const { return P ? P->SW : 0; }
    u32 FDlssPass::DisplayH() const { return P ? P->SH : 0; }

#else
    struct FDlssPass::Impl {};
    FDlssPass::FDlssPass()  = default;
    FDlssPass::~FDlssPass() = default;
    bool FDlssPass::InitStreamline() { return false; }
    void FDlssPass::SetDevice(ID3D12Device*) {}
    void FDlssPass::ShutdownStreamline() {}
    bool FDlssPass::Initialize(ID3D12Device*, FTextureSRVHeap&, u32, u32, u32, u32) { return false; }
    void FDlssPass::Shutdown() {}
    bool FDlssPass::IsInitialized() const { return false; }
    bool FDlssPass::Available()     const { return false; }
    void FDlssPass::GetJitter(u32, f32& OutX, f32& OutY) const { OutX = OutY = 0.0f; }
    void FDlssPass::Dispatch(ID3D12GraphicsCommandList*, const FUpscaleParams&) {}
    ID3D12Resource* FDlssPass::OutputResource() const { return nullptr; }
    u32             FDlssPass::OutputSRVSlot() const { return 0xFFFFFFFFu; }
    f32 FDlssPass::RenderRatioForQuality(int) const { return 1.0f; }
    u32 FDlssPass::RenderW()  const { return 0; }
    u32 FDlssPass::RenderH()  const { return 0; }
    u32 FDlssPass::DisplayW() const { return 0; }
    u32 FDlssPass::DisplayH() const { return 0; }

#endif
}
