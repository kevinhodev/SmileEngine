#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/RHI/GpuResources.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Graphics/Water/OceanSpectrum.h"
#include "Smile/Graphics/RayTracing/RTMasks.h" // kRTMaskShadowFull: mascara dos shadow rays de direta local
#include "Smile/Graphics/RHI/Barriers.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/Renderer/DepthConfig.h"
#include "Smile/Graphics/RayTracing/RayEpsilons.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <exception>
#include <functional>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <type_traits>

namespace Smile {
    namespace {
        // Só usada pelo relato de progresso do boot (nome do adaptador vem em wchar do DXGI).
        std::string ToUtf8(const std::wstring& _Wide) {
            if (_Wide.empty()) return {};
            const int Size = WideCharToMultiByte(CP_UTF8, 0, _Wide.data(),
                                                 static_cast<int>(_Wide.size()),
                                                 nullptr, 0, nullptr, nullptr);
            if (Size <= 0) return {};
            std::string Utf8(static_cast<size_t>(Size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, _Wide.data(), static_cast<int>(_Wide.size()),
                                Utf8.data(), Size, nullptr, nullptr);
            return Utf8;
        }
    }

    Renderer::Renderer() : SettingsImpl(std::make_unique<FRenderSettings>(*this)) {}

    FRenderSettings&       Renderer::Settings()       { return *SettingsImpl; }
    const FRenderSettings& Renderer::Settings() const { return *SettingsImpl; }

    Renderer::~Renderer() noexcept {
        try {
            Shutdown();
        } catch (const std::exception& Error) {
            LogError(std::string("Falha absorvida no shutdown do Renderer: ") + Error.what());
        } catch (...) {
            LogError("Falha desconhecida absorvida no shutdown do Renderer");
        }
    }

    void Renderer::ReportInitProgress(std::string_view _Label, std::string_view _Detail,
                                      f32 _Fraction) const {
        if (InitProgressCallback) InitProgressCallback(_Label, _Detail, _Fraction);
    }

    void Renderer::Initialize(HWND _hWnd, u32 _Width, u32 _Height) {
        if (Initialized) return;

        const auto InitStarted = std::chrono::steady_clock::now();

    #ifdef _DEBUG
        constexpr bool kDebugLayer = true;
    #else
        constexpr bool kDebugLayer = false;
    #endif

        ReportInitProgress("Criando dispositivo Direct3D 12", {}, 0.04f);

        // Streamline (DLSS) em manual hooking: inicializar ANTES de criar o device D3D12.
        FDlssPass::InitStreamline();
        Device.Initialize(kDebugLayer);
        FDlssPass::SetDevice(Device.Native());   // avisa o SL do device (manual hooking)
        // O adaptador so tem nome depois do Device.Initialize; vira o chip da splash.
        ReportInitProgress("Criando filas e swap chain",
                           ToUtf8(Device.GetAdapterDescription()), 0.12f);
        CommandQueue.Initialize(Device.Native(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        UploadQueue.Initialize(Device.Native());
        ComputeQueue.Initialize(Device.Native());
        GpuProfiler.Initialize(Device.Native(), CommandQueue.Native(),
                               FCommandQueue::kFramesInFlight);
        GpuProfilerCompute.Initialize(Device.Native(), ComputeQueue.Native(),
                                      FAsyncComputeQueue::kSlots);
        SwapChain.Initialize(Device.GetFactory(),
                             CommandQueue.Native(),
                             Device.Native(),
                             _hWnd, _Width, _Height,
                             Device.TearingSupported());
        SRVHeap.Initialize(Device.Native());
        ReportInitProgress("Compilando pipelines de raster", {}, 0.22f);
        PipelineState.Initialize(Device.Native());
        Targets.SceneColorMipPSO.Initialize(Device.Native(), "WaterSceneColorMip.cs_6_0.cso", false);

        ReportInitProgress("Alocando G-Buffer e alvos de cena", {}, 0.40f);
        Targets.CreateDepthBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateNormalBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        GBuffer.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        GBuffer.WriteDepthSRV(Device.Native(), SRVHeap, Targets.DepthBuffer.Get()); 
        DebugViewPass.Initialize(Device.Native(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        CreateDebugPreviewTargets();
        Targets.CreateHDRBuffers(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateVelocityBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateUpscaleMasks(Device.Native(), RenderWidth(), RenderHeight());
        Targets.CreateSceneCopies(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        CreateConstantBuffer();
        CreateDefaultMaterial();
        BuildDefaultScene();

        ReportInitProgress("Pré-computando IBL, céu e atmosfera", {}, 0.50f);
        HDREnv.Initialize(Device.Native(), CommandQueue, SRVHeap);
        CreateIBLDescriptorTable();

        Skybox.Initialize(Device.Native(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        Atmosphere.Initialize(Device.Native(), CommandQueue, UploadQueue, SRVHeap,
                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        ReportInitProgress("Gerando ruídos de nuvem e cascatas do oceano", {}, 0.60f);
        CloudNoise.Initialize(Device.Native(), CommandQueue, SRVHeap);
        VolumetricClouds.Initialize(Device.Native(), SRVHeap, CloudNoise,
                                    Atmosphere.TransmittanceSRV(), Atmosphere.MultiScatterSRV(),
                                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
                                    SwapChain.GetWidth(), SwapChain.GetHeight());

        static_assert(kDefaultOceanCascades.size() == kOceanCascades);
        for (u32 c = 0; c < kOceanCascades; ++c) {
            const auto& Cascade = kDefaultOceanCascades[c];
            Ocean[c].ConfigureCascade(Cascade.Seed, Cascade.TileMetres,
                                      Cascade.LowCycles, Cascade.HighCycles);
        }

        for (u32 c = 0; c < kOceanCascades; ++c)
            Ocean[c].Initialize(Device.Native(), SRVHeap);
        Water.Initialize(Device.Native(), UploadQueue,
                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
                         DXGI_FORMAT_R16G16_FLOAT);

        Fog.Initialize(Device.Native(), DXGI_FORMAT_R16G16B16A16_FLOAT);

        VolumetricFog.Initialize(Device.Native(), SRVHeap);

        SunShafts.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        RainWetness.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        ReportInitProgress("Preparando sombras, terreno e pós-processo", {}, 0.70f);
        SunShadows.Initialize(Device.Native(), SRVHeap);
        LocalShadows.Initialize(Device.Native(), SRVHeap);

        Terrain.Initialize(Device.Native());

        PostProcessor.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        ObjectPicker.Initialize(Device.Native(), SwapChain.GetWidth(), SwapChain.GetHeight());

        SelectionOutline.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        DebugDraw.Initialize(Device.Native(), FSwapChain::kFormat);

        ReportInitProgress("Iniciando upscalers e oclusão de ambiente", {}, 0.80f);
        TemporalAA.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());
        TemporalAA.SetupInputs(Device.Native(), SRVHeap, Targets.HDRColorBuffer.Get(), Targets.DepthBuffer.Get(), Targets.VelocityBuffer.Get());

        Fsr.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                        SwapChain.GetWidth(), SwapChain.GetHeight());
        Dlss.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                        SwapChain.GetWidth(), SwapChain.GetHeight());
        DlssRR.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                          SwapChain.GetWidth(), SwapChain.GetHeight());
        RRGuides.Initialize(Device.Native());
        RRGuides.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        BgVelocity.Initialize(Device.Native());

        Flicker.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        AO.Initialize(Device.Native());
        // O dimensionamento dela sai daqui: e um passe migrado, entao quem o faz e o
        // Passes.ResizeAll logo apos o RegisterPasses (abaixo).

        HiZ.Initialize(Device.Native());
        HiZ.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        if (Device.RaytracingSupported()) {
            ReportInitProgress("Compilando pipelines de ray tracing", {}, 0.88f);
            // A NVAPI precisa reservar o slot antes da criacao das PSOs instrumentadas.
            // A falha e esperada em GPUs sem suporte; nesses casos nao ha permutacao de timer.
            FShaderTimer::InitializeApi(Device.Native(), SRVHeap);
            BvhDebug.Initialize(Device.Native());
            // O fallback precisa existir antes dos consumidores que copiam seus descriptors.
            GIFallback.Initialize(Device.Native(), CommandQueue, SRVHeap);
            DDGI.Initialize(Device.Native());
            ReGIR.Initialize(Device.Native());
            RadianceCache.Initialize(Device.Native());
            MeshLights.Initialize(Device.Native());
            ReSTIRGI.Initialize(Device.Native());
            Nrd.Initialize(Device.Native());
            NrdDirect.Initialize(Device.Native(), FNrdDenoiser::ESignalProfile::Direct);
            Reflections.Initialize(Device.Native());
            ReSTIRDI.Initialize(Device.Native());
            TemporalMotion.Initialize(Device.Native());
            DDGIDebugPass.Initialize(Device.Native(),
                                     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);
        }

        ReportInitProgress("Construindo estruturas de aceleração", {}, 0.96f);
        BuildRaytracingScene();

        RegisterPasses();

        // O primeiro resize precisa ocorrer depois de todos os passes, inclusive os de RT,
        // estarem inicializados e registrados.
        Passes.ResizeAll(MakePassInitContext());

        Initialized = true;
        ReportInitProgress("Renderizador pronto", {}, 1.0f);

        std::string Features = Device.RaytracingSupported() ? "DXR" : "Raster";
        if (Fsr.Available())    Features += " | FSR";
        if (Dlss.Available())   Features += " | DLSS";
        if (DlssRR.Available()) Features += " | DLSS-RR";
        const auto InitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - InitStarted).count();
        LogInfo("Renderer pronto em " + std::to_string(InitMs) + " ms | " +
                std::to_string(SwapChain.GetWidth()) + "x" +
                std::to_string(SwapChain.GetHeight()) + " | " + Features);
    }

    // O registro e incondicional; cada passe decide se esta inicializado/ativo.
    void Renderer::RegisterPasses() {
        // Objetos sob demanda/compartilhados possuem PSO, mas nao gravam um passe por frame.
        static_assert(!std::is_base_of_v<FRenderPass, FPipelineState>,
            "FPipelineState e estado compartilhado, nao passe de frame");
        static_assert(!std::is_base_of_v<FRenderPass, FHDREnvironment>, "baker, nao passe de frame");
        static_assert(!std::is_base_of_v<FRenderPass, FCloudNoise>,     "baker, nao passe de frame");
        static_assert(!std::is_base_of_v<FRenderPass, FMaterialPreview>, "baker sob demanda");
        static_assert(std::is_base_of_v<FRenderPass, FWaterRenderer>,   "a agua grava frame");
        static_assert(std::is_base_of_v<FRenderPass, FAmbientOcclusion>, "a GTAO grava frame");

        Passes.Clear();

        // Donos de pipeline que nao gravam frame.
        Passes.RegisterPipelineOwner(&PipelineState);  // root sig + PSOs que as fases bindam
        Passes.RegisterPipelineOwner(&HDREnv);         // bake do IBL ao carregar um HDRI
        Passes.RegisterPipelineOwner(&CloudNoise);     // bake do ruido 3D / mapa de clima
        Passes.RegisterPipelineOwner(&MaterialPreview);// preview offscreen do editor

        // Passes de frame. A ordem tambem define a ordem dos hooks de ciclo de vida.
        Passes.RegisterPass(&Skybox);
        Passes.RegisterPass(&Atmosphere);
        Passes.RegisterPass(&VolumetricClouds);
        Passes.RegisterPass(&Water);
        for (u32 c = 0; c < kOceanCascades; ++c) Passes.RegisterPass(&Ocean[c]);
        Passes.RegisterPass(&Terrain);
        Passes.RegisterPass(&MeshLights);
        Passes.RegisterPass(&ReGIR);
        Passes.RegisterPass(&RadianceCache);
        Passes.RegisterPass(&DDGI);
        Passes.RegisterPass(&SunShadows);
        Passes.RegisterPass(&HiZ);
        Passes.RegisterPass(&AO);
        Passes.RegisterPass(&BgVelocity);
        Passes.RegisterPass(&RainWetness);
        Passes.RegisterPass(&TemporalMotion);
        Passes.RegisterPass(&ReSTIRGI);
        Passes.RegisterPass(&ReSTIRDI);
        Passes.RegisterPass(&Reflections);
        Passes.RegisterPass(&VolumetricFog);
        Passes.RegisterPass(&SunShafts);
        Passes.RegisterPass(&Fog);
        Passes.RegisterPass(&RRGuides);
        Passes.RegisterPass(&TemporalAA);
        Passes.RegisterPass(&PostProcessor);
        Passes.RegisterPass(&ObjectPicker);
        Passes.RegisterPass(&SelectionOutline);
        Passes.RegisterPass(&DebugDraw);
        Passes.RegisterPass(&Flicker);
        Passes.RegisterPass(&BvhDebug);
        Passes.RegisterPass(&DDGIDebugPass);
        // As duas instancias compartilham stems e precisam ser recriadas no mesmo reload.
        Passes.RegisterPass(&DebugViewPass);
        Passes.RegisterPass(&DebugPreviewPass);
    }

    FPassInitContext Renderer::MakePassInitContext() {
        FPassInitContext Ctx;
        Ctx.Device       = Device.Native();
        Ctx.SRVHeap      = &SRVHeap;
        Ctx.Targets      = &Targets;
        Ctx.RenderWidth  = RenderWidth();
        Ctx.RenderHeight = RenderHeight();
        return Ctx;
    }

    bool Renderer::IsBvhDebugAvailable() const {
        // Precisa da TLAS E do snapshot de instancias — o passe le os dois. Antes da primeira
        // cena carregar, o toggle fica desabilitado em vez de ligar e nao produzir imagem.
        return Device.RaytracingSupported() && RaytracingScene.IsBuilt() && BvhDebug.IsReady();
    }

    bool Renderer::IsRtShaderTimerAvailable() const {
        // Exige os dois lados: a NVAPI ligada (C++) E as permutacoes instrumentadas criadas
        // (shader). Ter so um dos dois deixaria o toggle ligar sem produzir imagem nenhuma.
        return FShaderTimer::IsAvailable() &&
               ReSTIRGI.HasTimerPipeline() && Reflections.HasTimerPipeline();
    }

    // Reload completo e por stem percorrem o mesmo registro de donos de pipeline.
    void Renderer::RecreateAllPSOs() {
        Passes.RecreateAllPipelines(MakePassInitContext());
    }

    void Renderer::NotifyShaderReloadQueued(const std::string& _ChangedStem) {
        if (!Initialized) return;
        // Um .hlsli força reload completo; um stem nomeado só afeta a captura quando possui dono.
        if (!_ChangedStem.empty() && !Passes.OwnerOfShaderStem(_ChangedStem)) return;
        Capture.Cancel("um shader mudou no disco durante o aquecimento");
    }

    bool Renderer::ReloadShaders(const std::string& _ChangedStem) {
        if (!Initialized) return false;
        try {
            CommandQueue.Flush();

            // A captura nao pode misturar frames produzidos por builds diferentes do shader.
            // Cancele antes da primeira mutacao, inclusive quando varias instancias casam o stem.
            if (_ChangedStem.empty()) {
                Capture.Cancel("os shaders foram recarregados durante o aquecimento");
                RecreateAllPSOs();
                LogInfo("Shaders recarregados (reload completo)");
                return true;
            }

            const FPassInitContext Ctx = MakePassInitContext();
            const char* Owner = Passes.OwnerOfShaderStem(_ChangedStem);
            if (Owner) Capture.Cancel("os shaders foram recarregados durante o aquecimento");
            const u32 Hits = Passes.RecreatePipelinesForStem(_ChangedStem, Ctx);
            if (Hits > 0) {
                LogInfo("Shader recarregado: " + _ChangedStem + " (passe: " + Owner +
                        (Hits > 1 ? ", " + std::to_string(Hits) + " instancias)" : ")"));
                return true;
            }

            LogWarning("Shader '" + _ChangedStem +
                       "' compilado, mas sem pipeline mapeado para hot reload");
            return false;
        } catch (const std::exception& e) {
            LogError(std::string("Erro ao recarregar shaders: ") + e.what());
            return false;
        }
    }

    void Renderer::Resize(u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) return;
        // Um resize reinicia historicos e invalida o aquecimento da captura atual.
        Capture.Cancel("a janela foi redimensionada durante o aquecimento");
        CommandQueue.Flush();
        SwapChain.Resize(Device.Native(), _Width, _Height);
        RecreateInternalTargets();
    }

    void Renderer::SetRenderScale(f32 _Scale) {
        // O DLSS-RR exige a resolucao de entrada definida pelo modo de qualidade.
        if (Denoiser == EDenoiser::DLSS_RR) {
            LogWarning("Escala de renderizacao ignorada: com o DLSS Ray Reconstruction a resolucao de "
                       "entrada vem do modo de qualidade (o RR nao suporta resolucao dinamica)");
            return;
        }
        ApplyRenderScale(_Scale);
    }

    void Renderer::ApplyRenderScale(f32 _Scale) {
        _Scale = _Scale < 0.33f ? 0.33f : (_Scale > 2.0f ? 2.0f : _Scale);
        if (_Scale == RenderScale) return;
        // Recriar os alvos invalida o aquecimento, exceto durante a aplicacao do proprio preset.
        if (!CaptureSetupGuard)
            Capture.Cancel("a escala de renderizacao mudou durante o aquecimento");
        RenderScale = _Scale;
        if (!Initialized || SwapChain.GetWidth() == 0) return;
        CommandQueue.Flush();
        RecreateInternalTargets();
    }

    void Renderer::SetCloudsHalfRes(bool _HalfRes) {
        if (VolumetricClouds.GetHalfRes() == _HalfRes) return;
        VolumetricClouds.SetHalfRes(_HalfRes);
        if (!Initialized || !VolumetricClouds.IsInitialized()) return;
        CommandQueue.Flush();
        VolumetricClouds.Resize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
    }

    void Renderer::SetCloudWeatherSeed(u32 _Seed) {
        CloudNoise.SetSeed(_Seed);
        if (!Initialized || !CloudNoise.IsInitialized()) return;
        CommandQueue.Flush();
        CloudNoise.ClearWeatherOverride(SRVHeap); // mexer no procedural desativa a autorada
        CloudNoise.RebakeWeather(CommandQueue, SRVHeap);
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
    }

    void Renderer::SetCloudWeatherCells(u32 _Mult) {
        CloudNoise.SetCellMult(_Mult);
        if (!Initialized || !CloudNoise.IsInitialized()) return;
        CommandQueue.Flush();
        CloudNoise.ClearWeatherOverride(SRVHeap);
        CloudNoise.RebakeWeather(CommandQueue, SRVHeap);
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
    }

    bool Renderer::LoadCloudWeatherTexture(const std::wstring& _Path) {
        if (!Initialized || !CloudNoise.IsInitialized()) return false;
        CommandQueue.Flush();
        if (!CloudNoise.LoadWeatherOverride(Device.Native(), UploadQueue, SRVHeap, _Path))
            return false;
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
        return true;
    }

    void Renderer::ClearCloudWeatherTexture() {
        if (!Initialized || !CloudNoise.HasWeatherOverride()) return;
        CommandQueue.Flush();
        CloudNoise.ClearWeatherOverride(SRVHeap);
        VolumetricClouds.SetWeatherSRV(Device.Native(), SRVHeap, CloudNoise.WeatherSRV());
    }

    void Renderer::RecreateInternalTargets() {
        // Os chamadores drenam a fila antes de qualquer recurso ser substituido.
        const auto RecreateStart = std::chrono::steady_clock::now();
        GpuResources::ResetCreationStats();

        // Builds de diagnostico registram descriptors reais para o Tools/AllocBench.
        // A variavel de ambiente altera apenas o caminho da captura.
#if SMILE_DIAGNOSTICS
        const char* CaptureOverride = std::getenv("SMILE_CAPTURE_DESCS");
        GpuResources::FDescCaptureSession DescCapture(
            CaptureOverride ? CaptureOverride : "smile-resize-descs.txt");
#endif

        const u32 RW = RenderWidth(),        RH = RenderHeight();
        const u32 SW = SwapChain.GetWidth(), SH = SwapChain.GetHeight();

        Targets.CreateHDRBuffers(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateDepthBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateNormalBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        GBuffer.Resize(Device.Native(), SRVHeap, RW, RH);
        GBuffer.WriteDepthSRV(Device.Native(), SRVHeap, Targets.DepthBuffer.Get());
        Targets.CreateVelocityBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateUpscaleMasks(Device.Native(), RenderWidth(), RenderHeight());

        VolumetricClouds.Resize(Device.Native(), SRVHeap, RW, RH);
        Water.Resize(Device.Native(), RW, RH);
        RainWetness.Resize(Device.Native(), SRVHeap, RW, RH);
        SunShafts.Resize(Device.Native(), SRVHeap, RW, RH);
        Targets.CreateSceneCopies(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

        PostProcessor.Resize(Device.Native(), SRVHeap, SW, SH);    
        ObjectPicker.Resize(Device.Native(), RW, RH);
        SelectionOutline.Resize(Device.Native(), SRVHeap, SW, SH); 

        TemporalAA.Resize(Device.Native(), SRVHeap, RW, RH);
        TemporalAA.SetupInputs(Device.Native(), SRVHeap, Targets.HDRColorBuffer.Get(), Targets.DepthBuffer.Get(), Targets.VelocityBuffer.Get());
        TAARanLastFrame = false;

        Fsr.Initialize(Device.Native(), SRVHeap, RW, RH, SW, SH);
        Dlss.Initialize(Device.Native(), SRVHeap, RW, RH, SW, SH);
        DlssRR.Initialize(Device.Native(), SRVHeap, RW, RH, SW, SH);
        RRGuides.SetupForResize(Device.Native(), SRVHeap, RW, RH);
        Flicker.Resize(Device.Native(), SRVHeap, RW, RH);
        FlickerResetPending = true;

        // Passes que ja adotaram o contrato. Fica NESTE ponto (e nao no topo) porque o
        // OnResize deles le Targets.*SRVSlot, que as criacoes acima acabaram de reatribuir.
        Passes.ResizeAll(MakePassInitContext());

        HiZ.SetupForResize(Device.Native(), SRVHeap, RW, RH);

        SetupReflectionsForScene();
        if (DDGI.IsReady()) {
            DDGIDebugPass.SetupPointDiagnosticInputs(
                Device.Native(), SRVHeap, GBuffer.SRVTableStart(), DDGI);
        }

        RegisterDebugTargets();

        const auto RecreateMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - RecreateStart).count();
        LogDebug("[Resize] alvos internos recriados em " +
                 std::to_string(static_cast<int>(RecreateMs)) + " ms (" +
                 std::to_string(RW) + "x" + std::to_string(RH) + " render, " +
                 std::to_string(SW) + "x" + std::to_string(SH) + " display)");
        GpuResources::LogCreationStats("resize");
#if SMILE_DIAGNOSTICS
        // A sessao desliga a captura mesmo se a recriacao lancar uma excecao.
        DescCapture.Complete();
#endif
    }

    // Existencia do volume e politica de iluminacao sao perguntas distintas.
    bool Renderer::DDGIVolumeLive() const { return UseGI && DDGI.IsReady(); }

    // A iluminacao volumetrica nao possui substituto para o atlas DDGI.
    bool Renderer::DDGIVolumetricAvailable() const { return DDGIVolumeLive(); }

    // Mesmo com SHaRC primario, DDGI ainda atende folhagem, subsurface e translucidos.
    bool Renderer::DDGISurfaceAvailable() const {
        return DDGIVolumeLive() && EffectivePrimary() != EIndirectPrimary::Off;
    }

    // Capacidade alimenta a politica; FFrameModes e apenas uma consequencia dela.
    bool Renderer::ReSTIRGIReady() const    { return UseReSTIRGI && ReSTIRGI.IsReady(); }
    bool Renderer::ReflectionsReady() const { return UseReflections && Reflections.IsReady(); }

    FEffectiveIndirectPolicy Renderer::EffectiveIndirectPolicy() const {
        FEffectiveIndirectPolicy P;
        P.VolumeLive = DDGIVolumeLive();
        P.Primary    = EffectivePrimary();
        P.Fallback   = EffectiveFallback();
        // Reflexoes e segundo bounce tambem consomem o fallback compartilhado.
        P.FallbackActive = P.Primary == EIndirectPrimary::ReSTIR_SHaRC ||
                           ReflectionsReady() || P.VolumeLive;
        P.DDGISurface    = DDGISurfaceAvailable();
        P.DDGIVolumetric = DDGIVolumetricAvailable();
        return P;
    }

    EIndirectPrimary Renderer::EffectivePrimary() const {
        // SHaRC degrada para DDGI quando possivel; apenas um pedido Off desliga o indireto.
        if (IndirectPrimary == EIndirectPrimary::ReSTIR_SHaRC && ReSTIRGIReady())
            return EIndirectPrimary::ReSTIR_SHaRC;
        if (IndirectPrimary != EIndirectPrimary::Off && DDGIVolumeLive())
            return EIndirectPrimary::DDGI;
        return EIndirectPrimary::Off;
    }

    EIndirectFallback Renderer::EffectiveFallback() const {
        // O estado efetivo precisa refletir o que o shader realmente produz.
        if (IndirectFallback == EIndirectFallback::DDGI && !DDGIVolumeLive())
            return EIndirectFallback::Black;
        // Environment ainda nao possui sinal publicado nos passes de RT.
        if (IndirectFallback == EIndirectFallback::Environment)
            return EIndirectFallback::Black;
        return IndirectFallback;
    }

    void Renderer::PresentFrame() {
        SwapChain.Present();
    }

    void Renderer::Shutdown() {
        if (!Initialized) return;
        CommandQueue.Flush();
        Capture.Release();
        ComputeQueue.Shutdown();
        UploadQueue.Shutdown();
        Nrd.Shutdown();
        NrdDirect.Shutdown();
        Fsr.Shutdown();
        Dlss.Shutdown();
        DlssRR.Shutdown();
        RRGuides.Shutdown();
        BgVelocity.Shutdown();
        FDlssPass::ShutdownStreamline();   // desliga o Streamline apos liberar os recursos do DLSS/RR
        BvhDebug.Release(SRVHeap);
        GIFallback.Release(SRVHeap);       // 3 slots + os recursos neutros do fallback indireto
        FShaderTimer::ShutdownApi();       // libera o slot falso da extensao + o buffer dummy
        if (ConstantBuffer && MappedFrameBase) {
            ConstantBuffer->Unmap(0, nullptr);
            MappedFrameBase = nullptr;
        }
        if (ObjectCB && MappedObjectCB) {
            ObjectCB->Unmap(0, nullptr);
            MappedObjectCB = nullptr;
        }
        Initialized = false;
        LogDebug("Renderer encerrado");
    }
}
