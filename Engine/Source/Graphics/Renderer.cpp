#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Graphics/OceanSpectrum.h"
#include "Smile/Graphics/RTMasks.h" // kRTMaskShadowFull: mascara dos shadow rays de direta local
#include "Smile/Graphics/Barriers.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Graphics/RayEpsilons.h"
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
        f32 Halton(u32 i, u32 b) {
            f32 f = 1.0f, r = 0.0f;
            while (i > 0) { f /= static_cast<f32>(b); r += f * static_cast<f32>(i % b); i /= b; }
            return r;
        }

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

        constexpr DXGI_FORMAT kDebugPreviewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr u32 kDebugPreviewRowPitch =
            (Renderer::kDebugPreviewWidth * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        constexpr u64 kDebugProbeIrrOffset  = 0;
        constexpr u64 kDebugProbeDistOffset = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        constexpr u64 kDebugProbeReadbackSize =
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * 2ull;

        f32 HalfToFloat(u16 H) {
            const u32 Sign = static_cast<u32>(H & 0x8000u) << 16;
            u32 Exp  = (H >> 10) & 0x1Fu;
            u32 Mant = H & 0x03FFu;
            u32 Bits;
            if (Exp == 0) {
                if (Mant == 0) {
                    Bits = Sign;
                } else {
                    i32 E = -14;
                    while ((Mant & 0x0400u) == 0) { Mant <<= 1; --E; }
                    Mant &= 0x03FFu;
                    Bits = Sign | (static_cast<u32>(E + 127) << 23) | (Mant << 13);
                }
            } else if (Exp == 0x1Fu) {
                Bits = Sign | 0x7F800000u | (Mant << 13);
            } else {
                Bits = Sign | ((Exp + 112u) << 23) | (Mant << 13);
            }
            f32 Result;
            std::memcpy(&Result, &Bits, sizeof(Result));
            return Result;
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
        DebugPreviewPass.Initialize(Device.Native(), kDebugPreviewFormat);
        CreateDebugPreviewTargets();
        Targets.CreateHDRBuffers(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateVelocityBuffer(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        Targets.CreateUpscaleMasks(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
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
            // ANTES das pipelines: o slot falso da extensao da NVAPI precisa estar reservado no
            // device na hora em que a PSO instrumentada e criada, senao o driver nao reconhece o
            // padrao do timer e a permutacao nasce invalida. Falhar aqui e normal (GPU nao-NVIDIA):
            // os passes so nao criam a gemea instrumentada.
            FShaderTimer::InitializeApi(Device.Native(), SRVHeap);
            BvhDebug.Initialize(Device.Native());
            // ANTES do DDGI e dos consumidores: o SetupReflectionsForScene le estes slots para
            // montar as tabelas de reflexao/ReSTIR GI quando nao ha volume, e sem eles os dois
            // passes recusariam o setup por slot invalido — que e exatamente o estado que a
            // remocao do early-out existe para evitar.
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

        // Dimensionamento INICIAL dos passes migrados. Sem isto o OnResize deles so acontecia no
        // primeiro Resize()/ApplyRenderScale() da viewport — que pode nunca vir, porque a
        // SwapChain ja nasce no tamanho certo e o Resize e disparado por MUDANCA de tamanho.
        // Os passes nao-migrados recebem o SetupForResize a dedo la em cima; este e o equivalente
        // para quem ja adotou o contrato, e e por isso que a linha da GTAO saiu de la.
        //
        // Aqui e o ponto mais cedo possivel: depois do RegisterPasses (a lista tem de existir) e
        // depois do bloco de RT (o OnResize de um passe de RT desiste se o Initialize dele nao
        // rodou, e o do radiance cache faz exatamente isso).
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

    // Fila de adocao do FRenderPass. Registrar e INCONDICIONAL de proposito, inclusive para os
    // passes de RT: cada override ja se protege com o proprio Initialized, e uma segunda lista
    // condicional para manter em sincronia com a de cima seria reintroduzir exatamente o defeito
    // que este registro existe para eliminar.
    void Renderer::RegisterPasses() {
        // --- Invariante deste registro, verificada pelo compilador -----------------------
        // Estes tres tem shader e PSO mas NAO gravam frame: o FPipelineState e um saco de PSOs que
        // os outros bindam, e os dois bakers rodam ao carregar HDRI / trocar a seed do clima. Se
        // algum deles voltar a herdar FRenderPass, ele reentra na lista de passes de frame em
        // silencio e aparece na UI de custo com 0 ms e nada para desligar. Entao o erro e aqui.
        static_assert(!std::is_base_of_v<FRenderPass, FPipelineState>,
            "FPipelineState e estado compartilhado, nao passe de frame — ver as duas bases no RenderPass.h");
        static_assert(!std::is_base_of_v<FRenderPass, FHDREnvironment>, "baker, nao passe de frame");
        static_assert(!std::is_base_of_v<FRenderPass, FCloudNoise>,     "baker, nao passe de frame");
        // Preview offscreen do Editor de Materiais: sai por Submit/LoadEnvironment, nunca pelo
        // RenderFrame. Se virar passe de frame, entra no resize e na invalidacao de historico
        // de graca — e nenhum dos dois faz sentido para um alvo que ele mesmo dimensiona.
        static_assert(!std::is_base_of_v<FRenderPass, FMaterialPreview>, "baker sob demanda");
        // E a contraparte: quem grava frame tem de estar do lado certo.
        static_assert(std::is_base_of_v<FRenderPass, FWaterRenderer>,   "a agua grava frame");
        static_assert(std::is_base_of_v<FRenderPass, FAmbientOcclusion>, "a GTAO grava frame");

        Passes.Clear();

        // --- Donos de pipeline que NAO gravam frame ------------------------------------
        // Participam do hot reload e de nada mais. Ficam agrupados e com a funcao de nome
        // proprio justamente para nao se confundirem com a lista de baixo.
        Passes.RegisterPipelineOwner(&PipelineState);  // root sig + PSOs que as fases bindam
        Passes.RegisterPipelineOwner(&HDREnv);         // bake do IBL ao carregar um HDRI
        Passes.RegisterPipelineOwner(&CloudNoise);     // bake do ruido 3D / mapa de clima
        Passes.RegisterPipelineOwner(&MaterialPreview);// preview offscreen do editor

        // --- Passes de frame, em ordem de gravacao -------------------------------------
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
        // Duas instancias do MESMO passe, com formatos de alvo diferentes (viewport e a grade
        // offscreen da janela de debug). Ambas precisam recompilar: e o caso que motivou o
        // RecreatePipelinesForStem casar TODOS os donos em vez de parar no primeiro.
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

    // Duas exigencias DIFERENTES pesam sobre esta funcao, e confundi-las custou uma regressao:
    //
    // (1) LIFETIME — o Build comeca com um Release que solta BlasPool, TLAS, scratch, os uploads
    //     de instancia, o snapshot InstanceGeo e os descritores dos tres. O lock do RendererHandle
    //     serializa a CPU e nao diz nada sobre a GPU: no editor esta chamada vem da thread da GUI
    //     com o ultimo frame ainda EM VOO, tracando contra exatamente esses recursos. Por isso o
    //     dreno vive AQUI e nao no chamador — a exposicao e da funcao, qualquer sitio que a chame
    //     a tem. O SetupGIForScene tambem drena, e continua precisando: ele roda sozinho pelo
    //     RebuildGIVolume. Drenar fila ja parada custa zero, que e o caso do load.
    //
    //     A ordem das duas e a mesma do SetupGIForScene e pelo mesmo motivo: o update do DDGI e
    //     submetido na COMPUTE com um Wait no fence da DIRETA (SubmitAfter no RenderFrame), entao
    //     drenar a direta primeiro e o que deixa o compute pendente de fato terminar em vez de
    //     ficar preso no proprio Wait.
    //
    //     ⚠️ Nao confiar no Flush do RecreateObjectCB: ele so dispara quando `Count > MaxObjects`
    //     e alcanca so a DIRETA. O caminho em que o NeedsResize dispara pela capacidade do
    //     InstanceGeo sem estourar o MaxObjects chegava aqui sem dreno nenhum, com o DDGI
    //     assincrono lendo o snapshot na compute.
    //
    // (2) O PAR com SetupGIForScene — outro problema, e o dreno nao o resolve. A tabela de trace
    //     do DDGI carrega uma COPIA do descritor do snapshot (CopyDescriptors), entao ela nao
    //     acompanha a realocacao sozinha; quem a reconstroi e o DDGI.SetupForScene. Toda chamada
    //     daqui tem de ser seguida de um. Os dois sitios de hoje cumprem: o do load roda com a
    //     cena vazia (o Build desiste sem criar nada, e o DDGI ainda nao tem tabela) e o do
    //     OnSceneStructureChanged chama os dois em sequencia.
    void Renderer::BuildRaytracingScene() {
        if (!Device.RaytracingSupported()) return;
        CommandQueue.Flush();
        ComputeQueue.WaitIdle();
        RaytracingScene.Build(Device, CommandQueue, SRVHeap, Scene);
        TlasTransformsVersion = Scene.TransformsVersion();
    }

    void Renderer::SetCameraPose(const Vec3& _Pos, f32 _PitchDeg, f32 _YawDeg) {
        // Teleporte no meio de uma captura invalidaria o aquecimento inteiro em silencio — os
        // caches de mundo ja acumularam N frames a partir da pose anterior, e a imagem sairia de
        // uma pose sem o aquecimento dela. Recusar e melhor que cancelar: o duplo-clique no
        // outliner e acidental por natureza, e cancelar jogaria fora minutos de aquecimento.
        //
        // Nao afeta o fluxo normal do disparo: o "Capturar" do slot restaura a pose ANTES de
        // enfileirar o pedido, e ate ali o Busy() ainda e falso.
        if (Capture.Busy()) {
            LogWarning("Camera travada: ha uma captura deterministica em andamento");
            return;
        }
        Camera.SetPose(_Pos, _PitchDeg, _YawDeg);
        // Teleporte e o caso classico de corte: nenhum vetor de movimento liga o frame novo ao
        // antigo, e reprojetar traria imagem do lugar de onde a camera saiu. Antes daqui isto
        // nao avisava ninguem — a engine resetava os filtros de tela em todo knob de conteudo e
        // NAO resetava no unico evento que de fato pedia.
        Settings().NotifyCameraCut();
    }

    void Renderer::SetSelectedObject(int _Index) {
        if (_Index < 0 || _Index >= static_cast<int>(Scene.Renderables().size())) {
            ClearSelection();
            return;
        }
        Selection = { Scene.IdAt(static_cast<u32>(_Index)), ESceneObject::Renderable,
                      static_cast<u32>(_Index) };
    }

    int Renderer::GetSelectedObject() const {
        return Selection.IsRenderable() ? static_cast<int>(Selection.Index) : -1;
    }

    u64 Renderer::GetSelectedObjectId() const {
        return Selection.IsRenderable() ? Selection.Id : 0ull;
    }

    // Escopadas por tipo: limpar "a selecao de mesh" quando ha uma LUZ selecionada e no-op, nao
    // limpa a luz. Preserva o significado que os call sites do editor ja tinham quando os dois
    // campos eram separados.
    void Renderer::ClearSelection() {
        if (Selection.IsRenderable()) Selection = {};
    }

    void Renderer::ClearLightSelection() {
        if (Selection.IsLight()) Selection = {};
    }

    void Renderer::SetSelectedLight(int _Index) {
        if (_Index < 0 || _Index >= static_cast<int>(Scene.Lights().size())) {
            ClearLightSelection();
            return;
        }
        // A luz pode nao ter identidade ainda: o editor faz push_back direto e quem atribui e o
        // RenderFrame, no proximo frame. Selecionar e um bom momento para adiantar — sem isso a
        // selecao ficaria com Id 0, ou seja, invalida, ate um frame passar.
        FLight& L = Scene.Lights()[static_cast<size_t>(_Index)];
        if (L.Id == 0) L.Id = Scene.AllocObjectId();
        Selection = { L.Id, ESceneObject::Light, static_cast<u32>(_Index) };
    }

    int Renderer::GetSelectedLight() const {
        return Selection.IsLight() ? static_cast<int>(Selection.Index) : -1;
    }

    bool Renderer::RemoveRenderable(u64 _Id) {
        // AABB capturada ANTES da remocao: depois dela o objeto nao existe mais e nao ha de onde
        // tirar a regiao que o GI precisa reavaliar.
        const FRenderable* Doomed = Scene.FindRenderable(_Id);
        if (!Doomed) return false;
        const Vec3 Min = Doomed->AABBMin, Max = Doomed->AABBMax;
        if (!Scene.RemoveRenderable(_Id)) return false;
        OnSceneStructureChanged(&Min, &Max);
        return true;
    }

    u64 Renderer::DuplicateRenderable(u64 _Id) {
        const FRenderable* Added = Scene.DuplicateRenderable(_Id);
        if (!Added) return 0;
        // Le ANTES do re-setup, que pode realocar a lista.
        const u64  NewId = Added->Id;
        const Vec3 Min = Added->AABBMin, Max = Added->AABBMax;
        OnSceneStructureChanged(&Min, &Max);
        return NewId;
    }

    void Renderer::NotifyGIRegionChanged(const Vec3& _Min, const Vec3& _Max,
                                         EGIRegionChange _Change) {
        DDGI.InvalidateRegion(_Min, _Max, _Change);
    }

    void Renderer::OnSceneStructureChanged(const Vec3* _ChangedMin, const Vec3* _ChangedMax) {
        const u32 Count = static_cast<u32>(Scene.Renderables().size());

        // Regiao que o GI precisa reavaliar. Invalidacao ESPACIAL, no lugar de derrubar o atlas
        // inteiro: o dominio SceneStructure nao carrega mais DDGIAtlas justamente por isto.
        // Geometria por definicao: quem chega aqui criou, removeu ou trocou um renderavel.
        if (_ChangedMin && _ChangedMax)
            DDGI.InvalidateRegion(*_ChangedMin, *_ChangedMax, EGIRegionChange::Geometry);

        // (1) Quem fala em indice e sobrevive ao frame, do lado da CPU.
        // O pick resolve kFramesInFlight depois do pedido: um pick em voo agora devolveria o
        // indice de outro objeto, entao ele morre aqui em vez de virar uma selecao errada.
        ObjectPicker.CancelPending();
        // Uma linha reancora os dois tipos: o FindObject devolve onde aquela identidade esta
        // agora, ou um ref invalido se ela deixou de existir.
        Selection = Scene.FindObject(Selection.Id);
        // PrevModels e o transform do frame anterior POR INDICE (motion vector do raster). Com a
        // lista deslocada, cada entrada passou a descrever outro objeto. Limpar faz o proximo
        // frame usar PrevModel = Model, ou seja, movimento zero — que e exatamente o que um
        // objeto recem-criado ja recebia. Remapear por Id daria o mesmo resultado visivel: o
        // TAA cai junto (dominio abaixo), entao nao ha historico para preservar.
        PrevModels.clear();

        // (2) Estruturas de GPU dimensionadas pelo tamanho da cena. Todas nascem com a MESMA
        // folga (SceneCapacityFor), entao o caso comum — criar ou apagar alguns objetos — cabe
        // e cai no caminho barato abaixo. Quando a folga acaba nao ha remendo: o snapshot do
        // InstanceGeo alocaria descriptors novos, a TLAS precisa de scratch maior e o buffer de
        // transforms do TemporalMotion e um SRV com NumElements fixo. Ai refaz o setup de cena
        // inteiro, que e o mesmo caminho do load.
        //
        // O teste e por ESTRUTURA e nao pela folga nominal de proposito: quem foi criado antes
        // de existir TLAS (ou com o RT desligado) tem capacidade 0, e os guardas de > 0 abaixo
        // deixam essas passarem em vez de forcar um re-setup que nao teria o que reconstruir.
        const bool NeedsResize = Count > MaxObjects
                              || Count > HiZ.Capacity()
                              || (RaytracingScene.IsBuilt() &&
                                  Count > RaytracingScene.InstanceCapacity())
                              || (TemporalMotion.InstanceCount() > 0 &&
                                  Count > TemporalMotion.InstanceCount())
                              || (RaytracingScene.InstanceGeoCapacity() > 0 &&
                                  Count > RaytracingScene.InstanceGeoCapacity());

        if (NeedsResize) {
            if (Count > MaxObjects) {
                // De novo COM folga, senao cada objeto criado a partir daqui pagaria o
                // re-setup completo — que e exatamente o que a folga existe para evitar.
                MaxObjects = SceneCapacityFor(Count);
                RecreateObjectCB(); // ja da Flush na fila (e recria a tabela do HiZ)
            }
            HiZ.SetupObjects(Device.Native(), SRVHeap, MaxObjects);
            BuildRaytracingScene();
            SetupGIForScene(SceneBoundsMin, SceneBoundsMax);
        } else {
            // Caminho barato (remocao, ou copia que ainda cabe): so o snapshot que o RT le por
            // InstanceID precisa reacompanhar a lista. Mesmo dreno do NotifyMaterialRTStateChanged
            // e pela mesma razao — o InstanceGeo e upload heap sem versao por frame em voo.
            //
            // As DUAS filas: aqui havia so o Flush da direta, e o DDGI le o snapshot pela tabela
            // de trace dele, que roda na COMPUTE. Reescrever o buffer com esse trace em voo
            // corrompe o que ele esta lendo — sem device removal, so material trocado em alguns
            // raios de um frame, que e a forma mais cara de descobrir isto.
            CommandQueue.Flush();
            ComputeQueue.WaitIdle();
            RaytracingScene.RefreshInstanceGeo(Scene);
            // MeshLights precisa do SetupForScene INTEIRO, nao de um MarkDirty: a lista de
            // tasks (uma por malha emissiva, com o InstanceIndex dentro) e montada so ali, e o
            // MarkDirty apenas re-dispara o extract sobre a lista VELHA. Numa remocao isso
            // extrairia radiancia com o transform da instancia errada; numa copia de malha
            // emissiva, a copia simplesmente nao iluminaria. E ordens de grandeza mais barato
            // que o caminho caro — varre a cena em CPU e realoca buffers pequenos, sem tocar
            // nos ~200 MB de pool de BLAS.
            MeshLights.SetupForScene(Device.Native(), SRVHeap, Scene,
                                     RaytracingScene.InstanceGeoSRV());
            // O SetupForScene acima soltou os buffers do pool/alias e alocou outros. As tabelas de
            // trace do ReSTIR DI carregam uma COPIA daqueles descritores e nao acompanham a troca:
            // sem este refresh o DI le recurso liberado e a tela estoura em branco na primeira vez
            // que ele roda depois de uma duplicacao. Este caminho e o unico que reconstroi o
            // FMeshLights sem passar pelo SetupReflectionsForScene, que e onde as tabelas nascem.
            // As filas ja foram drenadas acima, entao sobrescrever descritor aqui e seguro.
            ReSTIRDI.RefreshMeshLightDescriptors(Device.Native(), SRVHeap,
                                                 MeshLights.LightSRVSlot(),
                                                 MeshLights.AliasSRVSlot());
            // A TLAS nao precisa de pedido explicito: a FScene ja bumpou TransformsVersion na
            // mutacao, e o rebuild por frame do RenderFrame reage a isso sozinho — a lista nova
            // cabe na capacidade, que e o que o NeedsResize acabou de garantir.
        }

        Settings().NotifySceneStructureChanged();
    }

    void Renderer::SetupGIForScene(const Vec3& _AABBMin, const Vec3& _AABBMax) {
        SceneBoundsMin = _AABBMin;
        SceneBoundsMax = _AABBMax;

        // Uma probe selecionada pertence ao volume anterior; nunca deixa o marcador apontar
        // para o mesmo indice numerico de uma grade recem-criada.
        SetDebugProbeIndex(-1);
        PreviousDirectLightPositions.clear();

        if (!Device.RaytracingSupported() || !RaytracingScene.IsBuilt()) return;
        if (!Atmosphere.IsInitialized()) return;

        // Daqui para baixo TODO SetupForScene comeca liberando os recursos e os slots de
        // descritor do volume anterior. O lock do RendererHandle serializa a CPU e nao diz nada
        // sobre a GPU — quando este caminho vem de um clique no editor (RebuildGIVolume, pela
        // contagem de cascatas) o ultimo frame ainda esta EM VOO e continua referenciando o
        // atlas, o ProbesTrace e os slots que estao prestes a ser soltos e reusados.
        //
        // As duas filas, e nesta ordem. O update do DDGI mora na fila de COMPUTE, submetido com
        // um Wait no fence da direta (ver SubmitAfter no RenderFrame): drenar a direta primeiro
        // e o que permite ao trabalho pendente do compute de fato terminar em vez de ficar preso
        // no proprio Wait. Flush sozinho nao alcanca a compute; WaitIdle sozinho esperaria por
        // uma fila que ainda depende da outra.
        //
        // No load a fila ja esta parada e isto custa zero. Fica aqui, e nao no RebuildGIVolume,
        // porque a exposicao e da funcao inteira: TemporalMotion, ReGIR, MeshLights, reflexoes e
        // o passe de debug realocam pelo mesmo motivo e no mesmo instante.
        CommandQueue.Flush();
        ComputeQueue.WaitIdle();

        DDGI.SetupForScene(Device.Native(), CommandQueue, SRVHeap, Scene, _AABBMin, _AABBMax,
                           RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
                           RaytracingScene.InstanceGeoSRV());
        TemporalMotion.SetupForScene(Device.Native(), SRVHeap, Scene,
                                     RaytracingScene.TlasSRVSlot(),
                                     RaytracingScene.InstanceGeoSRV());
        ReGIR.SetupForScene(Device.Native(), SRVHeap, _AABBMin, _AABBMax, GILightSRVSlot);
        // Nao recebe AABB: o hash e de MUNDO e o tamanho da celula vem da distancia a camera, nao
        // da extensao da cena. E justamente o que ele resolve em relacao ao grid do DDGI, cujo
        // espacamento sai de maxExt/23 e vira ~8 m no Bistro.
        RadianceCache.SetupForScene(Device.Native(), SRVHeap);
        // A extracao le VB/IB bindless e o material emissivo pelo InstanceGeo, que agora vem do
        // FRaytracingScene — construido junto da TLAS, antes desta funcao. Faz o levantamento e
        // monta as tasks; a extracao em si so dispara no Record, e so quando sujo.
        MeshLights.SetupForScene(Device.Native(), SRVHeap, Scene,
                                 RaytracingScene.InstanceGeoSRV());

        SetupReflectionsForScene();

        DDGIDebugPass.SetupForScene(Device.Native(), SRVHeap, DDGI.NumProbesCount());
        DDGIDebugPass.SetupPointDiagnosticInputs(
            Device.Native(), SRVHeap, GBuffer.SRVTableStart(), DDGI);

        // DDGI/reflexoes so ganham SRV aqui (por cena), DEPOIS do registro feito em
        // RecreateInternalTargets — sem esta 2a passada eles nunca apareciam na lista.
        // Registrar de novo e barato e idempotente: a chave e o nome, entao sobrescreve.
        RegisterDebugTargets();
    }

    void Renderer::CreateIBLDescriptorTable() {
        IBLTableStart = SRVHeap.Allocate(3);

        D3D12_CPU_DESCRIPTOR_HANDLE DstStart = SRVHeap.CpuHandle(IBLTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Sources[3] = {
            SRVHeap.CpuHandleStaging(HDREnv.IrradianceSRV()),
            SRVHeap.CpuHandleStaging(HDREnv.SpecularSRV()),
            SRVHeap.CpuHandleStaging(HDREnv.BRDFLutSRV()),
        };
        UINT DstCount = 3;
        UINT SrcCounts[3] = { 1, 1, 1 };
        Device.Native()->CopyDescriptors(1, &DstStart, &DstCount,
                                          3, Sources, SrcCounts,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    bool Renderer::LoadHDREnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        CommandQueue.Flush();
        return HDREnv.LoadFromFile(Device.Native(), CommandQueue, SRVHeap, _Path);
    }

    void Renderer::CreateDefaultMaterial() {
        auto* Dev = Device.Native();

        TexDefaultWhite  = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::White);
        TexDefaultNormal = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::FlatNormal);
        TexDefaultORM    = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::ORM);
        TexDefaultBlack  = FTexture::CreateDefault(Dev, UploadQueue, SRVHeap, EDefaultTexture::Black);

        DefaultMaterial.Albedo            = &TexDefaultWhite;
        DefaultMaterial.Normal            = &TexDefaultNormal;
        DefaultMaterial.MetallicRoughness = &TexDefaultORM;
        DefaultMaterial.AO                = &TexDefaultWhite;
        DefaultMaterial.Emissive          = &TexDefaultBlack;
        DefaultMaterial.Height            = &TexDefaultWhite; 
        DefaultMaterial.Metalness         = &TexDefaultWhite; 
        DefaultMaterial.Roughness         = &TexDefaultWhite; 

        DefaultMaterial.Constants.BaseColorFactor  = { 0.8f, 0.8f, 0.8f, 1.0f };
        DefaultMaterial.Constants.MetallicFactor   = 0.0f;
        DefaultMaterial.Constants.RoughnessFactor  = 0.5f;

        DefaultMaterial.Finalize(Dev, SRVHeap);
        ActiveMaterial = &DefaultMaterial;
    }

    void Renderer::SetMaterial(FMaterial* _Material) {
        ActiveMaterial = (_Material && _Material->IsFinalized()) ? _Material : &DefaultMaterial;
    }

    void Renderer::SetUseWater(bool _Use) {
        if (_Use == UseWater) return;
        if (_Use)
            LogInfo("Oceano/agua ativados (FFT 256^2 + superficie)");
        UseWater = _Use;
        // O reset do historico das cascatas mora no FRenderSettings::SetUseWater, pelo dominio
        // OceanTemporal. Ficava aqui como laco escrito a mao — a classe de coisa que o
        // HistoryDomain existe para eliminar —, e por estar fora do grafo ninguem mais ficava
        // sabendo: em particular, uma captura em aquecimento nao era cancelada por ele.
        //
        // Os filtros de TELA continuam de fora, e isso e deliberado: ligar a agua e mudanca de
        // CONTEUDO, e uma superficie grande aparecendo e disoclusao, que e o caso que TAA e FSR
        // resolvem por construcao. Era o reset deles aqui que fazia a tela piscar ao alternar o
        // oceano.
    }

    void Renderer::BuildDefaultScene() {
        FGpuMesh* Sphere = Scene.AddMesh(Device.Native(), FMesh::CreateSphere());

        FRenderable Renderable;
        Renderable.Name     = "Sphere";
        Renderable.Mesh     = Sphere;
        Renderable.Material = nullptr;
        Scene.AddRenderable(Renderable);
    }

    void Renderer::CreateConstantBuffer() {
        static_assert(sizeof(FrameConstants) % 256 == 0,
                      "o CB do frame e indexado por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Frame = GpuResources::CreateUploadBuffer(
            Device.Native(), sizeof(FrameConstants), FCommandQueue::kFramesInFlight);
        ConstantBuffer  = Frame.Resource;
        MappedFrameBase = Frame.Mapped;

        // As duas listas de luz sao lidas como StructuredBuffer (root SRV), nao como CB: o
        // passo e kMaxLights * sizeof(), e arredondar para 256 mudaria o offset por frame.
        const GpuResources::FUploadBuffer Lights = GpuResources::CreateUploadBuffer(
            Device.Native(), static_cast<u64>(kMaxLights) * sizeof(FGPULight),
            FCommandQueue::kFramesInFlight, false);
        LightBuffer     = Lights.Resource;
        MappedLightBase = Lights.Mapped;

        const GpuResources::FUploadBuffer GILights = GpuResources::CreateUploadBuffer(
            Device.Native(), static_cast<u64>(kMaxLights) * sizeof(FGPULightGI),
            FCommandQueue::kFramesInFlight, false);
        GILightBuffer     = GILights.Resource;
        MappedGILightBase = GILights.Mapped;

        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) {
            GILightSRVSlot[i] = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<UINT64>(i) * kMaxLights;
            Srv.Buffer.NumElements         = kMaxLights;
            Srv.Buffer.StructureByteStride = sizeof(FGPULightGI);
            SRVHeap.CreateSRV(Device.Native(), GILightBuffer.Get(), Srv, GILightSRVSlot[i]);
        }

        // Idem para o LightBuffer do deferred (FGPULight, com slice/fade e intencao de sombra). O
        // deferred o le como root SRV; o ReSTIR DI o le pela propria tabela de descritores.
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i) {
            DirectLightSRVSlot[i] = SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<UINT64>(i) * kMaxLights;
            Srv.Buffer.NumElements         = kMaxLights;
            Srv.Buffer.StructureByteStride = sizeof(FGPULight);
            SRVHeap.CreateSRV(Device.Native(), LightBuffer.Get(), Srv, DirectLightSRVSlot[i]);
        }

        RecreateObjectCB();
    }



    void Renderer::SetupReflectionsForScene() {
        if (!Device.RaytracingSupported()) return;
        if (!GBuffer.IsInitialized() || Targets.DepthSRVSlot == kInvalidSlot) return;

        // A direta local usa o snapshot InstanceGeo (do FRaytracingScene), mas nao usa
        // atlas/probes. Se o snapshot ainda nao existe, o passe degenera para not-ready por conta
        // propria; assim uma troca de cena nunca deixa um alvo direto antigo aparentemente valido.
        TemporalMotion.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
                                      Targets.DepthSRVSlot, Targets.VelocitySRVSlot);
        const u32 ReliableVelocitySlot = TemporalMotion.IsReady()
            ? TemporalMotion.MotionSRV() : Targets.VelocitySRVSlot;
        const u32 TemporalTransformSlots[FCommandQueue::kFramesInFlight] = {
            TemporalMotion.TransformSRV(0), TemporalMotion.TransformSRV(1) };
        const u32 TemporalSurfaceSlots[FCommandQueue::kFramesInFlight] = {
            TemporalMotion.SurfaceSRV(0), TemporalMotion.SurfaceSRV(1) };

        ReSTIRDI.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            GBuffer.SRVSlot(0), GBuffer.SRVSlot(1), GBuffer.SRVSlot(2), Targets.DepthSRVSlot,
            ReliableVelocitySlot, RaytracingScene.TlasSRVSlot(), RaytracingScene.InstanceGeoSRV(),
            MeshLights.LightSRVSlot(), MeshLights.AliasSRVSlot(),
            DirectLightSRVSlot, TemporalTransformSlots, TemporalSurfaceSlots);
        // A direta nao depende do volume DDGI. Ela ja vinha antes do early-out que existia aqui;
        // agora ninguem depende, e a ordem so reflete que o pack dela le o TemporalMotion acima.
        SetupNrdDirect();

        // ==== Fallback indireto ============================================================
        // Era aqui que morava `if (!DDGI.IsReady()) return;`. Ele matava reflexoes, ReSTIR GI e o
        // NRD indireto junto com o volume — tres passes cujo caminho PRINCIPAL nao consulta sonda
        // nenhuma; o DDGI e so o terminador do 2o bounce deles. Numa cena sem volume o resultado
        // era tela sem reflexo e sem GI indireta, e nao "GI mais pobre".
        //
        // No lugar dele, cada passe abaixo e guardado pelos PROPRIOS pre-requisitos (a validacao
        // vive dentro de cada SetupForResize) e o volume entra como um contrato que sabe nao
        // existir. Os SetGIParams continuam saindo do FDDGI: sem volume eles descrevem uma grade
        // degenerada que o shader nunca consulta, porque o FallbackAvailable fecha o gate.
        //
        // O contrato decide por EXISTENCIA do volume, nao por UseGI: o que sai daqui fica latched
        // na tabela ate o proximo setup, e o UseGI muda sem provocar setup. Ver
        // GIFallbackBindingsForSetup.
        const FGIFallbackBindings GIFb = GIFallbackBindingsForSetup();

        Reflections.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                                DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        Reflections.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            RaytracingScene.InstanceGeoSRV(), GIFb,
            Targets.DepthSRVSlot, GBuffer.SRVSlot(1), GBuffer.SRVSlot(2), HDREnv.BRDFLutSRV(),
            GBuffer.SRVSlot(0), // GBufferA = BaseColor (tint do metal no reflexo)
            Targets.VelocitySRVSlot,
            Targets.SceneCopyTableStart, Targets.SceneCopyTableStart + 1, Targets.SceneColorMipCount,
            Atmosphere.SkyReflectionSRV(), HDREnv.SpecularSRV(),
            TemporalTransformSlots, TemporalSurfaceSlots);

        ReSTIRGI.SetGIParams(DDGI.GridMin(), DDGI.Spacing(), DDGI.GridCount(),
                             DDGI.TileSizeF(), DDGI.AtlasW(), DDGI.AtlasH(), DDGI.MaxRayDistance());
        ReSTIRGI.SetupForResize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight(),
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            RaytracingScene.InstanceGeoSRV(), GIFb,
            Targets.DepthSRVSlot, GBuffer.SRVSlot(1), ReliableVelocitySlot);

        // Produtor dedicado do radiance cache (Fase 3). Mesmos insumos do ReSTIR GI menos o
        // velocity — ele nao tem historico de tela: o "historico" dele e a propria tabela, que e
        // de mundo. O fallback vai pelo mesmo contrato, mas por um motivo diferente dos outros
        // dois: aqui os tres slots existem so para o gather do HitShading COMPILAR, e nunca sao
        // lidos. E o que torna "o update nunca le DDGI" uma propriedade do codigo, e nao um gate
        // que alguem pode religar por engano.
        RadianceCache.SetupUpdatePass(Device.Native(), SRVHeap,
            RaytracingScene.TlasSRVSlot(), Atmosphere.SkyViewSRV(),
            RaytracingScene.InstanceGeoSRV(), GIFb,
            Targets.DepthSRVSlot, GBuffer.SRVSlot(1), RenderWidth(), RenderHeight());

        // Alvos de timer: cada um no dominio do SEU dispatch — o gather do ReSTIR e full-res e o
        // trace de reflexao e half-res. Sem NVAPI, Initialize() e no-op e os alvos nao existem.
        TimerGI.Initialize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        TimerReflections.Initialize(Device.Native(), SRVHeap,
                                    Reflections.TraceWidth(), Reflections.TraceHeight());

        // Debug da BVH: alvo full-res + a tabela t0/t1 (TLAS + snapshot de instancias). Fica aqui
        // porque este ponto e o unico que roda nos DOIS eventos que invalidam a tabela — resize
        // da tela e reconstrucao da cena de RT (os slots mudam nos dois).
        BvhDebug.Resize(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());
        BvhDebug.SetSceneSRVs(Device.Native(), SRVHeap,
                              RaytracingScene.TlasSRVSlot(), RaytracingScene.InstanceGeoSRV());

        SetupNrdIndirect();

        // Depois do NRD: o SRV do ReSTIR nasce acima e o da OUT do NRD so existe apos o
        // SetupNrdPack. Registrar antes deixava a saida do denoiser fora da lista p/ sempre.
        RegisterDebugTargets();
    }

    bool Renderer::WantNrdIndirect() const {
        // Espelha o NrdIndirectMode do Render(): alocar por um criterio e consumir por outro
        // daria o pior dos dois mundos — memoria reservada sem uso, ou pack apontando p/ recurso
        // liberado.
        //
        // O DDGI SAIU do predicado. Ele estava aqui por acidente de posicao — o SetupNrdIndirect
        // vivia depois do early-out do volume, entao alocar sem volume teria deixado o pack
        // apontando para tabelas do ReSTIR GI que nunca foram montadas. Com o early-out removido,
        // quem produz o sinal indireto e o ReSTIR GI (e o reflexo, no canal especular), e nenhum
        // dos dois depende de sonda para existir. Manter o volume aqui faria o denoiser sumir numa
        // cena sem DDGI e devolver GI crua e ruidosa exatamente onde ela mais precisa de filtro.
        return Denoiser == EDenoiser::NRD && UseReSTIRGI;
    }

    FGIFallbackBindings Renderer::GIFallbackBindingsForSetup() const {
        // EXISTENCIA FISICA, e nao habilitacao. O `UseGI` NAO entra aqui, e a razao e de TEMPO de
        // amostragem, nao de valor:
        //
        //   - esta funcao e lida uma vez, no setup/resize, e o resultado fica LATCHED dentro das
        //     tabelas de descritores de reflexoes e ReSTIR GI;
        //   - o FGIHitSampling::FallbackAvailable e recalculado TODO FRAME.
        //
        // Com `UseGI` nos dois lados, um setup ocorrido com a GI desligada gravava os neutros nas
        // tabelas; religar a GI depois abria o gate do shader — que e por frame — enquanto as
        // tabelas continuavam presas ao atlas 1x1 zerado. O terminador ficava PRETO ate alguem
        // provocar outro setup (trocar de cena, redimensionar), porque o SetUseGI so vira o
        // booleano: nao invalida, nao realoca, nao refaz tabela.
        //
        // `DDGI.IsReady()` pode ficar aqui porque tem a propriedade que falta ao `UseGI`: ele so
        // muda dentro do FDDGI::SetupForScene, e o SetupGIForScene chama o
        // SetupReflectionsForScene logo em seguida. Toda transicao dele ja vem acompanhada da
        // remontagem das tabelas — o latch nunca fica velho.
        //
        // Divisao final: existencia manda no DESCRITOR (aqui), habilitacao manda na LEITURA
        // (FallbackAvailable). Assim desligar a GI fecha o tap sem reconstruir nada, religar
        // reabre no frame seguinte, e os neutros so entram quando nao ha volume de verdade.
        if (DDGI.IsReady()) {
            FGIFallbackBindings B{};
            B.IrradianceAtlasSRV = DDGI.IrradianceAtlasSRV();
            B.DistanceAtlasSRV   = DDGI.DistAtlasSRV();
            B.ProbeDataSRV       = DDGI.ProbeDataSRV();
            B.Available          = true;
            return B;
        }
        return GIFallback.Bindings();
    }

    bool Renderer::WantNrdDirect() const {
        return Denoiser == EDenoiser::NRD && UseReSTIRDI;
    }

    void Renderer::SetupNrdDirect() {
        if (!WantNrdDirect()) { NrdDirect.ReleaseResources(); return; }
        const u32 ReliableVelocitySlot = TemporalMotion.IsReady()
            ? TemporalMotion.MotionSRV() : Targets.VelocitySRVSlot;
        NrdDirect.SetupForResize(Device.Native(), RenderWidth(), RenderHeight());
        if (!NrdDirect.IsReady()) return;
        ReSTIRDI.SetupNrdPack(Device.Native(), SRVHeap,
            GBuffer.SRVSlot(0), GBuffer.SRVSlot(1), GBuffer.SRVSlot(2),
            Targets.DepthSRVSlot, ReliableVelocitySlot,
            NrdDirect.IoResource(FNrdDenoiser::IO_VIEWZ),
            NrdDirect.IoResource(FNrdDenoiser::IO_NORMAL_ROUGHNESS),
            NrdDirect.IoResource(FNrdDenoiser::IO_MV),
            NrdDirect.IoResource(FNrdDenoiser::IO_DIFF_RADIANCE_HITDIST),
            NrdDirect.IoResource(FNrdDenoiser::IO_SPEC_RADIANCE_HITDIST),
            NrdDirect.IoResource(FNrdDenoiser::IO_OUT_DIFF),
            NrdDirect.IoResource(FNrdDenoiser::IO_OUT_SPEC));
    }

    void Renderer::SetupNrdIndirect() {
        if (!WantNrdIndirect()) { Nrd.ReleaseResources(); return; }
        Nrd.SetupForResize(Device.Native(), RenderWidth(), RenderHeight());
        if (!Nrd.IsReady()) return;
        ReSTIRGI.SetupNrdPack(Device.Native(), SRVHeap,
            Nrd.IoResource(FNrdDenoiser::IO_VIEWZ),
            Nrd.IoResource(FNrdDenoiser::IO_NORMAL_ROUGHNESS),
            Nrd.IoResource(FNrdDenoiser::IO_MV),
            Nrd.IoResource(FNrdDenoiser::IO_DIFF_RADIANCE_HITDIST),
            Nrd.IoResource(FNrdDenoiser::IO_OUT_DIFF));
        Reflections.SetupNrdSpec(Device.Native(), SRVHeap,
            Nrd.IoResource(FNrdDenoiser::IO_SPEC_RADIANCE_HITDIST),
            Nrd.IoResource(FNrdDenoiser::IO_OUT_SPEC));
    }

    void Renderer::ReconcileNrdAllocation() {
        if (!Initialized || RenderWidth() == 0 || RenderHeight() == 0) return;
        if (WantNrdIndirect() == Nrd.IsReady() && WantNrdDirect() == NrdDirect.IsReady()) return;
        // Os packs do ReSTIR/Reflections guardam descritores que apontam p/ o IO do NRD. Liberar
        // com frame em voo deixaria a GPU lendo descriptor morto — o mesmo motivo do Flush em
        // toda troca de recurso em runtime (ver ApplyRenderScale e SetCloudsHalfRes).
        CommandQueue.Flush();
        SetupNrdDirect();
        SetupNrdIndirect();
        // A OUT do NRD entra e sai da lista do visualizador junto com a alocacao.
        RegisterDebugTargets();
    }


    void Renderer::CreateDebugPreviewTargets() {
        D3D12_CLEAR_VALUE Clear{};
        Clear.Format   = kDebugPreviewFormat;
        Clear.Color[0] = 0.035f;
        Clear.Color[1] = 0.037f;
        Clear.Color[2] = 0.031f;
        Clear.Color[3] = 1.0f;
        DebugPreviewTarget = GpuResources::CreateTex2D(
            Device.Native(), kDebugPreviewWidth, kDebugPreviewHeight, kDebugPreviewFormat,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            EVramCategory::RenderTargets, &Clear, 1, 1, "Preview de debug");

        DebugPreviewRTVHeap.Initialize(
            Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        Device.Native()->CreateRenderTargetView(
            DebugPreviewTarget.Get(), nullptr, DebugPreviewRTVHeap.CpuHandle(0));

        // Os dois readbacks tem tamanhos muito diferentes (5,76 MB do preview x 1 KB do probe).
        // Antes eles dividiam um Desc, com o Width alternando duas vezes por iteracao — correto,
        // mas so enquanto ninguem mexesse na ordem: tirar a reatribuicao do fim do laco daria a
        // segunda copia do preview um buffer de 1 KB, silenciosamente. Cada criacao carrega o
        // proprio tamanho agora.
        const u64 PreviewBytes = static_cast<u64>(kDebugPreviewRowPitch) * kDebugPreviewHeight;

        for (u32 I = 0; I < FCommandQueue::kFramesInFlight; ++I) {
            DebugPreviewReadback[I] =
                GpuResources::CreateReadbackBuffer(Device.Native(), PreviewBytes);
            DebugPreviewReadbackPending[I] = false;
            DebugPreviewReadbackVersion[I] = 0;

            DebugProbeSampleReadback[I] =
                GpuResources::CreateReadbackBuffer(Device.Native(), kDebugProbeReadbackSize);
            DebugProbeSamplePending[I] = false;
            DebugProbeSampleVersion[I] = 0;
            DebugProbeSampleIndex[I]   = kNoDebugProbe;
        }
    }

    void Renderer::CollectDebugPreviewReadback(u32 _FrameSlot) {
        if (_FrameSlot >= FCommandQueue::kFramesInFlight) return;

        if (DebugPreviewReadbackPending[_FrameSlot]) {
            DebugPreviewReadbackPending[_FrameSlot] = false;
            if (DebugPreviewReadbackVersion[_FrameSlot] == DebugPreviewConfigVersion &&
                DebugPreviewEnabled) {
                void* Mapped = nullptr;
                const size_t TotalBytes =
                    static_cast<size_t>(kDebugPreviewRowPitch) * kDebugPreviewHeight;
                D3D12_RANGE ReadRange{ 0, TotalBytes };
                SMILE_HR(DebugPreviewReadback[_FrameSlot]->Map(0, &ReadRange, &Mapped));

                DebugPreviewPixels.resize(
                    static_cast<size_t>(kDebugPreviewWidth) * kDebugPreviewHeight * 4u);
                const auto* Src = static_cast<const u8*>(Mapped);
                u8* Dst = DebugPreviewPixels.data();
                constexpr size_t TightRow = static_cast<size_t>(kDebugPreviewWidth) * 4u;
                for (u32 Y = 0; Y < kDebugPreviewHeight; ++Y)
                    std::memcpy(Dst + static_cast<size_t>(Y) * TightRow,
                                Src + static_cast<size_t>(Y) * kDebugPreviewRowPitch,
                                TightRow);

                D3D12_RANGE NoWrite{ 0, 0 };
                DebugPreviewReadback[_FrameSlot]->Unmap(0, &NoWrite);
                DebugPreviewPixelsReady = true;
            }
        }

        if (DebugProbeSamplePending[_FrameSlot]) {
            DebugProbeSamplePending[_FrameSlot] = false;
            if (DebugProbeSampleVersion[_FrameSlot] == DebugPreviewConfigVersion &&
                DebugProbeSampleIndex[_FrameSlot] == DebugProbeIndex &&
                DebugPreviewEnabled) {
                void* Mapped = nullptr;
                D3D12_RANGE ReadRange{ 0, static_cast<SIZE_T>(kDebugProbeReadbackSize) };
                SMILE_HR(DebugProbeSampleReadback[_FrameSlot]->Map(
                    0, &ReadRange, &Mapped));
                const auto* Bytes = static_cast<const u8*>(Mapped);
                const auto* Irr = reinterpret_cast<const u16*>(
                    Bytes + kDebugProbeIrrOffset);
                const auto* Dist = reinterpret_cast<const u16*>(
                    Bytes + kDebugProbeDistOffset);

                DebugProbeSampleResult.ProbeIndex = DebugProbeSampleIndex[_FrameSlot];
                for (u32 C = 0; C < 3; ++C)
                    DebugProbeSampleResult.Irradiance[C] =
                        std::pow(std::max(HalfToFloat(Irr[C]), 0.0f), 1.5f);
                const f32 Mean  = HalfToFloat(Dist[0]);
                const f32 Mean2 = HalfToFloat(Dist[1]);
                DebugProbeSampleResult.MeanDistance = Mean;
                DebugProbeSampleResult.DistanceDeviation =
                    std::sqrt(std::abs(Mean2 - Mean * Mean));

                D3D12_RANGE NoWrite{ 0, 0 };
                DebugProbeSampleReadback[_FrameSlot]->Unmap(0, &NoWrite);
                DebugProbeSampleReady = true;
            }
        }
    }

    bool Renderer::ConsumeDebugPreview(std::vector<u8>& _OutPixels) {
        if (!DebugPreviewPixelsReady) return false;
        _OutPixels = DebugPreviewPixels;
        DebugPreviewPixelsReady = false;
        return true;
    }

    bool Renderer::ConsumeDebugProbeSample(FDebugProbeSample& _OutSample) {
        if (!DebugProbeSampleReady) return false;
        _OutSample = DebugProbeSampleResult;
        DebugProbeSampleReady = false;
        return true;
    }

    // === Captura deterministica (Docs/CAPTURE-PROTOCOL.md) ==============================

    FCaptureSettings Renderer::CurrentCaptureSettings() const {
        FCaptureSettings S;
        S.Upscaler        = static_cast<u32>(Upscaler);
        S.Denoiser        = static_cast<u32>(Denoiser);
        S.UpscalerQuality = UpscalerQuality;
        S.UseTAA          = UseTAA;
        S.RenderScale     = RenderScale;
        S.ElapsedTime     = ElapsedTime;
        S.Wetness         = Weather.GetWetness();
        S.TimeOfDayHours  = TimeOfDay.TimeHours;
        S.SunDir[0] = SunDir.X; S.SunDir[1] = SunDir.Y; S.SunDir[2] = SunDir.Z;
        S.CacheAutoWarmup = RadianceCache.GetAutoWarmup();
        return S;
    }

    // Molhadura ASSENTADA para a chuva atual — o valor que o TickWorldClock alcancaria com tempo
    // infinito. Mesma regra de corte do tick (abaixo de 0,001 de chuva a molhadura vai a zero em
    // vez de tender assintoticamente a ela).
    //
    // E este o valor canonico, e nao "o que estava no clique": o tau da molhadura e de 5 a 30
    // segundos e um aquecimento de 128 frames cobre 2,1 s, entao o passo fixo sozinho NAO faz duas
    // capturas convergirem — elas so partiriam do mesmo lugar por coincidencia. Partindo do
    // assentado, a mesma chuva da a mesma molhadura em toda captura, e ela deixa de ser variavel.
    f32 Renderer::SettledWetness() const {
        const f32 Target = Weather.GetRainAmount();
        return Target <= 0.001f ? 0.0f : Target;
    }

    // Aplica um conjunto de knobs pelos SETTERS, nao por atribuicao. E o que garante que o
    // acoplamento upscaler/denoiser/render scale seja o mesmo do caminho normal — o preset nao
    // pode inventar um estado que a UI nao consegue produzir, senao a captura mede um regime que
    // nao existe fora dela.
    void Renderer::ApplyCaptureSettings(const FCaptureSettings& _S) {
        // Denoiser primeiro: DLSS_RR trava o upscaler em DLSS por conta propria (SetDenoiser), e
        // faze-lo depois desfaria o upscaler que acabamos de escolher.
        Settings().SetDenoiser(static_cast<EDenoiser>(_S.Denoiser));
        Settings().SetUpscaler(static_cast<EUpscaler>(_S.Upscaler));
        Settings().SetUpscalerQuality(_S.UpscalerQuality);
        Settings().SetUseTAA(_S.UseTAA);
        // Ignorado sob DLSS_RR (a res de entrada vem do modo de qualidade), e esse e o
        // comportamento correto — ver Renderer::SetRenderScale.
        Settings().SetRenderScale(_S.RenderScale);
    }

    // Devolve o mundo ao operador. O relogio e o sol voltam SEMPRE; os knobs so se o preset os
    // mutou — reaplicar valores iguais pareceria inocente, mas SetUpscaler invalida o filtro de
    // tela incondicionalmente, e o operador veria um frame aliasado no fim de toda captura
    // gameplay.
    void Renderer::RestoreCaptureState(const FCaptureSettings& _S) {
        ElapsedTime      = _S.ElapsedTime;
        TimeOfDay.TimeHours = _S.TimeOfDayHours;
        SetSunDirection(Vec3{ _S.SunDir[0], _S.SunDir[1], _S.SunDir[2] });
        Weather.SetWetness(_S.Wetness);
        // Antes do early return: o aquecimento automatico e desligado nos DOIS presets, entao tem
        // de voltar nos dois. Com a mesma regra de nao pisar no operador que vale para os knobs
        // abaixo — se ele mexeu no toggle durante a sessao, o funil ja cancelou a captura e a
        // escolha nova e dele.
        if (RadianceCache.GetAutoWarmup() == Capture.AppliedByPreset().CacheAutoWarmup)
            Settings().SetRadianceCacheAutoWarmup(_S.CacheAutoWarmup);
        if (!_S.KnobsMutated) return;

        // Knob a knob, e so os que continuam com o valor que o PRESET pos. Um que tenha mudado
        // desde entao foi o operador quem mudou — o funil ja cancelou a captura por causa disso —,
        // e devolver o valor antigo por cima apagaria a escolha dele sem aviso. O caso e concreto:
        // trocar o upscaler no meio de uma captura cientifica cancelava a sessao e, no frame
        // seguinte, o upscaler voltava sozinho para o que era antes.
        const FCaptureSettings& Applied = Capture.AppliedByPreset();
        FCaptureSettings Target = _S;
        if (static_cast<u32>(Upscaler) != Applied.Upscaler) Target.Upscaler = static_cast<u32>(Upscaler);
        if (static_cast<u32>(Denoiser) != Applied.Denoiser) Target.Denoiser = static_cast<u32>(Denoiser);
        if (UpscalerQuality != Applied.UpscalerQuality)     Target.UpscalerQuality = UpscalerQuality;
        if (UseTAA != Applied.UseTAA)                       Target.UseTAA = UseTAA;
        if (RenderScale != Applied.RenderScale)             Target.RenderScale = RenderScale;
        ApplyCaptureSettings(Target);
    }

    void Renderer::UpdateFrameCapture() {
        // Tudo que o capturador faz com o renderer roda sob esta guarda. Preset, reset e
        // restauracao passam pelos setters, que passam pelo funil de invalidacao — e o funil
        // cancela capturas. Sem a guarda, a sessao se cancelaria ao comecar, e a restauracao
        // descartaria um pedido novo enfileirado no mesmo frame.
        struct FGuard {
            bool& Flag;
            ~FGuard() { Flag = false; }
        } Guard{ CaptureSetupGuard };
        CaptureSetupGuard = true;

        // Restauracao ANTES de abrir sessao nova: as duas mexem em upscaler/render scale, e as
        // duas so podem acontecer aqui, fora da gravacao do command list.
        if (Capture.HasPendingRestore()) RestoreCaptureState(Capture.ConsumeRestore());

        if (!Capture.HasPendingRequest()) return;

        // O estado a devolver e guardado SEMPRE, nos dois presets: mesmo o gameplay, que nao toca
        // em knob nenhum, muda o relogio.
        FCaptureSettings Stash = CurrentCaptureSettings();

        // Passo 1 do protocolo. Preset cientifico = resolucao nativa, sem upscaler E SEM TAA:
        // `TAAActive = UseTAA && !UpscaleActive` faz o TAA ACENDER quando o upscaler sai, o que
        // trocaria o jitter do FSR pelo Halton em vez de elimina-lo. Com os dois desligados o
        // JitterPx fica em 0 e Projection == ProjUnjittered, que e o que "sem jitter" significa.
        FCaptureSettings Applied = Stash;
        if (Capture.Pending().Preset == ECapturePreset::Scientific) {
            Stash.KnobsMutated = true;
            FCaptureSettings Sci;
            Sci.Upscaler        = static_cast<u32>(EUpscaler::None);
            Sci.Denoiser        = static_cast<u32>(Denoiser); // eixo do operador, nao do preset
            Sci.UpscalerQuality = 0;
            Sci.UseTAA          = false;
            Sci.RenderScale     = 1.0f;
            // Sair do DLSS_RR e consequencia de tirar o upscaler (o RR faz denoise E upscale num
            // eval so). Fica registrado no manifesto, que grava o estado EFETIVO.
            if (Denoiser == EDenoiser::DLSS_RR) Sci.Denoiser = static_cast<u32>(EDenoiser::NRD);
            ApplyCaptureSettings(Sci);
            // O que ficou DE FATO no renderer, e nao o que o preset pediu: o SetUpscaler pode ter
            // recusado um upscaler indisponivel e o SetRenderScale e ignorado sob DLSS_RR. E
            // contra ISTO que a restauracao compara para saber se o operador mexeu.
            Applied = CurrentCaptureSettings();
            Applied.KnobsMutated = true;
        }

        // NOS DOIS PRESETS, como o relogio: o aquecimento automatico do cache sai da sessao.
        //
        // Ele nao e um knob de qualidade que o gameplay deva preservar — e um evento agendado
        // dentro da janela de aquecimento. Com ele ligado, a borda `Filling -> Active` cai por
        // volta do frame 30 de 128 e derruba o historico dos consumidores no meio da contagem: a
        // captura sairia sub-aquecida com o manifesto afirmando N, que e exatamente o que o funil
        // de invalidacao existe para impedir — e, de fato, o funil cancelaria a sessao. Como o
        // reset deterministico logo abaixo zera o cache, isso aconteceria em TODA captura.
        //
        // Desligado, a sessao inteira roda sob uma regra so: a consulta segue o toggle de leitura
        // do operador do primeiro ao ultimo frame. Nos primeiros ela erra numa tabela fria, que e
        // inofensivo — o piso de confianca ja impede celula fria de encerrar caminho, e ninguem
        // olha um frame de aquecimento. O manifesto grava `cacheAutoWarmup`, entao a captura diz
        // em que regime foi tirada.
        Settings().SetRadianceCacheAutoWarmup(false);
        Applied.CacheAutoWarmup = false;
        Capture.StashSettings(Stash, Applied);

        // Passo 2: FASE TEMPORAL CANONICA. Congelar o relogio onde ele estava fixaria uma fase
        // arbitraria — a de quando o operador clicou —, e nuvem, oceano, vento e ondulacao saem
        // dela. Duas capturas do mesmo bookmark, disparadas com minutos de diferenca, sairiam com
        // nuvens em posicoes diferentes e o A/B mediria isso. Com valor fixo, a fase deixa de ser
        // variavel: toda captura parte do mesmo instante do mundo.
        //
        // Zero e canonico por ser fixo, nao por ser especial. O salto que ele provoca e inerte
        // aqui: o reset logo abaixo derruba todo historico que poderia reprojetar o estado velho.
        ElapsedTime = kCaptureElapsedSeconds;
        Weather.SetWetness(SettledWetness());

        // A HORA e caso a parte: ela nao pode ser canonicalizada como o ElapsedTime, porque e a
        // iluminacao AUTORADA e nao um contador sem significado. Entao ela e declarada por quem
        // dispara — e duas capturas com a mesma hora declarada tem o mesmo sol por construcao,
        // mesmo com o Time-of-Day correndo entre elas.
        const f32 PinHours = Capture.Pending().PinTimeOfDayHours;
        CapturePinApplied  = -1.0f;
        if (PinHours >= 0.0f && TimeOfDay.Enabled) {
            TimeOfDay.TimeHours = std::fmod(PinHours, 24.0f);
            SetSunDirection(TimeOfDay.SunDirection());
            // O EFETIVO, nao o pedido: com o TOD desligado o sol e autorado a mao e fixar a hora
            // nao faz nada, entao o manifesto nao pode registrar um controle que nao houve.
            CapturePinApplied = TimeOfDay.TimeHours;
        }
        // Reafirmados a cada frame da sessao (ver TickWorldClock). Com o TOD desligado o sol e
        // autorado a mao e nao deriva da hora, por isso os dois sao guardados.
        CaptureSunHours = TimeOfDay.TimeHours;
        CaptureSunDir   = SunDir;

        // Passos 3 e 4: reset deterministico + semente zerada. Depois do preset e do relogio,
        // porque realocar alvos invalida historico por conta propria e a ordem inversa deixaria
        // residuo do estado anterior nos acumuladores.
        Capture.BeginSession();
        Settings().NotifyDeterministicCapture();
        LogInfo("Captura: aquecendo " + std::to_string(Capture.Active().WarmupFrames) +
                " frames renderizados");
    }

    FCaptureState Renderer::CollectCaptureState(const FFrameModes& _Modes) const {
        FCaptureState S;
        S.OutputWidth  = OutputWidth();
        S.OutputHeight = OutputHeight();
        S.RenderWidth  = RenderWidth();
        S.RenderHeight = RenderHeight();
        S.RenderScale  = RenderScale;

        switch (Upscaler) {
            case EUpscaler::FSR:  S.Upscaler = "FSR";  break;
            case EUpscaler::DLSS: S.Upscaler = "DLSS"; break;
            default:              S.Upscaler = "None"; break;
        }
        switch (Denoiser) {
            case EDenoiser::NRD:     S.Denoiser = "NRD";     break;
            case EDenoiser::DLSS_RR: S.Denoiser = "DLSS_RR"; break;
            default:                 S.Denoiser = "None";    break;
        }
        S.UpscalerQuality = UpscalerQuality;
        // EFETIVO, nao selecionado: o preset cientifico desliga o upscaler, e sem upscaler o
        // `TAAActive = UseTAA && !UpscaleActive` decide sozinho se o TAA acendeu.
        S.UseTAA = _Modes.TAAActive;

        // Daqui para baixo, tudo vem do FFrameModes deste frame — ou seja, do que RODOU, e nao do
        // que o operador PEDIU. A diferenca nao e teorica: um toggle ligado com o passe fora do
        // IsReady() (volume ausente, setup pendente, denoiser incompativel) faria o manifesto
        // afirmar que o passe participou da imagem. Um manifesto que mente sobre a configuracao e
        // pior que nenhum, porque o A/B seria feito em cima dele.
        S.UseGI       = UseGI;               // dominio indireto: intencao, e nao ha passe unico
        S.DDGIReady   = DDGI.IsReady();      // EXISTENCIA do volume (criterio do fallback)
        S.ReSTIRGI    = _Modes.ReSTIRGIActive;
        S.ReSTIRDI    = _Modes.ReSTIRDIActiveFrame;
        S.ReGIR       = ReGIRRanThisFrame;    // toggle + consumidor + luz na cena
        S.ReGIRRequested     = UseReGIR;      // so o toggle — a diferenca entre os dois e o achado
        S.PunctualLightCount = GILightCountThisFrame;
        S.Reflections = _Modes.ReflectionsActive;
        // O que o ShaderParams REALMENTE publicou aos traces deste frame. Recompor a condicao
        // aqui (Enabled && Ready && !ResetPending) daria outra resposta: o resolve limpa o
        // ResetPending no meio do frame, entao o primeiro frame depois de um reset — que e
        // exatamente a captura com N=0 — se declararia ativo tendo entregado flags zeradas.
        S.CacheUpdate = RadianceCache.PublishedUpdateThisFrame();
        S.CacheQuery  = RadianceCache.PublishedQueryThisFrame();
        // Pelo mesmo criterio: QUEM escreveu e com que fracao EFETIVA. Produtor legado e dedicado
        // sao dois estimadores, e sem isto as duas capturas sairiam com etiqueta e manifesto
        // identicos — o pior caso possivel, porque nada no arquivo denunciaria a diferenca.
        S.CacheDedicatedUpdate = RadianceCache.PublishedDedicatedThisFrame();
        S.CacheUpdateFraction  = RadianceCache.PublishedUpdateFraction();
        S.CacheUsePrevTerminal = RadianceCache.GetUsePrevCacheAtTerminal();
        S.CacheMaxVertices     = RadianceCache.PublishedUpdateVertices();
        S.CacheMinRoughness    = RadianceCache.PublishedMinCacheableRoughness();
        // Piso de confianca EFETIVO — zero quando ninguem consultou o cache neste frame. Ele vale
        // para os dois lados (traces de render e terminal do updater), entao vem publicado por
        // quem quer que tenha recebido a flag de query.
        S.CacheMinSamples      = RadianceCache.PublishedMinSampleCount();
        S.CacheStats  = RadianceCache.PublishedStatsThisFrame();
        S.CacheStatsDetail = RadianceCache.PublishedStatsDetailThisFrame();
        // Aquecimento global. Nao precisa de campo "publicado" como os de cima: o estado so muda no
        // UpdatePerFrame, no comeco do frame, e ler aqui devolve o mesmo valor com que os
        // consumidores foram montados. O que ele acrescenta ao `cacheQuery` e o MOTIVO de a
        // consulta estar fechada — "enchendo" e "resetando" pedem esperas de ordem completamente
        // diferente, e uma captura tirada antes da hora tem de se denunciar.
        S.CacheWarmup     = RadianceCache.WarmupStateName();
        S.CacheAutoWarmup = RadianceCache.GetAutoWarmup();
        S.GIMeasureTerminatorOff = GIMeasureTerminatorOff;
        // Toggle, e nao "efetivo": esta politica e lida por frame pelos DOIS consumidores (o
        // gather do ReSTIR GI e o produtor do cache) da mesma fonte, entao ela descreve o regime
        // do frame inteiro. Mesmo criterio do GIMeasureTerminatorOff acima.
        S.GIBackfacePolicy = ReSTIRGI.GetBackfacePolicy();

        // Ocupacao do frame capturado (copia POS-resolve, feita junto do backbuffer) e hit rate
        // do frame capturado (copia PRE-resolve, do anel). As duas metades ficam prontas em
        // momentos opostos do frame — ver FRadianceCache::RecordStatsCopy.
        //
        // So quando o cache de fato participou: com o passe parado nao ha copia nova, e o readback
        // devolveria um numero de idade desconhecida ao lado de `cacheUpdate: false`. Zero e mais
        // honesto que velho.
        //
        // UPDATE **ou** QUERY: o A/B tem uma configuracao so-leitura legitima — reflexoes
        // consultando com DDGI e ReSTIR GI desligados —, e ali a ocupacao e o hit rate sao
        // justamente o que se quer medir. Gatear so pelo update zerava a linha inteira num caso
        // que o proprio plano prevê.
        if (S.CacheUpdate || S.CacheQuery) {
            S.CacheOccupied  = CaptureCacheStats.Occupied;
            S.CacheValid     = CaptureCacheStats.HasSamples;
            S.CacheConfident = CaptureCacheStats.Confident;
            S.CacheSamples   = CaptureCacheStats.Samples;
            S.CacheEvicted   = CaptureCacheStats.Evicted;
            S.CacheCapacity  = RadianceCache.Capacity();
            const FRadianceCacheStats& Ring = RadianceCache.Stats();
            S.CacheQueries  = Ring.Queries;  // zero sem a instrumentacao ligada
            S.CacheHits     = Ring.Hits;
            // Do mesmo anel, e zerados sem o regime de detalhe: as duas metades da telemetria vem
            // da mesma copia PRE-resolve, porque misses e insercoes sao escritos pelos passes que
            // rodam antes dele.
            S.CacheMissShort   = Ring.MissShort;
            S.CacheMissCone    = Ring.MissCone;
            S.CacheMissNoEntry = Ring.MissNoEntry;
            S.CacheMissEmpty   = Ring.MissEmpty;
            S.CacheMissWarming = Ring.MissWarming;
            S.CacheMissStale   = Ring.MissStale;
            S.CacheInsertTries = Ring.InsertTries;
            S.CacheInsertFull  = Ring.InsertFull;
            S.CacheContended   = Ring.Contended;
            S.CacheRetries     = Ring.Retries;
            S.CacheCapped      = Ring.Capped;
            S.CacheProbeSum    = Ring.ProbeSum;
            S.CacheProbeMax    = Ring.ProbeMax;
            S.CachePaths       = Ring.Paths;
            S.CachePathVerts   = Ring.PathVerts;
            S.CachePathDepth   = Ring.PathDepth;
            S.CacheTermSky     = Ring.TermSky;
            S.CacheTermCache   = Ring.TermCache;
            S.CacheTermKilled  = Ring.TermKilled;
            S.CacheTermMiss    = Ring.TermMiss;
            S.CacheTermNoQuery = Ring.TermNoQuery;
            S.CacheTermLobe    = Ring.TermLobe;
            S.CacheTermOther   = Ring.TermOther;
        }

        // Antes do ++ dos contadores: e o indice com que ESTE frame amostrou.
        S.TemporalSampleIndex = TemporalSampleIndex;
        S.FrameIndex          = FrameIndex;

        const Vec3 Pos = Camera.GetPosition();
        S.CameraPos[0] = Pos.X; S.CameraPos[1] = Pos.Y; S.CameraPos[2] = Pos.Z;
        S.PitchDeg = Camera.GetPitch() * ToDeg;
        S.YawDeg   = Camera.GetYaw()   * ToDeg;
        S.FovYDeg  = kFovYDegrees;

        S.SunDir[0] = SunDir.X; S.SunDir[1] = SunDir.Y; S.SunDir[2] = SunDir.Z;
        S.TimeOfDayHours     = TimeOfDay.TimeHours;
        S.TimeOfDayEnabled   = TimeOfDay.Enabled;
        S.PinnedHoursApplied = CapturePinApplied;
        return S;
    }

    void Renderer::FinishFrameCapture(const FFrameModes& _Modes, u32 _FrameSlot) {
        if (!Capture.AdvanceFrame()) return;
        // A copia ja foi submetida junto do frame; so a captura paga o stall, uma vez, numa
        // operacao que ja e explicitamente offline. Um readback por frame em voo evitaria o
        // stall e custaria estado pendente atravessando frames — troca ruim aqui.
        CommandQueue.Flush();
        // As duas metades da telemetria do cache, agora que a fila esta sincronizada:
        //  - do ANEL (copia pre-resolve deste frame): query/hits, que os traces escreveram e o
        //    resolve zerou logo depois;
        //  - do buffer da CAPTURA (copia pos-resolve): ocupacao, que a varredura acabou de gravar.
        // Uma copia so nao entrega as duas, e a calibracao do N quer as duas.
        RadianceCache.CollectStats(_FrameSlot);
        CaptureCacheStats = {};
        RadianceCache.CollectCaptureStats(CaptureCacheStats);
        Capture.Finish(CollectCaptureState(_Modes));
    }




    // Reload COMPLETO: todo passe registrado recria os proprios pipelines. Era uma lista escrita
    // a mao, irma da tabela do ReloadShaders — e as duas divergiram nos dois sentidos (o
    // MeshLights so estava aqui, o RainWetness so estava la, e a GTAO em nenhuma). Agora as duas
    // leem o MESMO ShaderStems()/OnRecreatePipelines de cada passe, entao nao ha o que divergir.
    void Renderer::RecreateAllPSOs() {
        Passes.RecreateAllPipelines(MakePassInitContext());
    }

    bool Renderer::ReloadShaders(const std::string& _ChangedStem) {
        if (!Initialized) return false;
        try {
            CommandQueue.Flush();

            if (_ChangedStem.empty()) {
                RecreateAllPSOs();
                LogInfo("Shaders recarregados (reload completo)");
                return true;
            }

            const FPassInitContext Ctx = MakePassInitContext();
            const char* Owner = Passes.OwnerOfShaderStem(_ChangedStem);
            const u32   Hits  = Passes.RecreatePipelinesForStem(_ChangedStem, Ctx);
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
        // Recria os alvos e invalida historico, mas nao mexeria no contador de aquecimento: a
        // sessao continuaria contando "N frames apos um reset" com um reset a mais no meio, e a
        // captura sairia sub-aquecida sem nada denunciando. Recusar nao e opcao — a janela tem de
        // poder ser redimensionada.
        Capture.Cancel("a janela foi redimensionada durante o aquecimento");
        CommandQueue.Flush();
        SwapChain.Resize(Device.Native(), _Width, _Height);
        RecreateInternalTargets();
    }

    void Renderer::SetRenderScale(f32 _Scale) {
        // Com o RR a res de entrada e ditada pelo modo de qualidade (ver ApplyUpscalerScale): o RR nao
        // suporta DRS e a feature NGX nasce na res otima do modo, entao aceitar uma escala arbitraria
        // aqui poria o render subrect fora do buffer criado. Quem manda na escala nesse estado e o
        // ApplyUpscalerScale, que entra pelo ApplyRenderScale e nao passa por este gate.
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
        // Segundo caminho que recria os alvos da cena — o outro e o Resize, que ja cancela. Aqui
        // nao ha invalidacao pelo funil a que se pendurar: recriar recurso zera historico por
        // CONSTRUCAO, sem passar por Invalidate, entao a captura seguiria contando "N frames apos
        // um reset" com um reset a mais no meio. Chegam por aqui o slider de render scale e, via
        // ApplyUpscalerScale, a troca de modo de qualidade do upscaler.
        //
        // Sob a guarda e o proprio preset da captura mudando a escala; ai nao ha o que cancelar.
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
        // Instrumentacao do churn de resize: os chamadores (Resize e ApplyRenderScale) ja
        // deram Flush na fila, entao TUDO daqui e destruir e recriar alvo. Medir os dois
        // numeros — parede e tempo dentro do D3D12 — separa "o driver e lento criando" de
        // "temos coisa demais a recriar", que apontam para solucoes opostas (pool/D3D12MA
        // de um lado, alocar no tamanho maximo e usar sub-rect do outro).
        const auto RecreateStart = std::chrono::steady_clock::now();
        GpuResources::ResetCreationStats();

        // Captura os descritores REAIS deste resize para o Tools/AllocBench reproduzi-los.
        //
        // Automatica, mas SO em build de diagnostico. A primeira versao exigia
        // SMILE_CAPTURE_DESCS e nao produziu nada — o editor e lancado por atalho/IDE, entao
        // um `set` num terminal qualquer nao alcanca o processo, e o unico sinal da falha era
        // a AUSENCIA de uma linha no log. Automatizar consertou isso mas criou outro
        // problema: em Release o jogo do usuario final escreveria um arquivo a cada resize.
        // O gate de configuracao resolve os dois. A variavel sobrevive como override do
        // CAMINHO.
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
        Targets.CreateUpscaleMasks(Device.Native(), SRVHeap, RenderWidth(), RenderHeight());

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
        // Sobrescreve a cada resize: o ultimo vence, e e o que o usuario acabou de ver no
        // log. O Dump loga o caminho absoluto; o guard desliga a captura tambem em excecao.
        DescCapture.Complete();
#endif
    }

    // Publica no registro os alvos que ja possuem SRV. Nomes sao a chave (o filtro do
    // visualizador e por substring), entao os prefixos importam: digitar "gbuffer" traz o
    // G-buffer inteiro, "reflex" traz os estagios de reflexao.
    void Renderer::RegisterDebugTargets() {
        using namespace DebugTargets;
        constexpr u32 kNoSlot = 0xFFFFFFFFu;

        // O indice e detalhe de apresentacao; preserva a selecao pelo nome enquanto a lista
        // e reconstruida. Isso tambem remove entradas DDGI/ReSTIR obsoletas quando uma nova
        // cena nao consegue recriar seus recursos, em vez de deixar um SRV ja liberado visivel.
        std::vector<std::string> SelectedNames;
        std::string MainTargetName;
        {
            const auto& Previous = All();
            SelectedNames.reserve(DebugSelection.size());
            for (u32 Index : DebugSelection)
                if (Index < Previous.size()) SelectedNames.push_back(Previous[Index].Name);
            if (DebugTargetIndex < Previous.size())
                MainTargetName = Previous[DebugTargetIndex].Name;
        }
        Clear();

        // --- Cena / G-buffer -------------------------------------------------------------
        // Os 8 modos antigos viram entradas com decode GBufferField (SubIndex == modo antigo).
        // O SrvSlot aqui e so p/ o registro ter algo valido; o decode le os 3 MRTs da tabela.
        if (GBuffer.IsInitialized()) {
            const u32 GB = GBuffer.SRVTableStart();
            Register("GBuffer · base color",    GB, EDebugDecode::GBufferField, 1);
            Register("GBuffer · normal",        GB, EDebugDecode::GBufferField, 2);
            Register("GBuffer · roughness",     GB, EDebugDecode::GBufferField, 3);
            Register("GBuffer · metallic",      GB, EDebugDecode::GBufferField, 4);
            Register("GBuffer · subsurface",    GB, EDebugDecode::GBufferField, 5);
            Register("GBuffer · AO",            GB, EDebugDecode::GBufferField, 6);
            Register("GBuffer · shading model", GB, EDebugDecode::GBufferField, 7);
        }
        if (Targets.VelocitySRVSlot != kNoSlot)
            Register("Motion vectors", Targets.VelocitySRVSlot, EDebugDecode::Velocity);
        if (Targets.DepthSRVSlot != kNoSlot)
            Register("Depth (reverse-Z)", Targets.DepthSRVSlot, EDebugDecode::ReverseZ);
        // "HDR color" NAO entra: o Targets.HDRColorBuffer e o proprio render target deste passe
        // (RTV em Targets.HDRRTVHeap.CpuHandle(0)), entao le-lo como SRV no mesmo draw seria
        // ler e escrever o mesmo recurso. Precisaria de copia; fica p/ quando houver captura.

        // --- Sombras ---------------------------------------------------------------------
        // Uma entrada por cascata sobre o MESMO SRV (o array inteiro), com a fatia no
        // SubIndex — e o unico jeito de enderecar slice, ja que nao existe SRV de dimensao
        // TEXTURE2D que selecione um. Com as quatro selecionadas de uma vez a grade mostra o
        // conjunto lado a lado, que e como se ve o encaixe entre cascatas e o efeito do cache.
        //
        // Exposure 1.0 mostra a profundidade CRUA do ortho. Ela e linear em mundo ao longo da
        // luz (projecao ortografica), entao a rampa ja e legivel sem linearizar: limpo = 1.0
        // (far, sem caster), escuro = caster perto do sol.
        if (SunShadows.IsInitialized() && SunShadows.ShadowSRVSlot() != kNoSlot) {
            static constexpr const char* kCascadeNames[FSunShadows::kNumCascades] = {
                "Sombra do sol · cascata 0", "Sombra do sol · cascata 1",
                "Sombra do sol · cascata 2", "Sombra do sol · cascata 3" };
            for (u32 c = 0; c < FSunShadows::kNumCascades; ++c)
                Register(kCascadeNames[c], SunShadows.ShadowSRVSlot(),
                         EDebugDecode::ArraySlice, /*SubIndex=*/c);
        }

        // --- Iluminacao indireta ---------------------------------------------------------
        if (AO.AOSRVSlot() != kNoSlot)
            Register("GTAO", AO.AOSRVSlot(), EDebugDecode::Grayscale);
        // Entrada do GTAO, nao a normal do G-buffer: sai do Z-prepass (DepthNormal.ps) com a
        // normal INTERPOLADA do vertice, sem normal map — oclusao e sobre a forma da geometria.
        // Fica no grupo do GTAO porque nasceu como entrada dele. Hoje tambem e escrita quando o
        // visualizador do radiance cache esta selecionado (ver GeometricNormalWillRun no Render).
        // Fora desses dois casos o alvo congela no ultimo frame valido ou no clear neutro cinza.
        if (Targets.NormalSRVSlot != kNoSlot)
            Register("GTAO · normal geometrica", Targets.NormalSRVSlot, EDebugDecode::Raw);
        // As duas pontas do ReSTIR entram como alvos SEPARADOS, cada uma com slot fixo. Seguir
        // o slot vigente (que alterna com o toggle do NRD) daria uma entrada que troca de
        // textura por baixo — e o util p/ debugar o ReSTIR e justamente o sinal CRU, onde o
        // ruido e a convergencia aparecem. Com o RELAX a OUT ja sai em radiancia linear, entao
        // as duas usam o mesmo decode HDR (o REBLUR exigia um decode YCoCg proprio).
        if (ReSTIRGI.GITexRawSRVSlot() != kNoSlot)
            Register("ReSTIR GI", ReSTIRGI.GITexRawSRVSlot(), EDebugDecode::HDR, 0, 1,
                     /*Exposure=*/1.5f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (ReSTIRGI.NrdOutSRVSlot() != kNoSlot)
            Register("ReSTIR GI · NRD", ReSTIRGI.NrdOutSRVSlot(), EDebugDecode::HDR, 0, 1,
                     /*Exposure=*/1.5f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (ReSTIRDI.OutputSRVSlot() != kNoSlot)
            Register("ReSTIR DI", ReSTIRDI.OutputSRVSlot(), EDebugDecode::HDR, 0, 1,
                     /*Exposure=*/1.0f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        // O especular resolvido faltava na lista, e e o unico sinal grande que NAO dava p/
        // inspecionar — o que atrapalhou o diagnostico do campo de firefly de 2026-08-07. E o
        // alvo que o composite le direto quando RawSpec esta ligado (modo Ray Reconstruction),
        // isto e, exatamente o que chega ao RR sem passar por denoiser nenhum. Exposure 1.0 e
        // filtro OFF pelo mesmo motivo do ReSTIR: aqui interessa ver o outlier por pixel.
        if (Reflections.GetResolvedSRVSlot() != kNoSlot)
            Register("Reflexos · resolvido", Reflections.GetResolvedSRVSlot(), EDebugDecode::HDR,
                     0, 1, /*Exposure=*/1.0f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        // Guides do DLSS Ray Reconstruction. Sao os buffers que a NGX consome (DlssRRPass.cpp taga
        // os recursos direto), e ate 2026-08-07 eram os UNICOS sinais grandes sem visualizador —
        // o que deixou uma investigacao de firefly cega justo no lado suspeito. Filtro linear OFF
        // nos quatro: aqui interessa o valor exato do texel, nao a aparencia.
        // ATENCAO ao ler: com um visualizador ativo o eval do RR e PULADO (ver RRPoisoned), entao
        // o que aparece aqui e o guide do frame ANTERIOR — que e exatamente o que se quer, mas
        // significa que nenhum alvo de debug pode mostrar um artefato criado PELO RR.
        if (RRGuides.DiffuseAlbedoSRV() != kNoSlot) {
            Register("DLSS-RR · albedo difuso", RRGuides.DiffuseAlbedoSRV(), EDebugDecode::Raw,
                     0, 1, 1.0f, 0, /*LinearFilter=*/false);
            Register("DLSS-RR · albedo especular", RRGuides.SpecularAlbedoSRV(), EDebugDecode::Raw,
                     0, 1, 1.0f, 0, /*LinearFilter=*/false);
            // Raw, NAO OctNormal: o guide grava `float4(g.WorldNormal, rough)` cru
            // (DlssRRGuides.cs.hlsl:102), nao octaedrico — o decode oct leria rg como oct e
            // inventaria uma normal. Em Raw o hemisferio negativo corta em preto, o que basta
            // p/ o que se procura aqui: normal zerada, NaN ou descontinuidade. A roughness vive
            // no .a (o visualizador nao mostra alpha); ela sai do mesmo G-buffer, entao use
            // "GBuffer · roughness" p/ inspecionar o valor.
            Register("DLSS-RR · normal+rough", RRGuides.NormalRoughnessSRV(), EDebugDecode::Raw,
                     0, 1, 1.0f, 0, /*LinearFilter=*/false);
            // HEATMAP, nao Grayscale. A primeira tentativa foi Grayscale com escala 1/50 e a tela
            // saiu "toda preta" — mas numa rua a reflexao acerta parede a poucos metros, e 2 m
            // viravam 0,04 de cinza, indistinguivel de zero num print. O heatmap resolve
            // exatamente esta pergunta: ele pinta o zero EXATO de preto e qualquer valor pequeno
            // de azul (ver o bloco DECODE_HEATMAP no DebugView.ps), separando "sem hit" de "hit
            // perto". Exposure = 1/"valor quente" -> 20 m satura o topo da rampa.
            Register("DLSS-RR · spec hitDist", RRGuides.SpecHitDistSRV(), EDebugDecode::Heatmap,
                     0, 1, /*Exposure=*/1.0f / 20.0f, 0, /*LinearFilter=*/false);
        }
        // Instrumentacao de timer (NVAPI). A Exposure e o 1/valor "quente" do artigo da NVIDIA:
        // escala FIXA de proposito — normalizar pelo maximo do frame faria a mesma cena mudar de
        // cor conforme a camera anda, e duas capturas deixariam de ser comparaveis.
        // Filtro linear OFF: cada texel e uma medida por thread, interpolar inventa custo.
        if (TimerGI.SrvSlot() != kNoSlot)
            Register("RT · timer · ReSTIR GI", TimerGI.SrvSlot(), EDebugDecode::Heatmap, 0, 1,
                     /*Exposure=*/kShaderTimerScaleGI, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (TimerReflections.SrvSlot() != kNoSlot)
            Register("RT · timer · reflexos", TimerReflections.SrvSlot(), EDebugDecode::Heatmap, 0, 1,
                     /*Exposure=*/kShaderTimerScaleReflections, /*AtlasTilePx=*/0,
                     /*LinearFilter=*/false);
        // Debug da BVH. Decode Raw porque o passe ja resolve a cor final (paleta de categoria,
        // rampa de falsa-cor) — nao ha canal cru p/ o visualizador interpretar, e um decode
        // generico so poderia estragar cor autorada. Filtro linear OFF: cada texel e um raio, e
        // interpolar borraria a fronteira entre duas instancias.
        if (BvhDebug.SrvSlot() != kNoSlot)
            Register("RT · BVH", BvhDebug.SrvSlot(), EDebugDecode::Raw, 0, 1,
                     /*Exposure=*/1.0f, /*AtlasTilePx=*/0, /*LinearFilter=*/false);
        if (DDGI.IrradianceAtlasSRV() != kNoSlot) {
            // O atlas guarda irradiancia comprimida por gamma; o decode generico de HDR
            // deixava o sinal artificialmente claro e nao correspondia ao que o lighting usa.
            Register("DDGI · irradiancia", DDGI.IrradianceAtlasSRV(),
                     EDebugDecode::DDGIIrradiance, 0, 1,
                     /*Exposure=*/0.4f, /*AtlasTilePx=*/6 + 2);
            // R e a media em unidades de mundo, ja limitada no update a 2,6 * spacing.
            // MaxRayDistance e o alcance do trace da cena e pode ser dezenas de vezes maior.
            const f32 DistMax  = DDGI.DistanceMomentMax();
            const f32 DistNorm = DistMax > 0.0f ? 1.0f / DistMax : 1.0f;
            Register("DDGI · distancia",   DDGI.DistAtlasSRV(),       EDebugDecode::DDGIDistance, 0, 1,
                     /*Exposure=*/DistNorm, /*AtlasTilePx=*/14 + 2);
        }

        // "Upscaler · saida" NAO entra: o upscaler roda DEPOIS deste passe no frame, entao o
        // que se leria aqui e o resultado do frame ANTERIOR — dado enganoso. Mesmo caso do
        // "HDR color": so volta com um passe de captura por copia (e o que a Unreal faz).

        // --- Atmosfera / volumetrico -----------------------------------------------------
        // As tres LUTs 2D do Hillaire. A transmitancia e [0..1] por construcao (fracao de luz
        // que sobrevive ao caminho), entao entra CRUA: passar tonemap nela mentiria sobre o
        // valor. Multiscatter e sky-view sao radiancia — HDR, com exposicao propria porque o
        // sky-view vive na escala de kSunIlluminance (22) e estoura branco em 1.0.
        // O volume 3D do aerial perspective e o cubo de reflexo ficam de fora: precisam de
        // decode novo (fatia de volume / cruz de cubo) no DebugView.ps, que e outra rodada.
        if (Atmosphere.IsInitialized()) {
            Register("Atmosfera · transmitancia", Atmosphere.TransmittanceSRV(), EDebugDecode::Raw);
            Register("Atmosfera · multiscatter",  Atmosphere.MultiScatterSRV(),  EDebugDecode::HDR,
                     0, 1, /*Exposure=*/1.0f);
            Register("Atmosfera · sky-view",      Atmosphere.SkyViewSRV(),       EDebugDecode::HDR,
                     0, 1, /*Exposure=*/0.15f);
        }
        if (SunShafts.IsInitialized())
            Register("Sun shafts", SunShafts.VolumetricSRVSlot(), EDebugDecode::HDR, 0, 1, /*Exposure=*/2.0f);

        // --- passes que publicam os proprios alvos ---------------------------------------
        // As entradas acima sao a dedo porque leem estado do Renderer (Targets, GBuffer). Um
        // passe migrado para o contrato publica sozinho, e AQUI: depois do Clear, senao a
        // entrada nasce apagada; antes do remapeamento, senao a selecao nao o encontra.
        Passes.RegisterDebugTargetsAll();

        std::vector<u32> RemappedSelection;
        RemappedSelection.reserve(SelectedNames.size());
        for (const std::string& Name : SelectedNames) {
            const u32 Index = IndexOf(Name);
            if (Index != DebugTargets::kInvalid) RemappedSelection.push_back(Index);
        }
        SetDebugSelection(RemappedSelection);
        DebugTargetIndex = MainTargetName.empty() ? kNoDebugTarget : IndexOf(MainTargetName);
    }

    void Renderer::SetDebugProbeIndex(i32 _Index) {
        const u32 NewIndex = (_Index >= 0 && DDGI.IsReady() &&
                              static_cast<u32>(_Index) < DDGI.NumProbesCount())
                           ? static_cast<u32>(_Index) : kNoDebugProbe;
        if (DebugProbeIndex == NewIndex) return;

        DebugProbeIndex = NewIndex;
        DebugProbeSampleU = -1.0f;
        DebugProbeSampleV = -1.0f;
        DebugProbeSampleReady = false;
        const bool Active = DebugProbeIndex != kNoDebugProbe;
        DDGIDebugPass.SetSelectedProbe(Active ? static_cast<i32>(DebugProbeIndex) : -1);
        DDGIDebugPass.SetEnabled(Active);
        if (Active) {
            DDGIDebugPass.SetMode(FDDGIDebug::EMode::Selected);
            DDGIDebugPass.SetProbeRadius(0.14f);
            DDGIDebugPass.SetShowVolume(false);
            DDGIDebugPass.SetShowRays(false);
        }
        ++DebugPreviewConfigVersion;
    }

    void Renderer::SetDebugProbeSampleUV(f32 _U, f32 _V) {
        const bool Valid = DebugProbeIndex != kNoDebugProbe &&
                           _U >= 0.0f && _U <= 1.0f &&
                           _V >= 0.0f && _V <= 1.0f;
        const f32 U = Valid ? std::clamp(_U, 0.0f, 0.999999f) : -1.0f;
        const f32 V = Valid ? std::clamp(_V, 0.0f, 0.999999f) : -1.0f;
        if (DebugProbeSampleU == U && DebugProbeSampleV == V) return;
        DebugProbeSampleU = U;
        DebugProbeSampleV = V;
        DebugProbeSampleReady = false;
        ++DebugPreviewConfigVersion;
    }

    bool Renderer::RequestDebugProbePoint(u32 _X, u32 _Y) {
        if (DebugProbeIndex == kNoDebugProbe || !DDGI.IsReady()) return false;
        const u32 InternalX = static_cast<u32>(
            static_cast<f32>(_X) * RenderScale + 0.5f);
        const u32 InternalY = static_cast<u32>(
            static_cast<f32>(_Y) * RenderScale + 0.5f);
        return DDGIDebugPass.RequestPointDiagnostic(InternalX, InternalY);
    }

    void Renderer::CancelDebugProbePoint() {
        DDGIDebugPass.CancelPointDiagnostic();
    }

    void Renderer::RepeatDebugProbePoint() {
        if (DebugProbeIndex == kNoDebugProbe || !DDGI.IsReady()) return;
        DDGIDebugPass.RepeatPointDiagnostic();
    }

    bool Renderer::ConsumeDebugProbePoint(
            FDDGIPointDiagnostic& _OutDiagnostic) {
        return DDGIDebugPass.ConsumePointDiagnostic(_OutDiagnostic);
    }

    void Renderer::SetDebugProbeContributors(
            const FDDGIPointDiagnostic& _Diagnostic) {
        u32 Indices[FDDGIDebug::kPointProbeCount]{};
        f32 Weights[FDDGIDebug::kPointProbeCount]{};
        u32 Count = 0;
        i32 RiskSlot = -1;
        for (u32 I = 0; I < FDDGIDebug::kPointProbeCount; ++I) {
            const FDDGIPointProbeDiagnostic& P = _Diagnostic.Probes[I];
            if (!P.Active || P.ProbeIndex >= DDGI.NumProbesCount()) continue;
            if (static_cast<i32>(I) == _Diagnostic.RiskSlot)
                RiskSlot = static_cast<i32>(Count);
            Indices[Count] = P.ProbeIndex;
            Weights[Count] = P.NormalizedWeight;
            ++Count;
        }
        SetDebugProbeContributors(Indices, Weights, Count, RiskSlot);
    }

    void Renderer::SetDebugProbeContributors(
            const u32* _Indices, const f32* _Weights,
            u32 _Count, i32 _RiskSlot) {
        if (_Count > 0 && _Indices && _Weights) {
            DDGIDebugPass.SetSelectedProbes(
                _Indices, _Weights, _Count, _RiskSlot);
            DDGIDebugPass.SetEnabled(true);
            DDGIDebugPass.SetMode(FDDGIDebug::EMode::Selected);
        } else if (DebugProbeIndex != kNoDebugProbe) {
            DDGIDebugPass.SetSelectedProbe(
                static_cast<i32>(DebugProbeIndex));
        }
    }

    // Descarta a lista de contribuintes de um point-pick e volta a destacar SO a probe da
    // sessao, sem encerra-la. Serve ao caso "ponto fora do volume", em que nenhuma das oito
    // contribui: o chamador restaura antes a probe-base (senao o indice da sessao ainda e a
    // dominante do pick anterior, e destacar essa seria apontar quem nao iluminou o ponto).
    //
    // Por que nao dava para reaproveitar o que ja existia: SetDebugProbeContributors com
    // contagem zero NAO limpa — ele cai no ramo de restauracao acima, sem zerar a lista; e
    // SetDebugProbeIndex(-1) faria o RequestDebugProbePoint recusar o proximo clique, matando a
    // inspecao no meio. Este caminho e incondicional, entao tambem funciona quando o indice da
    // sessao nao mudou (o setter faz early-out nesse caso e a lista velha sobreviveria).
    void Renderer::ClearDebugProbeContributors() {
        DDGIDebugPass.SetSelectedProbe(DebugProbeIndex != kNoDebugProbe
                                       ? static_cast<i32>(DebugProbeIndex) : -1);
        ++DebugPreviewConfigVersion; // o painel refaz o preview do tile
    }

    void Renderer::UpdateCamera(const CameraInput& _Input, f32 _DeltaTime) {
        // CAPTURA: pose e relogio do mundo ficam ONDE ESTAO, e este e o unico ponto por onde o
        // tempo de parede entra no renderer — por isso a trava mora aqui e nao espalhada pelos
        // consumidores.
        //
        // Sem isso o aquecimento nao era reproduzivel. O reset deterministico fixa o estado
        // INICIAL, mas centenas de frames depois a captura sai de um estado que o reset nao
        // controla: o operador pode ter encostado no mouse (e a mesma pose por trajetorias
        // diferentes aquece o cache de mundo com celulas diferentes), e nuvem, agua, vento e
        // ondulacao continuam animando com o ElapsedTime. Duas capturas do mesmo bookmark
        // divergiriam por causa de quantos segundos de relogio couberam entre elas.
        //
        // DOIS relogios, e a distincao e o que faz a captura ser reproduzivel:
        //
        //  - FASE DE ANIMACAO (ElapsedTime) fica PARADA. Nuvem, agua, vento e ondulacao sao
        //    funcao dela; deixa-la correr faria a imagem depender de quantos segundos couberam
        //    entre o clique e o disparo.
        //  - PROCESSO QUE ASSENTA (molhadura da chuva, fade das sombras spot) continua, com um
        //    delta FIXO. Congelar aqui seria tao arbitrario quanto animar: a molhadura pararia
        //    onde o exp() estivesse no instante do clique, e duas capturas da mesma cena
        //    divergiriam por causa disso. Com passo fixo elas convergem para o MESMO valor.
        //    O delta fixo tambem e o que o upscaler recebe (UpParams.DeltaTimeSec) — zero ali
        //    seria um insumo que nenhum frame normal produz.
        //
        // O Time-of-Day cai no primeiro grupo apesar de andar por delta, e por isso e o unico que
        // precisa de gate proprio no TickWorldClock: mover o sol durante o aquecimento e
        // exatamente o que duas capturas do mesmo bookmark nao podem ter de diferente.
        //
        // Nota do que isso NAO cobre: sequencia com sol em movimento ou objeto animado e outro
        // tipo de teste (Fase 9 do plano), nao esta captura estatica.
        if (Capture.Busy()) {
            LastDeltaTime = kCaptureDeltaSeconds;
            return;
        }
        Camera.Update(_Input, _DeltaTime);
        ElapsedTime  += _DeltaTime;
        LastDeltaTime = _DeltaTime;
    }

    void Renderer::SetSunDirection(const Vec3& _Direction) {
        SunDir = _Direction.NormalizedSafe(DefaultSunDirection());
    }

    void Renderer::SetSunAzimuthElevation(f32 _AzimuthDeg, f32 _ElevationDeg) {
        const f32 Az = _AzimuthDeg   * ToRad;
        const f32 El = _ElevationDeg * ToRad;
        const f32 CosEl = std::cos(El);
        SetSunDirection(Vec3{ CosEl * std::sin(Az), std::sin(El), CosEl * std::cos(Az) });
    }

    void Renderer::LoadMoonTexture(const std::wstring& _Path) {
        if (!Initialized || !Atmosphere.IsInitialized()) return;
        Atmosphere.LoadMoonTexture(Device.Native(), UploadQueue, SRVHeap, _Path);
    }

    FTexture* Renderer::ImportRuntimeTexture(const std::wstring& _Path, bool _IsNormalMap,
                                            bool _sRGB) {
        if (!Initialized) return nullptr;

        auto EndsWith = [&](const wchar_t* Ext) {
            const size_t N = wcslen(Ext);
            if (_Path.size() < N) return false;
            return _wcsnicmp(_Path.c_str() + _Path.size() - N, Ext, N) == 0;
        };

        FTexture Tex = EndsWith(L".dds")
            ? FTexture::LoadDDS(Device.Native(), UploadQueue, SRVHeap, _Path, _sRGB)
            : FTexture::CreateFromCPU(Device.Native(), UploadQueue, SRVHeap,
                                      FTexture::LoadCPU(_Path, _IsNormalMap, _sRGB));
        if (!Tex.IsValid()) return nullptr;

        ImportedTextures.push_back(std::make_unique<FTexture>(std::move(Tex)));
        return ImportedTextures.back().get();
    }

    FMaterialPreview::ESubmitResult Renderer::SubmitMaterialPreview(
        FMaterial* _Material, const FMaterialPreview::FParams& _Params, u64 _RequestId) {
        if (!Initialized || !_Material) return FMaterialPreview::ESubmitResult::Failed;

        const FGpuMesh* SceneMesh = nullptr;
        Mat44 SceneModel = Mat44::Identity();
        if (_Params.Primitive == FMaterialPreview::PrimSceneMesh) {
            const auto& Rnds = Scene.Renderables();
            const FRenderable* Pick = nullptr;
            const int Sel = GetSelectedObject();
            if (Sel >= 0 && Sel < (int)Rnds.size() &&
                Rnds[Sel].Material == _Material && !Rnds[Sel].RaytracingOnly)
                Pick = &Rnds[Sel];
            if (!Pick) {
                for (const auto& R : Rnds) {
                    if (R.Material != _Material || R.RaytracingOnly || !R.Mesh) continue;
                    Pick = &R;
                    break;
                }
            }
            if (Pick && Pick->Mesh && Pick->Mesh->IsValid()) {
                SceneMesh = Pick->Mesh;
                const Vec3 Center = (Pick->AABBMin + Pick->AABBMax) * 0.5f;
                const Vec3 Ext    = (Pick->AABBMax - Pick->AABBMin) * 0.5f;
                f32 Radius = std::sqrt(Ext.X * Ext.X + Ext.Y * Ext.Y + Ext.Z * Ext.Z);
                if (Radius < 1e-3f || Radius > 1e8f) Radius = 0.5f; // AABB ausente/degenerado
                const f32 S = 0.5f / Radius;
                SceneModel = Pick->Transform.Matrix()
                           * Mat44::Translation(-Center)
                           * Mat44::Scale({ S, S, S });
            }
        }

        return MaterialPreview.Submit(Device.Native(), CommandQueue, SRVHeap,
                                      *_Material, _Params, _RequestId, SceneMesh, SceneModel);
    }

    bool Renderer::LoadMaterialPreviewEnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        // O HDRI e seus descritores sao compartilhados por jobs de preview. Drenar aqui e raro
        // (browse explicito) e impede sobrescrever a tabela enquanto uma list assincrona a usa.
        CommandQueue.Flush();
        return MaterialPreview.LoadEnvironment(Device.Native(), CommandQueue, SRVHeap, _Path);
    }

    void Renderer::LoadStarCatalog(const std::wstring& _Path) {
        if (!Initialized || !Atmosphere.IsInitialized()) return;
        Atmosphere.LoadStarCatalog(Device.Native(), SRVHeap, _Path);
    }

    bool Renderer::WorldToScreen(const Vec3& _W, f32& _Sx, f32& _Sy) const {
        const Mat44& M = LastViewProj;
        const f32 cx = _W.X*M.M[0][0] + _W.Y*M.M[1][0] + _W.Z*M.M[2][0] + M.M[3][0];
        const f32 cy = _W.X*M.M[0][1] + _W.Y*M.M[1][1] + _W.Z*M.M[2][1] + M.M[3][1];
        const f32 cw = _W.X*M.M[0][3] + _W.Y*M.M[1][3] + _W.Z*M.M[2][3] + M.M[3][3];
        if (cw <= 1e-5f) return false;
        const f32 ndcx = cx / cw, ndcy = cy / cw;
        _Sx = (ndcx * 0.5f + 0.5f) * static_cast<f32>(SwapChain.GetWidth());
        _Sy = (0.5f - ndcy * 0.5f) * static_cast<f32>(SwapChain.GetHeight());
        return true;
    }

    bool Renderer::ScreenToRay(u32 _X, u32 _Y, Vec3& _O, Vec3& _D) const {
        const u32 Wd = SwapChain.GetWidth(), Ht = SwapChain.GetHeight();
        if (Wd == 0 || Ht == 0) return false;
        const f32 ndcx = ((static_cast<f32>(_X) + 0.5f) / Wd) * 2.0f - 1.0f;
        const f32 ndcy = 1.0f - ((static_cast<f32>(_Y) + 0.5f) / Ht) * 2.0f;
        const Mat44 Inv = LastViewProj.Inverse();
        const f32 v[4] = { ndcx, ndcy, 0.5f, 1.0f };
        f32 w[4];
        for (int j = 0; j < 4; ++j)
            w[j] = v[0]*Inv.M[0][j] + v[1]*Inv.M[1][j] + v[2]*Inv.M[2][j] + v[3]*Inv.M[3][j];
        if (std::fabs(w[3]) < 1e-9f) return false;
        const Vec3 WorldPt{ w[0]/w[3], w[1]/w[3], w[2]/w[3] };
        _O = Camera.GetPosition();
        _D = (WorldPt - _O).NormalizedSafe(Vec3::UnitZ());
        return true;
    }

    // Resolve num lugar so "que passes rodam neste frame". Todos os insumos sao membros que
    // nao mudam durante a gravacao (verificado: nenhum deles e reescrito dentro do RenderFrame),
    // entao resolver de antemao e equivalente a resolver espalhado — e passavel a uma fase.
    FFrameModes Renderer::ResolveFrameModes() {
        FFrameModes M;
        M.UpscaleActive = (ActiveUpscaler() != nullptr);
        M.TAAActive     = UseTAA && !M.UpscaleActive && TemporalAA.IsInitialized();
        // DLSS Ray Reconstruction: precisa do RR inicializado e dos guides prontos; o eval
        // acontece no bloco de upscale (ActiveUpscaler() == &DlssRR).
        M.RRMode = (Denoiser == EDenoiser::DLSS_RR) && DlssRR.IsInitialized() && RRGuides.IsReady();

        M.ReflectionsActive   = UseReflections && Reflections.IsReady();
        M.ReSTIRGIActive      = UseReSTIRGI && ReSTIRGI.IsReady();
        M.ReSTIRDIActiveFrame = UseReSTIRDI && ReSTIRDI.IsReady();
        M.NrdIndirectMode     = M.ReSTIRGIActive && Nrd.IsReady() &&
                                Denoiser == EDenoiser::NRD;
        M.NrdDirectMode       = M.ReSTIRDIActiveFrame && ReSTIRDI.IsNrdReady() &&
                                NrdDirect.IsReady() && Denoiser == EDenoiser::NRD;
        M.AOWillRun           = UseAO && AO.IsReady();
        // O produtor dedicado do cache nao depende de ReSTIR GI nem de DDGI: ele traca a partir do
        // G-buffer e termina no proprio cache. Depende so de o cache estar ligado e das tabelas
        // dele montadas — que e exatamente o que UpdatePassActive() responde.
        M.RadianceCacheUpdateActive = RadianceCache.UpdatePassActive();
        const u32 CacheDebugIndex = DebugTargets::IndexOf(FRadianceCache::kDebugTargetName);
        M.RadianceCacheDebugActive = RadianceCache.CanVisualize() &&
            CacheDebugIndex != DebugTargets::kInvalid &&
            (DebugTargetIndex == CacheDebugIndex ||
             std::find(DebugSelection.begin(), DebugSelection.end(), CacheDebugIndex) !=
                 DebugSelection.end());
        M.GeometricNormalWillRun = M.AOWillRun || M.RadianceCacheDebugActive;

        M.WaterReflectionDebug =
            Water.GetDebugMode() == FWaterRenderer::EDebugMode::Reflection;
        M.DedicatedWaterReflections = M.ReflectionsActive &&
            (Water.GetDebugMode() == FWaterRenderer::EDebugMode::Off || M.WaterReflectionDebug);
        M.WaterSceneCopiesReady = Targets.SceneColorCopy && Targets.SceneDepthCopy;

        M.ReliableMotionActive = TemporalMotion.IsReady() &&
            (M.ReflectionsActive || M.ReSTIRGIActive || M.ReSTIRDIActiveFrame);
        M.MotionHistoryValidThisFrame = TemporalMotion.HasHistory();

        M.VolShaftsActive = UseSunShafts && SunShafts.IsInitialized() && UseHeightFog;
        M.VolFogActive    = UseVolumetricFog && UseHeightFog && VolumetricFog.IsInitialized();
        M.FogSkyAnchor    = UseAtmosphereSky && Atmosphere.IsInitialized();
        return M;
    }

    // Camera e matrizes do frame. Hoistado do meio do RenderFrame: o segundo grupo (as
    // variantes sem translacao) morava ~280 linhas abaixo do primeiro, mas depende so do
    // mesmo View/Projection. O PrevVPNoTrans que ele le so e reescrito no FIM do frame,
    // entao juntar os dois aqui e equivalente (verificado).
    FFrameView Renderer::ResolveFrameView(const FFrameModes& _Modes, IUpscaler* _ActiveUp) {
        FFrameView V;
        V.Aspect = SwapChain.GetWidth() > 0 && SwapChain.GetHeight() > 0
                   ? static_cast<f32>(SwapChain.GetWidth()) / static_cast<f32>(SwapChain.GetHeight())
                   : 1.0f;
        V.View  = Camera.GetViewMatrix();
        V.NearZ = 0.1f;
        V.FarZ  = UseWater ? 20000.0f : 4000.0f;
        V.FovY  = GetFovY();
        V.ProjUnjittered = kReverseZ
            ? Mat44::PerspectiveFovReverseZLH(V.FovY, V.Aspect, V.NearZ, V.FarZ)
            : Mat44::PerspectiveFovLH(V.FovY, V.Aspect, V.NearZ, V.FarZ);

        V.Projection = V.ProjUnjittered;
        if (_Modes.UpscaleActive) {
            _ActiveUp->GetJitter(TemporalSampleIndex, V.JitterPxX, V.JitterPxY); // FSR: sequencia do SDK; DLSS: Halton
            V.ProjJitterYSign = -1.0f;
        } else if (_Modes.TAAActive) {
            const u32 kJitterPhases = 8;
            const u32 Idx = (TemporalSampleIndex % kJitterPhases) + 1;
            V.JitterPxX = Halton(Idx, 2) - 0.5f;
            V.JitterPxY = Halton(Idx, 3) - 0.5f;
        }
        if (_Modes.UpscaleActive || _Modes.TAAActive) {
            V.Projection.M[2][0] += V.JitterPxX * 2.0f / static_cast<f32>(RenderWidth());
            V.Projection.M[2][1] += V.ProjJitterYSign * V.JitterPxY * 2.0f / static_cast<f32>(RenderHeight());
        }
        V.JitterUv = Vec2{ V.JitterPxX / static_cast<f32>(RenderWidth()),
                           -V.ProjJitterYSign * V.JitterPxY / static_cast<f32>(RenderHeight()) };
        V.JitterPx = Vec2{ V.JitterPxX, -V.ProjJitterYSign * V.JitterPxY };

        V.ViewProjection     = V.View * V.Projection;
        V.ViewProjUnjittered = V.View * V.ProjUnjittered;
        V.CameraPosition     = Camera.GetPosition();

        V.ViewNoTrans = V.View;
        V.ViewNoTrans.M[3][0] = 0.0f;
        V.ViewNoTrans.M[3][1] = 0.0f;
        V.ViewNoTrans.M[3][2] = 0.0f;
        V.VPNoTrans    = V.ViewNoTrans * V.Projection;
        V.InvVPNoTrans = V.VPNoTrans.Inverse();
        V.VPNoTransUnjit    = V.ViewNoTrans * V.ProjUnjittered;
        V.SkyClipToPrevClip = V.VPNoTransUnjit.Inverse() * PrevVPNoTrans;
        V.InvViewProjFull  = V.ViewProjection.Inverse();
        V.InvViewProjUnjit = V.ViewProjUnjittered.Inverse();

        V.MipBias = (_Modes.UpscaleActive && RenderWidth() < OutputWidth())
            ? std::log2(static_cast<f32>(RenderWidth()) / static_cast<f32>(OutputWidth())) - 1.0f
            : 0.0f;

        {
            const Mat44& VP = V.ViewProjection;
            auto Col = [&](int j) { return Vec4{ VP.M[0][j], VP.M[1][j], VP.M[2][j], VP.M[3][j] }; };
            Vec4 c0 = Col(0), c1 = Col(1), c2 = Col(2), c3 = Col(3);
            V.FrustumPlanes[0] = { c3.X+c0.X, c3.Y+c0.Y, c3.Z+c0.Z, c3.W+c0.W };
            V.FrustumPlanes[1] = { c3.X-c0.X, c3.Y-c0.Y, c3.Z-c0.Z, c3.W-c0.W };
            V.FrustumPlanes[2] = { c3.X+c1.X, c3.Y+c1.Y, c3.Z+c1.Z, c3.W+c1.W };
            V.FrustumPlanes[3] = { c3.X-c1.X, c3.Y-c1.Y, c3.Z-c1.Z, c3.W-c1.W };
            V.FrustumPlanes[4] = { c2.X, c2.Y, c2.Z, c2.W };
            V.FrustumPlanes[5] = { c3.X-c2.X, c3.Y-c2.Y, c3.Z-c2.Z, c3.W-c2.W };
        }
        return V;
    }

    // Sol, lua, chuva e a luz-chave. So calculo — as publicacoes (constant buffer, parametros
    // de noite/estrelas da atmosfera) continuam no RenderFrame, lendo daqui. O MoonTint fica
    // local: e constante e so alimenta MoonLightCol e o MoonColorRaw publicado la.
    FFrameLighting Renderer::ResolveFrameLighting() {
        FFrameLighting L;
        // Redundante desde que o SunDir nasce unitario (ver a invariante no Renderer.h); fica como
        // rede contra um NaN vindo de fora, que e o que o NormalizedSafe existe para conter.
        L.SunN = SunDir.NormalizedSafe(DefaultSunDirection());

        L.RainSky    = Weather.GetDriveSky() ? Weather.GetRainAmount() : 0.0f;
        L.RainKeyDim = 1.0f - L.RainSky * 0.75f;
        L.RainAmbDim = 1.0f - L.RainSky * 0.40f;
        // POLITICA UNICA de escurecimento do CEU na chuva — o ceu procedural nao sabe da chuva,
        // entao quem consome a radiancia dele precisa atenuar. Era 1.0 no DDGI e no ReSTIR GI e
        // so as reflexoes atenuavam: dois dos tres consumidores do mesmo ceu ignoravam o
        // temporal, e o indireto continuava de dia limpo debaixo de tempestade.
        // Distinto do RainAmbDim, que escurece IRRADIANCIA (ambiente hemisferico), nao a
        // radiancia do ceu vista por um raio.
        L.RainSkyDim = 1.0f - L.RainSky * 0.65f;

        L.EffectiveSunColor = SunColorRGB;
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            const Vec3 T = Atmosphere.SunTransmittance(L.SunN);
            L.EffectiveSunColor = { SunColorRGB.X * T.X, SunColorRGB.Y * T.Y, SunColorRGB.Z * T.Z };
        }
        {
            const f32 hf = std::clamp(L.SunN.Y / 0.03f, 0.0f, 1.0f);
            const f32 HorizonFade = hf * hf * (3.0f - 2.0f * hf);
            L.EffectiveSunColor = { L.EffectiveSunColor.X * HorizonFade,
                                    L.EffectiveSunColor.Y * HorizonFade,
                                    L.EffectiveSunColor.Z * HorizonFade };
        }
        L.EffectiveSunColor = { L.EffectiveSunColor.X * L.RainKeyDim,
                                L.EffectiveSunColor.Y * L.RainKeyDim,
                                L.EffectiveSunColor.Z * L.RainKeyDim }; // F4: nublado de chuva

        L.MoonN = TimeOfDay.MoonDirection();

        const f32 nf     = std::clamp((0.0f - L.SunN.Y) / 0.15f, 0.0f, 1.0f);
        L.NightFactor    = nf * nf * (3.0f - 2.0f * nf);
        const f32 MoonSunCos = L.MoonN.X * L.SunN.X + L.MoonN.Y * L.SunN.Y + L.MoonN.Z * L.SunN.Z;
        L.MoonIllum      = (1.0f - MoonSunCos) * 0.5f;
        const f32 MoonUp = std::clamp(L.MoonN.Y * 8.0f, 0.0f, 1.0f);
        L.MoonOn         = TimeOfDay.MoonEnabled;

        L.MoonTrans = (UseAtmosphereSky && Atmosphere.IsInitialized())
                    ? Atmosphere.SunTransmittance(L.MoonN) : Vec3{ 1.0f, 1.0f, 1.0f };
        const Vec3 MoonTint = { 0.6f, 0.7f, 1.0f };
        L.MoonLightCol = { MoonTint.X * L.MoonTrans.X * L.RainKeyDim,
                           MoonTint.Y * L.MoonTrans.Y * L.RainKeyDim,
                           MoonTint.Z * L.MoonTrans.Z * L.RainKeyDim }; // F4: nublado
        L.MoonW = L.MoonOn ? (TimeOfDay.MoonIntensity * L.MoonIllum * L.NightFactor * MoonUp) : 0.0f;

        // MoonDiskSize e multiplicador do diametro lunar medio (0.518 grau), conforme o "x" do
        // editor. Antes o mesmo 1.5 era tratado como 1.5 grau e o disco ficava ~1.9x maior.
        const f32 MoonHalfAngleRad = 0.5f * ToRad * TimeOfDay.MoonDiskAngularDiameterDeg();
        L.CosMoonRadius = std::cos(MoonHalfAngleRad);
        // x2 sem textura: o disco procedural branco depende do brilho pra ter presenca.
        L.MoonDiskBright = L.MoonOn
            ? TimeOfDay.MoonDiskBrightness * (Atmosphere.HasMoonTexture() ? 1.0f : 2.0f)
            : 0.0f;
        L.MoonSkyScale = L.MoonOn ? (0.05f * TimeOfDay.MoonIntensity * L.MoonIllum) : 0.0f;

        L.KeyIsMoon = (L.SunN.Y <= 0.0f);
        L.KeyDir    = L.KeyIsMoon ? L.MoonN : L.SunN;
        L.KeyColor  = L.KeyIsMoon ? L.MoonLightCol : L.EffectiveSunColor;
        L.KeyInt    = L.KeyIsMoon ? L.MoonW : SunIntensity;
        L.CloudDim  = L.KeyIsMoon ? (L.MoonW / std::max(SunIntensity, 1e-3f)) : 1.0f;
        L.KeyCloudCol = { L.KeyColor.X * L.CloudDim, L.KeyColor.Y * L.CloudDim,
                          L.KeyColor.Z * L.CloudDim };
        return L;
    }

    void Renderer::TickWorldClock() {
        // O gate da captura esta AQUI e nao no delta porque o Time-of-Day e o unico consumidor de
        // delta que move o MUNDO em vez de assentar um valor — ver a nota no UpdateCamera. Com o
        // sol andando durante o aquecimento, duas capturas do mesmo bookmark sairiam com
        // iluminacao diferente apesar do reset deterministico, e o manifesto registrar a hora
        // final nao as tornaria comparaveis.
        //
        // REAFIRMAR, e nao so parar de avancar: o painel de TOD escreve direto na referencia do
        // GetTimeOfDay(), sem passar por setter nenhum, entao nao ha funil onde detectar a edicao.
        // Reescrever hora e sol a cada frame faz "o sol nao se move durante uma sessao" valer por
        // construcao, venha a escrita de onde vier — mesma politica da camera travada.
        if (Capture.SessionActive()) {
            TimeOfDay.TimeHours = CaptureSunHours;
            SunDir              = CaptureSunDir;
        } else if (TimeOfDay.Enabled) {
            TimeOfDay.Tick(LastDeltaTime);
            SetSunDirection(TimeOfDay.SunDirection());
        }
        {
            const f32 Target = Weather.GetRainAmount();
            const f32 Tau    = (Target > Weather.GetWetness()) ? 5.0f : 30.0f;
            Weather.SetWetness(Weather.GetWetness() +
                               (Target - Weather.GetWetness()) *
                               (1.0f - std::exp(-std::max(LastDeltaTime, 0.0f) / Tau)));
            if (Target <= 0.001f && Weather.GetWetness() < 0.005f) Weather.SetWetness(0.0f);
        }
    }

    FFrameAmbient Renderer::PublishFrameConstants(const FFrameView& _Vw,
                                                  const FFrameLighting& _Lt, u32 _FrameSlot,
                                                  FrameConstants* _CB) {
        // NOTA DE ORDEM (a unica reordenacao desta extracao): estas tres escritas corriam ANTES
        // do TickWorldClock. Descer para ca e inerte — o relogio so escreve SunDir e a molhadura,
        // e nenhum dos insumos abaixo (posicao da camera, HDRI, tempo decorrido, FrameIndex) e
        // tocado por ele. Em troca, TODA a publicacao no constant buffer passa a viver num lugar.
        _CB->CameraPosition = { _Vw.CameraPosition.X, _Vw.CameraPosition.Y, _Vw.CameraPosition.Z, 1.0f };

        const f32 IBLEnabled = HDREnv.HasHDRLoaded() ? 1.0f : 0.0f;
        _CB->IBLParams      = { IBLIntensity, IBLRotation,
                                static_cast<f32>(FHDREnvironment::kSpecularMips - 1),
                                IBLEnabled };
        // O .z e o contador ABSOLUTO, e continua sendo — nenhum shader le Time.z hoje (so o .w,
        // gate do AODebug), entao a escolha de indice aqui nao muda imagem nenhuma. Fica no
        // FrameIndex porque, se algum dia alguem ler, "quantos frames desde o boot" e a leitura
        // que o nome sugere; quem precisar de semente tem o TemporalSampleIndex, que e passado
        // explicitamente a cada passe. Campo sem consumidor: candidato a limpeza em outro commit.
        _CB->Time           = { ElapsedTime, LastDeltaTime,
                                static_cast<f32>(FrameIndex),
                                AODebug ? 1.0f : 0.0f };

        _CB->SunDirection   = { _Lt.SunN.X, _Lt.SunN.Y, _Lt.SunN.Z, SunIntensity };
        _CB->SunColor       = { _Lt.EffectiveSunColor.X, _Lt.EffectiveSunColor.Y,
                                _Lt.EffectiveSunColor.Z, 0.0f };
        // Variante CRUA (sem transmitancia, sem HorizonFade) p/ o caminho por pixel do deferred
        // e do ForwardBlend. O SunColor acima continua intacto: todos os outros consumidores
        // (fog volumetrico, nuvens, agua, sun shafts, readout do editor) seguem sem mudanca.
        _CB->SunColorRaw    = { SunColorRGB.X * _Lt.RainKeyDim, SunColorRGB.Y * _Lt.RainKeyDim,
                                SunColorRGB.Z * _Lt.RainKeyDim, 0.0f };

        _CB->MoonDirection = { _Lt.MoonN.X, _Lt.MoonN.Y, _Lt.MoonN.Z, _Lt.MoonW };
        _CB->MoonColor     = { _Lt.MoonLightCol.X, _Lt.MoonLightCol.Y, _Lt.MoonLightCol.Z, 0.0f };
        // MoonTint cru (sem transmitancia), so com o dim de chuva — mesma logica do SunColorRaw.
        _CB->MoonColorRaw  = { 0.6f * _Lt.RainKeyDim, 0.7f * _Lt.RainKeyDim,
                               1.0f * _Lt.RainKeyDim, 0.0f };

        // Transmitancia por pixel: so com o ceu procedural (o LUT descreve ESTA atmosfera).
        {
            const bool AtmoOn = UseAtmosphereSky && Atmosphere.IsInitialized();
            _CB->AtmoLightParams = {
                AtmoOn ? Atmosphere.BottomRadiusKm() : 0.0f,
                AtmoOn ? Atmosphere.TopRadiusKm()    : 0.0f,
                kKmPerWorldUnit,
                (AtmoOn && UsePerPixelAtmoTransmittance) ? 1.0f : 0.0f };
        }

        Atmosphere.SetNightParams(_Lt.MoonN, _Lt.CosMoonRadius, _Lt.MoonDiskBright,
                                  TimeOfDay.StarIntensity, _Lt.NightFactor, ElapsedTime);
        Atmosphere.SetMoonSkyLight(_Lt.MoonSkyScale, _Lt.MoonOn ? _Lt.MoonIllum : 0.0f);

        {
            const f32  LatR  = TimeOfDay.LatitudeDeg    * ToRad;
            const f32  NoR   = TimeOfDay.NorthOffsetDeg * ToRad;
            const Vec3 Pole  = { std::cos(LatR) * std::sin(NoR), std::sin(LatR),
                                 std::cos(LatR) * std::cos(NoR) };
            // TOD usa hora sideral local (dia sideral ~23h56m); no modo manual, preserva a
            // rotacao artistica lenta que existia antes.
            const f32 Angle = TimeOfDay.Enabled ? TimeOfDay.StarRotationAngleRad()
                                                : (ElapsedTime * 0.004f);
            Atmosphere.SetStarRotation(Pole, Angle);
        }

        FFrameAmbient Amb;
        {
            Vec3& Sky    = Amb.Sky;
            Vec3& Ground = Amb.Ground;
            const bool Physical = UseAtmosphereSky && Atmosphere.IsInitialized() &&
                                  Atmosphere.GetSkyAmbient(_FrameSlot, Sky, Ground);
            if (!Physical) {
                auto Sat = [](f32 X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); };
                const f32 SunY   = _Lt.SunN.Y;
                const f32 Day    = Sat(SunY * 4.0f + 0.2f);
                const f32 LowSun = Sat(1.0f - SunY * 2.5f);
                const Vec3 Zenith  = { 0.18f, 0.30f, 0.55f };
                const Vec3 Horizon = { 0.60f, 0.40f, 0.26f };
                Sky    = (Zenith + (Horizon - Zenith) * LowSun) * Day;
                Ground = Sky * 0.35f;
            }

            Sky    = { Sky.X * _Lt.RainAmbDim, Sky.Y * _Lt.RainAmbDim, Sky.Z * _Lt.RainAmbDim };
            Ground = { Ground.X * _Lt.RainAmbDim, Ground.Y * _Lt.RainAmbDim, Ground.Z * _Lt.RainAmbDim };
            _CB->SkyAmbientColor    = { Sky.X, Sky.Y, Sky.Z,
                                        UseAtmosphereAmbient ? 1.0f : 0.0f };
            _CB->GroundAmbientColor = { Ground.X, Ground.Y, Ground.Z, AtmoAmbientIntensity };

            // SH-L1 do MESMO integral. So vale com o ceu procedural: o fallback analitico acima
            // nao tem SH, e nesse caso o shader cai nas 2 cores chapadas.
            Vec4 SH[3]{};
            const bool HasSH = Physical && Atmosphere.GetSkyAmbientSH(_FrameSlot, SH);
            if (HasSH) {
                // O dim de chuva escurece IRRADIANCIA, entao escala a SH inteira — mesma
                // politica que as 2 cores acima recebem.
                for (u32 c = 0; c < 3; ++c)
                    SH[c] = { SH[c].X * _Lt.RainAmbDim, SH[c].Y * _Lt.RainAmbDim,
                              SH[c].Z * _Lt.RainAmbDim, SH[c].W * _Lt.RainAmbDim };
            }
            _CB->SkyAmbientSHR = SH[0];
            _CB->SkyAmbientSHG = SH[1];
            _CB->SkyAmbientSHB = SH[2];
            _CB->SkyAmbientSHParams = { (HasSH && UseSkyAmbientSH) ? 1.0f : 0.0f,
                                        0.0f, 0.0f, 0.0f };
        }

        if (UseGI && DDGI.IsReady()) {
            const Vec3 GMin = DDGI.GridMin();
            const Vec3 GCnt = DDGI.GridCount();
            _CB->DDGIGridMin   = { GMin.X, GMin.Y, GMin.Z, DDGI.Spacing() };

            _CB->DDGIGridCount = { GCnt.X, GCnt.Y, GCnt.Z, GIDebug ? 2.0f : 1.0f };
            _CB->DDGIParams    = { DDGI.GetIntensity(), DDGI.TileSizeF(),
                                   DDGI.AtlasW(), DDGI.AtlasH() };

            const f32 GIFlags = (GIChebyshev ? 1.0f : 0.0f) + (GISkipInactiveProbes ? 2.0f : 0.0f)
                              + (GISkipInactiveFallback ? 4.0f : 0.0f);
            _CB->DDGIDistParams = { DDGI.DistTileSizeF(), DDGI.DistAtlasW(),
                                    DDGI.DistAtlasH(), GIFlags };
            _CB->DDGIBiasParams = { DDGI.GetSurfaceBiasScale(), DDGI.GetSurfaceBiasMax(),
                                    DDGI.GetVolumeFadeProbes(), 0.0f };
            _CB->DDGICascades   = DDGI.CascadeConstants();
        } else {
            _CB->DDGIGridMin    = { 0.0f, 0.0f, 0.0f, 1.0f };
            _CB->DDGIGridCount  = { 0.0f, 0.0f, 0.0f, 0.0f };
            _CB->DDGIParams     = { 0.0f, 6.0f, 1.0f, 1.0f };
            _CB->DDGIDistParams = { 14.0f, 1.0f, 1.0f, 0.0f };
            _CB->DDGIBiasParams = { 0.2f, 0.0f, 0.0f, 0.0f };
            // O default do POD ja traz espacamento 1 nas quatro entradas — ver
            // FDDGICascadeConstants. Antes isto era garantido A MAO aqui, e por isso valia so
            // para este cbuffer.
            _CB->DDGICascades   = {};
        }

        return Amb;
    }

    void Renderer::PushRayTracingFrameState(const FFrameModes& _Modes) {
        // DLSS Ray Reconstruction: denoiser neural que substitui NRD + SR. Precisa do RR inicializado
        // e dos guides prontos; o eval acontece no bloco de upscale (ActiveUpscaler() == &DlssRR).
        // Instrumentacao de timer: um gate so, empurrado todo frame. kInvalidSlot manda o passe
        // de volta p/ a PSO normal — desligado nao custa uma instrucao (ver FShaderTimer).
        TimerCaptureActive = RtShaderTimer && FShaderTimer::IsAvailable() &&
                             TimerGI.IsReady() && TimerReflections.IsReady();
        ReSTIRGI.SetTimerSlot(TimerCaptureActive ? TimerGI.UavSlot()
                                                 : FShaderTimer::kInvalidSlot);
        Reflections.SetTimerSlot(TimerCaptureActive ? TimerReflections.UavSlot()
                                                    : FShaderTimer::kInvalidSlot);

        // Perfil de epsilons: um so p/ a engine inteira, empurrado todo frame (copia barata). Sem
        // isto cada passe teria a propria copia e o sweep de calibracao mexeria em metade deles.
        ReSTIRGI.SetRayEpsilons(RayEps);
        Reflections.SetRayEpsilons(RayEps);
        Reflections.SetWaterReflectionScale(Water.GetReflectionScale());
        Reflections.SetWaterWindDirection(Water.GetWindDirection());
        DDGI.SetRayEpsilons(RayEps);
        // O produtor do cache traca os MESMOS raios de gather e de sombra dos outros tres; um
        // perfil proprio aqui seria a copia por passe que a centralizacao fechou.
        RadianceCache.SetRayEpsilons(RayEps);

        // Gather do 2o bounce (ShadeSurfaceHit): mesmo raciocinio do perfil de epsilons — um so
        // para os tres passes, empurrado todo frame. O skipMode espelha exatamente o que o
        // deferred usa, senao o bounce pesaria as sondas com uma politica e a tela com outra.
        {
            FGIHitSampling GIHit;
            GIHit.DistTile   = DDGI.DistTileSizeF();
            GIHit.DistAtlasW = DDGI.DistAtlasW();
            GIHit.DistAtlasH = DDGI.DistAtlasH();
            GIHit.SkipMode   = GISkipInactiveProbes
                             ? (GISkipInactiveFallback ? 2.0f : 1.0f) : 0.0f;
            GIHit.BiasScale  = DDGI.GetSurfaceBiasScale();
            GIHit.BiasMax    = DDGI.GetSurfaceBiasMax();
            GIHit.FadeProbes = DDGI.GetVolumeFadeProbes();
            GIHit.TerminatorOff = GIMeasureTerminatorOff; // gate de medicao (ver Renderer.h)
            // HABILITACAO por frame, contra a EXISTENCIA fisica que escolhe o descritor no
            // GIFallbackBindingsForSetup. O `UseGI` mora deste lado justamente porque este lado e
            // recalculado todo frame: o SetUseGI so vira o booleano, entao um gate que dependesse
            // dele no lado do descritor ficaria latched no setup e mentiria ate o proximo.
            //
            // Sem volume os passes de RT ainda recebem descritores validos — os neutros —, e e
            // ESTE campo que impede o gather de ler o atlas 1x1 zerado como se fosse irradiancia.
            GIHit.FallbackAvailable = UseGI && DDGI.IsReady();
            DDGI.SetGIHitSampling(GIHit);
            Reflections.SetGIHitSampling(GIHit);
            const FDDGICascadeConstants GICasc = DDGI.CascadeConstants();
            Reflections.SetGICascades(GICasc);
            ReSTIRGI.SetGICascades(GICasc);
            ReSTIRGI.SetGIHitSampling(GIHit);
            // O produtor do cache recebe o MESMO bloco pelo unico campo que ele le: o piso de
            // roughness do secundario. As cascatas nao vao junto — ele nao amostra sonda, e a
            // cauda do cbuffer dele viaja zerada de proposito (ver RadianceCacheUpdateConstants).
            RadianceCache.SetGIHitSampling(GIHit);
        }

        ReSTIRGI.SetUseNrd(_Modes.NrdIndirectMode); // Modes.RRMode => false => ReSTIR entrega GI cru (ruidoso)
        Reflections.SetUseNrd(_Modes.NrdIndirectMode);
        Reflections.SetRawSpec(_Modes.RRMode);        // reflexao crua (Resolved direto) p/ o RR denoisar
    }

    void Renderer::UpdateAtmosphereAndVolumetrics(const FFrameModes& _Modes, const FFrameView& _Vw,
                                                  const FFrameLighting& _Lt,
                                                  const FFrameAmbient& _Amb, u32 _FrameSlot,
                                                  FrameConstants* _CB) {
        Atmosphere.UpdatePerFrame(_FrameSlot, _Lt.SunN, _Vw.InvVPNoTrans, _Vw.VPNoTrans,
                                  _Vw.InvViewProjFull, _Vw.CameraPosition, kKmPerWorldUnit,
                                  static_cast<f32>(RenderWidth()), static_cast<f32>(RenderHeight()),
                                  static_cast<f32>(OutputWidth()), static_cast<f32>(OutputHeight()));

        const f32 FogDensityBase = Fog.GetDensity();
        if (_Lt.RainSky > 0.0f) Fog.SetDensity(FogDensityBase * (1.0f + _Lt.RainSky * 1.5f));

        const Vec4 ShaftsFogCollapsed = Fog.CollapsedFogParams(_Vw.CameraPosition.Y);

        Vec3 CamForwardW{ 0.0f, 0.0f, 1.0f };
        {
            const Mat44& IM = _Vw.InvViewProjUnjit;
            const f32 v[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
            f32 w[4];
            for (int j = 0; j < 4; ++j)
                w[j] = v[2] * IM.M[2][j] + v[3] * IM.M[3][j];
            if (std::fabs(w[3]) > 1e-9f) {
                const Vec3 P{ w[0] / w[3], w[1] / w[3], w[2] / w[3] };
                CamForwardW = (P - _Vw.CameraPosition).NormalizedSafe(Vec3{ 0.0f, 0.0f, 1.0f });
            }
        }

        const f32 ShaftMarchMax = _Modes.VolShaftsActive
            ? (_Modes.VolFogActive
                ? std::min(SunShafts.GetVolMaxDist(), VolumetricFog.GetMaxDistance())
                : SunShafts.GetVolMaxDist())
            : 0.0f;
        if (_Modes.VolFogActive) {
            FVolumetricFogPass::FFrameParams VF{};
            VF.InvViewProjUnjit = _Vw.InvViewProjUnjit;
            VF.ViewProjUnjit    = _Vw.ViewProjUnjittered;
            VF.FrameIndex       = TemporalSampleIndex;
            VF.CameraPos        = _Vw.CameraPosition;
            VF.CameraForward    = CamForwardW;
            VF.DirToSun         = _Lt.KeyDir;
            VF.SunColorTimesIntensity = { _Lt.KeyColor.X * _Lt.KeyInt, _Lt.KeyColor.Y * _Lt.KeyInt,
                                          _Lt.KeyColor.Z * _Lt.KeyInt };
            // Shafts own the high-frequency solar term only over their configured
            // near range. The froxel resumes the sun afterwards, while extinction,
            // ambient, GI and local lights remain present over the full volume.
            VF.InjectDirectionalLight = true;
            VF.DirectionalLightStartDistance = _Modes.VolShaftsActive ? ShaftMarchMax : 0.0f;
            VF.CollapsedFog     = ShaftsFogCollapsed;
            VF.SkyAmbient       = _Amb.Sky;
            VF.NearZ            = _Vw.NearZ;
            VF.RenderW          = RenderWidth();
            VF.RenderH          = RenderHeight();
            if (UseGI && DDGI.IsReady()) {
                const Vec3 GMin = DDGI.GridMin();
                const Vec3 GCnt = DDGI.GridCount();
                VF.DDGIGridMin   = { GMin.X, GMin.Y, GMin.Z, DDGI.Spacing() };
                VF.DDGIGridCount = { GCnt.X, GCnt.Y, GCnt.Z, 1.0f };
                VF.DDGICascades  = DDGI.CascadeConstants();
                VF.DDGIParams    = { DDGI.GetIntensity(), DDGI.TileSizeF(),
                                     DDGI.AtlasW(), DDGI.AtlasH() };
                VF.DDGIVolumeFadeProbes = DDGI.GetVolumeFadeProbes();
            }
            VolumetricFog.UpdatePerFrame(_FrameSlot, VF);
        } else if (VolumetricFog.IsInitialized()) {
            VolumetricFog.ResetHistory(); // efeito dormiu: historia/PrevVP obsoletos
        }

        // Ancoragem do height fog na atmosfera: so faz sentido com o ceu procedural ligado (com
        // HDRI/skybox o SkyView LUT nao descreve o ceu que esta na tela). Lt.SunN, nao Lt.KeyDir: o
        // LUT e dobrado no azimute do SOL, entao a lua daria uv errado a noite.
        FFogPass::FVolumetricMatchParams FogMatch{};
        FogMatch.Enabled = _Modes.VolFogActive;
        if (_Modes.VolFogActive) {
            const Vec3 MediumAlbedo = VolumetricFog.GetAlbedo();
            const f32 AmbientIntensity = VolumetricFog.GetAmbientIntensity();
            FogMatch.Albedo = MediumAlbedo;
            FogMatch.AmbientRadiance = { _Amb.Sky.X * AmbientIntensity,
                                         _Amb.Sky.Y * AmbientIntensity,
                                         _Amb.Sky.Z * AmbientIntensity };
            FogMatch.SunRadiance = { _Lt.KeyColor.X * _Lt.KeyInt, _Lt.KeyColor.Y * _Lt.KeyInt,
                                     _Lt.KeyColor.Z * _Lt.KeyInt };
            FogMatch.ExtinctionScale = VolumetricFog.GetExtinctionScale();
            // The froxel smoothly regains the solar term before its boundary,
            // so the far analytical continuation always matches the base volume.
            FogMatch.DirectionalPhaseG = VolumetricFog.GetPhaseG();
            FogMatch.DirectionalScatteringScale = 1.0f;
            FogMatch.AmbientTransitionDistance =
                std::max(25.0f, VolumetricFog.GetMaxDistance() * 0.5f);
        }
        Fog.UpdatePerFrame(_FrameSlot, _Vw.InvViewProjFull, _Vw.CameraPosition, kKmPerWorldUnit, _Lt.KeyDir,
                           _Vw.NearZ, _Vw.FarZ, RenderWidth(), RenderHeight(),
                           UseAerialPerspective, UseHeightFog, Atmosphere.AerialDepthKm(),
                           Atmosphere.AerialSliceCount(),
                           _Modes.VolShaftsActive, _Modes.VolFogActive, VolumetricFog.GetMaxDistance(),
                           VolumetricFog.GridZParams(), CamForwardW,
                           _Lt.SunN,
                           _Modes.FogSkyAnchor ? Atmosphere.ViewHeightKm()   : 0.0f,
                           _Modes.FogSkyAnchor ? Atmosphere.BottomRadiusKm() : 0.0f,
                           Fog.GetHeightFogSkyContribution(), FogMatch);
        Fog.SetDensity(FogDensityBase);

        const f32 CloudGroundRadius = 6360.0f + FAtmosphere::kPlanetRadiusOffsetKm;
        const f32 CloudCovBase = VolumetricClouds.GetCoverage();
        if (_Lt.RainSky > 0.0f) {
            const f32 RainCov = std::min(_Lt.RainSky * 1.4f, 1.0f) * 0.92f;
            VolumetricClouds.SetCoverage(std::max(CloudCovBase, RainCov));
        }
        VolumetricClouds.UpdatePerFrame(_FrameSlot, _Vw.InvVPNoTrans, _Vw.InvViewProjFull,
                                        _Vw.ViewProjUnjittered, _Vw.CameraPosition, kKmPerWorldUnit,
                                        CloudGroundRadius, _Lt.KeyDir, _Lt.KeyCloudCol,
                                        _Amb.Sky, _Amb.Ground, ElapsedTime, TemporalSampleIndex,
                                        SunIntensity);
        VolumetricClouds.SetCoverage(CloudCovBase);

        Vec4 CloudShadowP{ 0.0f, 0.0f, 0.0f, 0.0f };
        Vec4 CloudShadowP2{ 0.0f, 0.0f, 0.0f, 0.0f };
        {
            const f32 KeyY = _Lt.KeyDir.Y > 0.05f ? _Lt.KeyDir.Y : 0.05f;
            const bool CloudShadowOn = UseClouds && VolumetricClouds.IsInitialized() &&
                                       VolumetricClouds.GetShadowsEnabled() && _Lt.KeyDir.Y > 0.02f;
            CloudShadowP  = { VolumetricClouds.ShadowCenterX(),
                              VolumetricClouds.ShadowCenterZ(),
                              VolumetricClouds.ShadowInvExtent(),
                              CloudShadowOn ? VolumetricClouds.GetShadowStrength() : 0.0f };
            CloudShadowP2 = { kKmPerWorldUnit,
                              VolumetricClouds.GetBottomAltitude(),
                              _Lt.KeyDir.X / KeyY, _Lt.KeyDir.Z / KeyY };
            _CB->CloudShadowParams  = CloudShadowP;
            _CB->CloudShadowParams2 = CloudShadowP2;
            if (VolumetricClouds.IsInitialized()) {
                D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(GBuffer.SRVTableStart() + 4);
                D3D12_CPU_DESCRIPTOR_HANDLE Src =
                    SRVHeap.CpuHandleStaging(VolumetricClouds.ShadowSRV());
                UINT One = 1;
                Device.Native()->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }

        if (_Modes.VolShaftsActive) {
            const Vec3 KeyColInt = { _Lt.KeyColor.X * _Lt.KeyInt, _Lt.KeyColor.Y * _Lt.KeyInt,
                                     _Lt.KeyColor.Z * _Lt.KeyInt };
            const f32 ShaftNoiseFrame =
                (_Modes.TAAActive || _Modes.UpscaleActive || SunShafts.GetVolTemporal())
                    ? static_cast<f32>(TemporalSampleIndex % 64u) : 0.0f;
            const Vec3 ShaftMediumAlbedo = _Modes.VolFogActive
                ? VolumetricFog.GetAlbedo() : Vec3{ 1.0f, 1.0f, 1.0f };
            const f32 ShaftExtinctionScale = _Modes.VolFogActive
                ? VolumetricFog.GetExtinctionScale() : 1.0f;
            SunShafts.UpdateVolumetric(_FrameSlot, _Lt.KeyDir, KeyColInt, ShaftsFogCollapsed,
                                       ShaftNoiseFrame, _Vw.InvViewProjFull, _Vw.CameraPosition,
                                       _Vw.ViewProjUnjittered, CloudShadowP, CloudShadowP2,
                                       ShaftMediumAlbedo, ShaftExtinctionScale,
                                       0.0f, ShaftMarchMax);
        } else if (SunShafts.IsInitialized()) {
            SunShafts.ResetHistory();
        }

        if (_Modes.VolFogActive) VolumetricFog.PatchCloudShadow(CloudShadowP, CloudShadowP2);
    }

    void Renderer::UpdateWaterAndOcean(const FFrameModes& _Modes, const FFrameView& _Vw,
                                       const FFrameLighting& _Lt, const FFrameAmbient& _Amb,
                                       u32 _FrameSlot) {
        if (!(UseWater && Water.IsInitialized())) return;

        const bool WaterAtmoRefl = UseAtmosphereSky && Atmosphere.IsInitialized();
        const f32 WaterReflIntensity =
            WaterAtmoRefl ? (1.0f - _Lt.RainSky * 0.65f) : IBLIntensity;
        // ViewProjection e a sua inversa vem do FFrameView: o `WaterInvViewProj` local era
        // `Vw.ViewProjection.Inverse()`, EXATAMENTE a expressao que produz o InvViewProjFull
        // (ver ResolveFrameView) — mesma entrada, mesma chamada. Era um inverse 4x4 por frame
        // recalculado a toa.
        Water.UpdatePerFrame(_FrameSlot, _Vw.ViewProjection, _Vw.Projection, _Vw.InvViewProjFull,
                             _Vw.ViewProjUnjittered, PrevViewProj, _Vw.CameraPosition, _Lt.KeyDir,
                             _Lt.KeyInt, _Lt.KeyColor, _Amb.Sky, ElapsedTime,
                             WaterAtmoRefl || HDREnv.HasHDRLoaded(), WaterReflIntensity,
                             RenderWidth(), RenderHeight(), _Vw.NearZ, _Vw.FarZ,
                             _Modes.WaterSceneCopiesReady, UseAtmosphereSky,
                             _Modes.DedicatedWaterReflections);
        for (u32 c = 0; c < kOceanCascades; ++c) {
            if (!Ocean[c].IsInitialized()) continue;
            Ocean[c].SetTime(ElapsedTime);
            Ocean[c].SetWindDirection(Water.GetWindDirection());
            Ocean[c].SetWindSpeed(Water.GetWindSpeed());
            Ocean[c].SetSpectrumFetch(Water.GetSpectrumFetch());
            Ocean[c].SetOceanDepth(Water.GetOceanDepth());
            Ocean[c].SetSwell(Water.GetSwell());
            Ocean[c].SetAmplitude(Water.GetWavesAmount());
            Ocean[c].SetGeometryScales(Water.GetFFTDisplacementScale() *
                                       Water.GetWavesSize(),
                                       Water.GetFFTChoppyScale());
        }
    }

    FPassContext Renderer::MakePassContext(const FFrameModes& _Modes, const FFrameView& _Vw,
                                           const FFrameLighting& _Lt, const FFrameAmbient& _Amb,
                                           u32 _FrameSlot) {
        FPassContext C;
        C.Cmd      = CommandQueue.List();
        C.Device   = Device.Native();
        C.SRVHeap  = &SRVHeap;
        C.Profiler = &GpuProfiler;

        C.FrameSlot    = _FrameSlot;
        C.RenderWidth  = RenderWidth();
        C.RenderHeight = RenderHeight();
        C.OutputWidth  = OutputWidth();
        C.OutputHeight = OutputHeight();

        C.Modes   = &_Modes;
        C.View    = &_Vw;
        C.Light   = &_Lt;
        C.Ambient = &_Amb;

        C.Targets      = &Targets;
        C.FrameCB      = ConstantBuffer->GetGPUVirtualAddress() +
                         static_cast<u64>(_FrameSlot) * sizeof(FrameConstants);
        C.ObjectCBBase = ObjectCB->GetGPUVirtualAddress();
        C.DSV          = Targets.DSVHeap.CpuHandle(0);

        C.Viewport.Width    = static_cast<FLOAT>(C.RenderWidth);
        C.Viewport.Height   = static_cast<FLOAT>(C.RenderHeight);
        C.Viewport.MinDepth = 0.0f;
        C.Viewport.MaxDepth = 1.0f;
        C.Scissor.right     = static_cast<LONG>(C.RenderWidth);
        C.Scissor.bottom    = static_cast<LONG>(C.RenderHeight);
        return C;
    }

    // Abre a gravacao da cena: limpa cor/mascaras/profundidade e amarra viewport, scissor e o
    // heap de descriptors que valem pelo frame inteiro.
    void Renderer::BeginSceneRecording(FPassContext& _Ctx) {
        auto* CommandList = _Ctx.Cmd;
        const auto& DSV   = _Ctx.DSV;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        {
            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(Targets.UpscaleReactiveMask.Get(), Targets.UpscaleReactiveState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(Targets.UpscaleCompositionMask.Get(), Targets.UpscaleCompositionState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            auto HDR_RTV = Targets.HDRRTVHeap.CpuHandle(0);
            const FLOAT MaskClear[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            CommandList->OMSetRenderTargets(1, &HDR_RTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(HDR_RTV, ClearColor, 0, nullptr);
            CommandList->ClearRenderTargetView(
                Targets.UpscaleMaskRTVHeap.CpuHandle(0), MaskClear, 0, nullptr);
            CommandList->ClearRenderTargetView(
                Targets.UpscaleMaskRTVHeap.CpuHandle(1), MaskClear, 0, nullptr);
            CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0, nullptr);
        }

        CommandList->RSSetViewports(1, &_Ctx.Viewport);
        CommandList->RSSetScissorRects(1, &_Ctx.Scissor);

        ID3D12DescriptorHeap* DescriptorHeaps[] = { SRVHeap.Native() };
        CommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);
    }

    // Simulacao da agua na GPU, ceu/atmosfera e o shadow map das nuvens. Tudo isto escreve em
    // alvos PROPRIOS dos subsistemas (LUTs, cascatas de FFT, mapa de sombra) e no HDR ainda
    // vazio — nao depende de geometria nenhuma, por isso abre o frame.
    void Renderer::RecordSkyAndClouds(FPassContext& _Ctx) {
        auto* CommandList        = _Ctx.Cmd;
        const FFrameView& Vw     = *_Ctx.View;
        const FFrameLighting& Lt = *_Ctx.Light;
        const u32 FrameSlot      = _Ctx.FrameSlot;

        if (UseWater && Ocean[0].IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Água — FFT");
            for (u32 c = 0; c < kOceanCascades; ++c)
                Ocean[c].RecordCompute(FrameSlot, CommandList, SRVHeap);
        }

        GpuProfiler.Begin(CommandList, "Céu e atmosfera");
        // Antes do sky-view: se algum parametro fisico mudou (MarkDirty), transmitancia e
        // multiscatter sao re-bakeadas na command list deste frame. No-op no caso comum.
        if (Atmosphere.IsInitialized()) Atmosphere.RecordBakeIfDirty(CommandList);
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            Atmosphere.RecordSkyViewBake(CommandList);
            Atmosphere.RecordSkyAmbientIntegration(CommandList);
            if (UseWater && Water.IsInitialized())
                Atmosphere.RecordSkyReflectionBake(CommandList);
            Atmosphere.RenderSky(CommandList, SRVHeap);
            if (Lt.NightFactor > 0.001f && TimeOfDay.StarIntensity > 0.0f)
                Atmosphere.RenderStars(CommandList, SRVHeap);
        } else if (ShowSkybox && HDREnv.HasHDRLoaded()) {
            Skybox.Render(FrameSlot, CommandList, SRVHeap, HDREnv.EnvCubeSRV(),
                          Vw.InvVPNoTrans, IBLIntensity, IBLRotation);
        }

        if (Atmosphere.IsInitialized()) {
            Atmosphere.RecordAerialPerspectiveBake(CommandList);
        }
        GpuProfiler.End(CommandList); // Céu e atmosfera

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Sombra das nuvens");
            VolumetricClouds.RecordShadowMap(CommandList, SRVHeap);
        }
    }

    // Empacota as luzes puntuais para o MUNDO INDIRETO e grava os caches que os raios leem:
    // extracao de mesh lights, build do ReGIR e o passe do DDGI (fila assincrona quando da).
    // Termina empurrando o estado por-frame para reflexoes e ReSTIR GI, que e onde a contagem
    // de luzes empacotada acima e consumida — por isso os tres andam juntos.
    void Renderer::PrepareIndirectLighting(FPassContext& _Ctx) {
        auto* CommandList             = _Ctx.Cmd;
        const FFrameModes& Modes      = *_Ctx.Modes;
        const FFrameView& Vw          = *_Ctx.View;
        const FFrameLighting& Lt      = *_Ctx.Light;
        const u32 FrameSlot           = _Ctx.FrameSlot;
        const auto& DSV               = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        u64& GIComputeFence           = _Ctx.AsyncGIFence; // sai daqui e e esperado la na frente

        u32 GILightCount = 0;
        u64 GILightSetSignature = 1469598103934665603ull; // FNV-1a dos IDs na ordem compacta
        {
            FGPULightGI* Dst = reinterpret_cast<FGPULightGI*>(
                MappedGILightBase + static_cast<size_t>(FrameSlot) * kMaxLights * sizeof(FGPULightGI));
            for (FLight& L : Scene.Lights()) {
                // O caminho direto atribui a identidade mais adiante, mas o ReGIR e construido
                // antes dele. Atribuir aqui garante que o historico nunca use indice como ID.
                if (L.Id == 0) L.Id = Scene.AllocObjectId();
                if (!L.Enabled || L.Intensity <= 0.0f || L.AttenuationRadius <= 0.0f) continue;
                // Peso de RT: com 0 a luz sai da lista do indireto por completo (nao so escurece —
                // some do hit, economizando o shadow ray dela). E o caso da luz que so existia p/
                // representar uma malha emissiva que agora ilumina sozinha. O raster nao ve isto.
                if (L.RTWeight <= 0.0f) continue;
                if (GILightCount >= kMaxLights) break;

                const f32 RTW = L.RTWeight;
                FGPULightGI G;
                G.PosInvRadius      = { L.Position.X, L.Position.Y, L.Position.Z,
                                        1.0f / L.AttenuationRadius };
                G.ColorSourceRadius = { L.Color.X * L.Intensity * RTW, L.Color.Y * L.Intensity * RTW,
                                        L.Color.Z * L.Intensity * RTW,
                                        std::max(L.SourceRadius, 0.01f) };
                if (L.Type == ELightType::Spot) {
                    const Vec3 D = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                    const f32 OuterDeg = std::clamp(L.OuterConeDeg, 1.0f, 89.0f);
                    const f32 InnerDeg = std::clamp(L.InnerConeDeg, 0.0f, OuterDeg);
                    const f32 CosOuter = std::cos(OuterDeg * ToRad);
                    const f32 CosInner = std::cos(InnerDeg * ToRad);
                    G.DirCosOuter = { D.X, D.Y, D.Z, CosOuter };
                    G.SpotParams  = { 1.0f / std::max(CosInner - CosOuter, 1e-4f),
                                      0.0f, 0.0f, 0.0f };
                } else {
                    G.DirCosOuter = { 0.0f, -1.0f, 0.0f, -2.0f };
                    G.SpotParams  = { 0.0f, 0.0f, 0.0f, 0.0f };
                }
                Dst[GILightCount++] = G;
                GILightSetSignature ^= L.Id;
                GILightSetSignature *= 1099511628211ull;
            }
        }
        GILightSetSignature ^= static_cast<u64>(GILightCount);

        // TlasFlagsDirty: mask/FORCE_NON_OPAQUE/two-sided de uma instancia mudaram (edicao de
        // material no editor) sem a cena se mexer, entao a versao de transforms sozinha nao
        // pediria rebuild.
        if (RaytracingScene.IsBuilt() &&
            (Scene.TransformsVersion() != TlasTransformsVersion || TlasFlagsDirty)) {
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> TlasCL;
            if (SUCCEEDED(CommandList->QueryInterface(IID_PPV_ARGS(&TlasCL))) &&
                RaytracingScene.RecordTlasRebuild(TlasCL.Get(), Scene, FrameSlot)) {
                TlasTransformsVersion = Scene.TransformsVersion();
                TlasFlagsDirty        = false;
            }
        }

        // ReGIR nasce na fila direta antes do ponto onde o DDGI pode bifurcar para compute. No
        // caminho async, SubmitSegmentAndContinue publica estas escritas e a fila compute espera
        // o mesmo fence; no sincrono/reflexoes, a ordem da propria command list basta.
        // Nao reconstrua os 2048 pools se nenhum passe vai consultar hits secundarios neste
        // frame. O toggle sozinho nao basta: DDGI, reflexoes e ReSTIR GI podem estar todos off.
        // O passe de update do cache entra na lista: ele sombreia hits com luz local pelo MESMO
        // PT_AddDirectLocal, entao e consumidor de ReGIR como qualquer trace. Sem ele aqui, o A/B
        // da Fase 3 — que desliga DDGI, ReSTIR GI e reflexoes para isolar o produtor — deixaria a
        // grade sem ser construida e o updater cairia no loop O(N) de luzes. Nao seria errado (o
        // loop e a referencia exata), mas o custo do passe mudaria conforme toggles que nao tem
        // nada a ver com ele, e e justamente esse custo que a fase precisa medir isolado.
        const bool HasReGIRConsumer = (UseGI && DDGI.IsReady()) ||
                                      Modes.ReflectionsActive || Modes.ReSTIRGIActive ||
                                      Modes.RadianceCacheUpdateActive;
        // Extracao dos triangulos emissivos. Sai de graca no caso comum: a geometria emissiva e
        // estatica, entao o Record so faz trabalho no primeiro frame apos carregar a cena.
        if (MeshLights.IsReady()) {
            FGpuScope Scope(GpuProfiler, CommandList, "MeshLights (extract)");
            MeshLights.Record(CommandList, SRVHeap);
        }

        const bool ReGIROn = Settings().ReGIRActive() && HasReGIRConsumer && GILightCount > 0;
        // O gate real do ReGIR e este, e nao o toggle: sem consumidor ou sem luz na cena a grade
        // nem chega a ser construida. O manifesto da captura le daqui — reconstituir a condicao
        // por fora e o comeco de uma divergencia.
        ReGIRRanThisFrame     = ReGIROn;
        GILightCountThisFrame = GILightCount;
        if (ReGIROn) {
            FGpuScope Scope(GpuProfiler, CommandList, "ReGIR (build)");
            ReGIR.UpdatePerFrame(FrameSlot, TemporalSampleIndex, GILightCount, GILightSetSignature);
            ReGIR.RecordBuild(CommandList, SRVHeap);
        }
        const FReGIRShaderParams ReGIRCB = ReGIR.ShaderParams(ReGIROn);
        DDGI.SetReGIRParams(ReGIRCB);
        Reflections.SetReGIRParams(ReGIRCB);
        ReSTIRGI.SetReGIRParams(ReGIRCB);

        // World radiance cache — mesmo padrao do ReGIR acima. A camera entra porque o nivel do
        // hash sai da distancia ate ela.
        RadianceCache.UpdatePerFrame(FrameSlot, Vw.CameraPosition);
        RadianceCache.SetReGIRParams(ReGIRCB);
        // QUEM ESCREVE NA TABELA. Com o produtor dedicado da Fase 3 escolhido, ninguem do render
        // escreve: os traces voltam a so consultar e a fonte passa a ser o passe proprio, que nao
        // le DDGI. Sem isso, o cache continuaria aprendendo o sinal que esta serie veio trocar.
        //
        // O gate e a POLITICA (`GetDedicatedUpdate`), e nao "o passe vai rodar de fato": se o
        // produtor dedicado esta escolhido mas as tabelas dele nao subiram, o certo e a ocupacao
        // ficar parada e denunciar o problema — cair no produtor antigo em silencio devolveria o
        // sinal do DDGI por baixo, que e o pior desfecho possivel para uma medicao.
        const bool LegacyProducer = !RadianceCache.GetDedicatedUpdate();
        // O segundo argumento e "este consumidor vai tracar neste frame". Os params sao montados
        // para os tres de qualquer jeito (custa nada, e o passe pode nem rodar), mas so quem roda
        // entra no registro que o manifesto le — senao a captura afirmaria cache ativo num frame
        // em que nenhum trace o consultou.
        const bool DDGIWillTrace = UseGI && DDGI.IsReady();
        DDGI.SetRadianceCacheParams(RadianceCache.ShaderParams(LegacyProducer, DDGIWillTrace));
        ReSTIRGI.SetRadianceCacheParams(
            RadianceCache.ShaderParams(LegacyProducer, Modes.ReSTIRGIActive));
        Reflections.SetRadianceCacheParams(
            RadianceCache.ShaderParams(false, Modes.ReflectionsActive));
        // Os tres buffers precisam estar em UAV antes de QUALQUER trace: a escrita e a leitura
        // acontecem dentro do ShadeSurfaceHit, que os cinco shaders de trace compartilham.
        RadianceCache.TransitionForTrace(CommandList);

        // Sky-view LUT: os tres passes de trace sombreiam raio que escapa lendo o MESMO LUT que
        // o raster, entao precisam da mesma parameterizacao. O view height e o raio do planeta
        // saem do FAtmosphere (fonte unica) — antes eram literais no HitShading.hlsli, e o de
        // view height estava 0,5 km acima do valor do bake. Empurrado todo frame porque o view
        // height segue a altitude da camera.
        const f32 SkyViewHeightKm = Atmosphere.ViewHeightKm();
        const f32 SkyBottomRKm    = Atmosphere.BottomRadiusKm();
        DDGI.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        Reflections.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        ReSTIRGI.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        RadianceCache.SetSkyParams(SkyViewHeightKm, SkyBottomRKm);
        DDGI.SetSkyIntensity(Lt.RainSkyDim); // reflexoes e ReSTIR recebem no proprio UpdatePerFrame

        if (Modes.ReliableMotionActive) {
            TemporalMotion.UpdatePerFrame(FrameSlot, Scene, Vw.InvViewProjFull,
                                          Vw.ViewProjUnjittered, PrevViewProj,
                                          Vw.CameraPosition);
        } else {
            // O historico de superficie nao e atualizado enquanto nenhum consumidor existe;
            // ao religar, o primeiro frame deve cair no velocity raster em vez de ler pose velha.
            TemporalMotion.InvalidateHistory();
        }

        GIComputeFence = 0;
        if (UseGI && DDGI.IsReady()) {
            DDGI.SetPunctualLightsSRV(Device.Native(), SRVHeap, GILightSRVSlot[FrameSlot], FrameSlot);
            DDGI.UpdatePerFrame(FrameSlot, Lt.KeyDir, Lt.KeyInt, Lt.KeyColor, TemporalSampleIndex, GILightCount);
            if (UseAsyncCompute && DDGI.CanRunAsync()) {
                DDGI.TransitionForUpdate(CommandList);
                const u64 S1 = CommandQueue.SubmitSegmentAndContinue();

                ID3D12GraphicsCommandList* CCL = ComputeQueue.Begin();
                GpuProfilerCompute.BeginFrame(ComputeQueue.CurrentSlot());
                ID3D12DescriptorHeap* CHeaps[] = { SRVHeap.Native() };
                CCL->SetDescriptorHeaps(_countof(CHeaps), CHeaps);
                {
                    FGpuScope Scope(GpuProfilerCompute, CCL, "DDGI (async)");
                    DDGI.RecordUpdate(CCL, SRVHeap);
                }
                GpuProfilerCompute.Resolve(CCL);
                GIComputeFence = ComputeQueue.SubmitAfter(CommandQueue.NativeFence(), S1);

                ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
            } else {
                FGpuScope Scope(GpuProfiler, CommandList, "DDGI");
                DDGI.TransitionForUpdate(CommandList);
                DDGI.RecordUpdate(CommandList, SRVHeap);
                DDGI.TransitionForRead(CommandList);
            }
        }

        if (Modes.ReflectionsActive) {
            Reflections.SetPunctualLightsSRV(Device.Native(), SRVHeap, GILightSRVSlot[FrameSlot], FrameSlot);
            const f32 ReflSkyIntensity = Lt.RainSkyDim;
            const bool WaterUsesAtmosphere = UseAtmosphereSky && Atmosphere.IsInitialized();
            const f32 WaterEnvironmentIntensity =
                WaterUsesAtmosphere ? ReflSkyIntensity : IBLIntensity;
            Reflections.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.ViewProjection, PrevViewProj,
                                       Vw.CameraPosition, PrevCameraPosition,
                                       RenderWidth(), RenderHeight(), Lt.KeyDir, Lt.KeyInt,
                                       Lt.KeyColor, TemporalSampleIndex, ReflSkyIntensity, Vw.View,
                                       WaterUsesAtmosphere, WaterEnvironmentIntensity, GILightCount,
                                       TemporalMotion.InstanceCount(), Modes.MotionHistoryValidThisFrame);
        }

        if (Modes.ReSTIRGIActive) {
            ReSTIRGI.SetPunctualLightsSRV(Device.Native(), SRVHeap, GILightSRVSlot[FrameSlot]);
            // O x1 do reuso temporal vem do historico de superficie do frame ANTERIOR (o reservoir
            // deixou de guardar o ponto visivel). Mesma convencao do ReSTIR DI e das reflexoes:
            // FrameSlot e o corrente, 1 - FrameSlot e o anterior. Sem motion confiavel neste frame
            // o slot vai invalido e o FReSTIRGI derruba so o reuso temporal.
            const u32 PrevSurfaceSlot = (Modes.ReliableMotionActive && Modes.MotionHistoryValidThisFrame)
                                      ? TemporalMotion.SurfaceSRV(1u - FrameSlot) : 0xFFFFFFFFu;
            ReSTIRGI.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.CameraPosition,
                                    RenderWidth(), RenderHeight(), Lt.KeyDir, Lt.KeyInt, Lt.KeyColor,
                                    TemporalSampleIndex, Lt.RainSkyDim, Vw.View,
                                    PrevJitterUv - Vw.JitterUv, PrevSurfaceSlot, GILightCount);
        }

        // Passe dedicado de update do cache (Fase 3). Preenchido AQUI, onde luzes e sol ja estao
        // resolvidos; gravado depois do G-buffer, que e de onde saem as origens dos caminhos.
        //
        // A mask de sombra sai do ReSTIR GI de proposito, em vez de virar knob proprio: o cache
        // ALIMENTA o ReSTIR GI, e se um contasse a folhagem nas sombras e o outro nao, a mesma
        // superficie teria duas radiancias conforme o caminho — e o A/B do toggle mediria a
        // divergencia entre os dois, nao o efeito da folhagem.
        if (Modes.RadianceCacheUpdateActive) {
            RadianceCache.SetUpdatePunctualLightsSRV(Device.Native(), SRVHeap,
                                                     GILightSRVSlot[FrameSlot], FrameSlot);
            RadianceCache.UpdatePassPerFrame(
                FrameSlot, Vw.InvViewProjFull, Vw.CameraPosition,
                Lt.KeyDir, Lt.KeyInt, Lt.KeyColor,
                ReSTIRGI.GetFoliageShadows() ? kRTMaskShadowFull : kRTMaskShadowFast,
                TemporalSampleIndex, Lt.RainSkyDim, DDGI.MaxRayDistance(), GILightCount,
                ReSTIRGI.GetBackfacePolicy());
        }
    }

    // Reconstrucao temporal: TAA proprio, ou o dispatch do upscaler (FSR / DLSS-SR / DLSS-RR).
    // Sao exclusivos entre si. Devolve a imagem que o pos-processamento deve consumir — sem
    // upscale ela e o proprio HDR da cena, com upscale e o alvo em resolucao de display.
    //
    // RRPoisoned vem de fora porque quem sabe se o HDR foi contaminado e a fase de debug: o
    // visualizador sobrescreve o buffer e o overlay de sondas desenha por cima, e o eval do RR
    // tem de ser pulado nesse caso (ver a nota longa la).
    FPostInput Renderer::RecordResolve(FPassContext& _Ctx, IUpscaler* _ActiveUp, bool _RRPoisoned) {
        auto* CommandList        = _Ctx.Cmd;
        const FFrameModes& Modes = *_Ctx.Modes;
        const FFrameView& Vw     = *_Ctx.View;
        const u32 FrameSlot      = _Ctx.FrameSlot;
        IUpscaler* ActiveUp      = _ActiveUp;
        const bool RRPoisoned    = _RRPoisoned;

        ID3D12Resource* PostInput    = Targets.HDRColorBuffer.Get();
        u32             PostInputSRV = Targets.HDRSRVSlot;
        if (Modes.TAAActive) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "TAA");
                TemporalAA.Execute(CommandList, SRVHeap, FrameSlot, Vw.InvViewProjUnjit, PrevViewProj,
                                   TAAHistoryBlend, TAARanLastFrame, TAAVarianceGamma, TAASharpness,
                                   TAAMotionBlend, TAAAntiFlicker, TAAStationaryMargin, Vw.CameraPosition,
                                   Vw.NearZ, Vw.FarZ, TAADebugMode);
            }

            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);

            PostInput    = TemporalAA.DisplayOutputResource();
            PostInputSRV = TemporalAA.DisplayOutputSRVSlot();
            TAARanLastFrame = true;
        } else if (Modes.UpscaleActive && !RRPoisoned) {
            // Modes.RRMode: o passe ativo e o proprio RR (ActiveUp == &DlssRR); ele denoisa a cor RUIDOSA
            // (GI+reflexao pre-denoise) e faz o upscale num eval so, guiado pelos buffers de material.
            // MESMO predicado do bloco de sinal (Modes.RRMode, ja em escopo): recalcular so pelo Denoiser
            // divergia dele — os guides podiam nao estar prontos e este bloco tentava o eval do RR
            // enquanto o GI/reflexao ja tinham sido configurados como crus.
            const bool IsRR = Modes.RRMode;

            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Transition(Targets.VelocityBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.UpscaleReactiveMask.Get(), Targets.UpscaleReactiveState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.UpscaleCompositionMask.Get(), Targets.UpscaleCompositionState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (IsRR) GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            if (IsRR) {
                // Guides de material do RR (albedo difuso/especular, normal-roughness) do G-buffer
                // [A,B,C,Depth] (mesma tabela contigua do deferred) + FrameCB. O specHitDist ja foi
                // extraido no bloco da reflexao (ou fica zerado sem reflexoes). Deixa tudo em NON_PIXEL.
                FGpuScope Scope(GpuProfiler, CommandList, "DLSS-RR guides");
                RRGuides.RecordGuides(CommandList, SRVHeap, SRVHeap.GpuHandle(GBuffer.SRVTableStart()),
                                      _Ctx.FrameCB);
                if (!Modes.ReflectionsActive) RRGuides.ClearSpecHitDist(CommandList, SRVHeap);
                RRGuides.TransitionForRR(CommandList);
            }

            FUpscaleParams UpParams{};
            UpParams.Color        = Targets.HDRColorBuffer.Get();
            UpParams.Depth        = Targets.DepthBuffer.Get();
            UpParams.Velocity     = Targets.VelocityBuffer.Get();
            UpParams.Reactive     = Targets.UpscaleReactiveMask.Get();
            UpParams.TransparencyAndComposition = Targets.UpscaleCompositionMask.Get();
            UpParams.JitterX      = Vw.JitterPxX;
            UpParams.JitterY      = Vw.JitterPxY;
            UpParams.NearZ        = Vw.NearZ;
            UpParams.FarZ         = Vw.FarZ;
            UpParams.FovYRadians  = Vw.FovY;
            UpParams.AspectRatio  = Vw.Aspect;
            UpParams.DeltaTimeSec = LastDeltaTime;
            UpParams.Quality      = UpscalerQuality;
            // Reset one-shot: descarta o historico temporal em troca de modo/denoiser/scene/resize (senao o
            // RR/DLSS reusa acumulacao velha => ghosting). Limpo logo apos o Dispatch.
            UpParams.Reset        = RRResetPending;
            // Matrizes p/ o DLSS (o FSR ignora): projecao unjittered + reprojecao (clip atual -> anterior).
            // PrevViewProj ainda guarda o frame anterior aqui (so e atualizado logo abaixo).
            UpParams.ViewToClip     = Vw.ProjUnjittered;
            UpParams.ClipToPrevClip = Vw.ViewProjUnjittered.Inverse() * PrevViewProj;
            {
                const Mat44 InvView = Vw.View.Inverse();   // view->world: linhas = base da camera em mundo
                UpParams.CamRight = { InvView.M[0][0], InvView.M[0][1], InvView.M[0][2] };
                UpParams.CamUp    = { InvView.M[1][0], InvView.M[1][1], InvView.M[1][2] };
                UpParams.CamFwd   = { InvView.M[2][0], InvView.M[2][1], InvView.M[2][2] };
                UpParams.CamPos   = { InvView.M[3][0], InvView.M[3][1], InvView.M[3][2] };
            }
            if (IsRR) {
                // Guides que so o RR consome (o FSR/DLSS-SR ignoram). WorldToView (row-major, sem jitter)
                // = Vw.View: o RR deriva os specular motion vectors do specHitDist + matrizes world<->view.
                UpParams.DiffuseAlbedo   = RRGuides.DiffuseAlbedo();
                UpParams.SpecularAlbedo  = RRGuides.SpecularAlbedo();
                UpParams.NormalRoughness = RRGuides.NormalRoughness();
                UpParams.SpecHitDist     = RRGuides.SpecHitDist();
                UpParams.WorldToView     = Vw.View;
            }

            {
                FGpuScope Scope(GpuProfiler, CommandList,
                                IsRR ? "DLSS-RR" : (Upscaler == EUpscaler::DLSS ? "DLSS-SR" : "FSR"));
                ActiveUp->Dispatch(CommandList, UpParams);
            }
            RRResetPending = false;   // reset consumido

            // Manual hooking (eDisableCLStateTracking): o SL pode ter mexido no estado do CL e nao tem
            // proxy p/ restaurar. O ProgrammingGuideManualHooking.md §7.1 lista o que o SL restauraria:
            // descriptor heaps, compute root signature + TODAS as bindings de compute (tabelas/CBV/SRV/
            // UAV/constants), PSO e state object. Aqui rebindamos SO os heaps de proposito: auditado, todo
            // consumidor a jusante seta root signature E PSO proprios antes de usar o CL — Flicker
            // (FlickerHeatmap.cpp:195), PostProcessor (PostProcess.cpp:284), SelectionOutline (:272/:322)
            // e DebugDraw (:241) —, e o CL e resetado por frame, entao nada herda estado do eval.
            // Os heaps ficam porque sao o unico estado que o SL troca e ninguem reestabelece.
            // ATENCAO: um passe futuro que dependa de estado herdado aqui exige revisitar isto.
            // (o FSR/ffx-api tolera o rebind redundante).
            {
                ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            }

            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Transition(Targets.VelocityBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (IsRR) GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            PostInput    = ActiveUp->OutputResource();
            PostInputSRV = ActiveUp->OutputSRVSlot();
            TAARanLastFrame = false;
            RRSkipLogged    = false; // voltou a avaliar: rearma o aviso p/ a proxima vez
        } else {
            // Eval pulado por debug na cena: arma o reset p/ o proximo eval real nao reprojetar de
            // um historico interrompido (o RR ficou N frames sem ver a cena).
            if (RRPoisoned) {
                RRResetPending = true;
                if (!RRSkipLogged) {
                    LogWarning("Ray Reconstruction pausado: ha debug desenhado na cena "
                               "(visualizador de alvo ou sondas do DDGI). A imagem sai em resolucao "
                               "de render ate o debug sair — desligue-o p/ voltar a avaliar o RR");
                    RRSkipLogged = true;
                }
            }
            TAARanLastFrame = false;
        }
        return { PostInput, PostInputSRV };
    }

    // Heatmap de flicker (opcional, troca a imagem), tonemap para o backbuffer, contorno da
    // selecao e a geometria de ferramenta do editor. Fecha o frame no backbuffer.
    void Renderer::RecordPost(FPassContext& _Ctx, FPostInput _In) {
        auto* CommandList     = _Ctx.Cmd;
        const FFrameView& Vw  = *_Ctx.View;
        const u32 FrameSlot   = _Ctx.FrameSlot;
        ID3D12Resource* PostInput = _In.Texture;
        u32 PostInputSRV          = _In.SRVSlot;
        const FSelectionDraw& Sel = _Ctx.Selection;

        if (FlickerMode > 0 && Flicker.IsInitialized()) {
            Flicker.Execute(CommandList, SRVHeap, PostInputSRV, static_cast<f32>(FlickerMode),
                            FlickerScale, FlickerAlpha, FlickerResetPending,
                            RenderWidth(), RenderHeight());
            FlickerResetPending = false;
            PostInput    = Flicker.OutputResource();
            PostInputSRV = Flicker.OutputSRVSlot();
        }

        FBarrierBatch BackBatch;
        BackBatch.Transition(SwapChain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        BackBatch.Flush(CommandList);

        {
            FGpuScope Scope(GpuProfiler, CommandList, "Pós (bloom+tonemap)");
            PostProcessor.Execute(CommandList, SRVHeap, PostInput, SwapChain.CurrentRTV(),
                                  PostInputSRV, FrameSlot, SwapChain.GetWidth(), SwapChain.GetHeight());
        }

        // Captura: DEPOIS do tonemap e ANTES dos overlays. O contorno de selecao e os gizmos que
        // vem abaixo sao a ferramenta, nao a imagem — um PNG de regressao com a seta do gizmo
        // atravessada muda pixels que nao tem nada a ver com o estimador.
        if (Capture.ShouldShoot()) {
            Capture.RecordCopy(Device.Native(), CommandList, SwapChain.CurrentBackBuffer(),
                               SwapChain.GetWidth(), SwapChain.GetHeight());
            // Aqui tambem estamos depois do RecordResolve do cache, entao esta copia pega a
            // ocupacao que a varredura DESTE frame escreveu — a do anel, feita antes do dispatch,
            // ainda e do frame anterior.
            RadianceCache.RecordStatsCopy(CommandList);
        }

        if (Sel.Renderable >= 0 && Sel.Slot != FSelectionDraw::kNoSlot && Sel.Mesh
            && SelectionOutline.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Contorno da seleção");
            FSelectionOutline::FDrawItem Item{ Sel.Mesh, Sel.Model * Vw.ViewProjUnjittered };
            SelectionOutline.RecordMask(CommandList, &Item, 1, FrameSlot);
            auto BackRTV = SwapChain.CurrentRTV();
            SelectionOutline.RecordOutline(CommandList, SRVHeap, BackRTV,
                                           SwapChain.GetWidth(), SwapChain.GetHeight(), FrameSlot);
        }

        if (DebugDraw.IsInitialized() && !DebugDraw.Empty()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Debug draw (gizmo/ícones)");
            const bool WantDepth = DebugDraw.NeedsSceneDepth() && Targets.DepthSRVSlot != kInvalidSlot;
            FBarrierBatch Batch;
            if (WantDepth) {
                Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Batch.Flush(CommandList);
            }
            // Eixos da camera em mundo (colunas da view row-vector) p/ os billboards de icone.
            const Vec3 CamRight{ Vw.View.M[0][0], Vw.View.M[1][0], Vw.View.M[2][0] };
            const Vec3 CamUp   { Vw.View.M[0][1], Vw.View.M[1][1], Vw.View.M[2][1] };
            DebugDraw.Render(CommandList, FrameSlot, Vw.ViewProjUnjittered, SwapChain.CurrentRTV(),
                             SwapChain.GetWidth(), SwapChain.GetHeight(), CamRight, CamUp,
                             WantDepth ? SRVHeap.GpuHandle(Targets.DepthSRVSlot)
                                       : D3D12_GPU_DESCRIPTOR_HANDLE{});
            if (WantDepth) {
                Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
                Batch.Flush(CommandList);
            }
        }
        // (o Clear e da FDebugDrawClearGuard no topo do metodo)

        BackBatch.Transition(SwapChain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT);
        BackBatch.Flush(CommandList);
    }

    // Fog de altura + perspectiva aerea, sun shafts e a cortina/gotas de chuva. Tudo isto
    // compoe SOBRE o HDR ja iluminado, lendo o depth da cena — por isso vem depois do forward
    // e antes de qualquer resolve temporal.
    void Renderer::RecordVolumetricsAndRain(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;

        if ((UseHeightFog || UseAerialPerspective) && Fog.IsInitialized()) {
            const D3D12_RESOURCE_STATES FogDepthRead =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, FogDepthRead);
            Batch.Flush(CommandList);

            const bool VolFogOn = UseVolumetricFog && UseHeightFog && VolumetricFog.IsInitialized();
            if (VolFogOn) {
                FGpuScope Scope(GpuProfiler, CommandList, "Volumetric fog");
                SunShadows.EnsureReadableCompute(CommandList);
                LocalShadows.EnsureReadableCompute(CommandList);
                VolumetricFog.Execute(CommandList, SRVHeap, SunShadows.ConstantsAddress(),
                                      SunShadows.ShadowSRVSlot(),
                                      (UseGI && DDGI.IsReady()) ? DDGI.IrradianceAtlasSRV()
                                                                : Targets.DepthSRVSlot,
                                      LightBuffer->GetGPUVirtualAddress() +
                                          static_cast<u64>(FrameSlot) * kMaxLights * sizeof(FGPULight),
                                      LocalShadows.ShadowSRVSlot(),
                                      VolumetricClouds.IsInitialized()
                                          ? VolumetricClouds.ShadowSRV() : Targets.DepthSRVSlot,
                                      Targets.DepthSRVSlot);
            }

            const bool VolShaftsOn = UseSunShafts && SunShafts.IsInitialized() && UseHeightFog;
            if (VolShaftsOn) {
                FGpuScope Scope(GpuProfiler, CommandList, "Sun shafts");
                SunShadows.EnsureReadable(CommandList);
                SunShafts.RecordVolumetric(CommandList, SRVHeap, Targets.DepthSRVSlot,
                                           SunShadows.ConstantsAddress(),
                                           SunShadows.ShadowSRVSlot(),
                                           VolumetricClouds.IsInitialized()
                                               ? VolumetricClouds.ShadowSRV()
                                               : Targets.DepthSRVSlot);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
            }

            GpuProfiler.Begin(CommandList, "Fog");
            auto Fog_RTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &Fog_RTV, FALSE, nullptr);
            Fog.Execute(CommandList, SRVHeap, Targets.DepthSRVSlot, Atmosphere.AerialVolumeSRV(),
                        SunShafts.IsInitialized() ? SunShafts.VolumetricSRVSlot()
                                                  : Targets.DepthSRVSlot,
                        VolFogOn ? VolumetricFog.IntegratedSRVSlot() : Targets.DepthSRVSlot,
                        Atmosphere.IsInitialized() ? Atmosphere.SkyViewSRV() : Targets.DepthSRVSlot);
            GpuProfiler.End(CommandList); // Fog

            Batch.Transition(Targets.DepthBuffer.Get(), FogDepthRead,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (Weather.Raining() && RainWetness.IsInitialized() &&
            (Weather.GetCurtainAmount() > 0.001f || Weather.GetRainParticles())) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            GpuProfiler.Begin(CommandList, "Chuva — cortina/gotas");
            auto RainRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &RainRTV, FALSE, nullptr);
            if (Weather.GetCurtainAmount() > 0.001f)
                RainWetness.ExecuteCurtain(CommandList, SRVHeap, Targets.DepthSRVSlot,
                                           RenderWidth(), RenderHeight());
            if (Weather.GetRainParticles())
                RainWetness.ExecuteParticles(CommandList, SRVHeap, Targets.DepthSRVSlot,
                                             RenderWidth(), RenderHeight());
            GpuProfiler.End(CommandList); // Chuva — cortina/gotas

            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

    }

    // Visualizadores: alvo unico em fullscreen, a grade offscreen da janela de debug, o debug da
    // BVH e o heatmap de flicker. Devolve se o HDR foi ENVENENADO para o Ray Reconstruction —
    // conteudo que nao e a cena entrando no buffer que a rede consome (ver a nota interna).
    bool Renderer::RecordDebugViews(FPassContext& _Ctx, bool _DDGIDebugDrew) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const bool DDGIDebugDrew       = _DDGIDebugDrew;

        // independente: ela e composta num target offscreen e copiada para o QML.
        const auto& DbgAll = DebugTargets::All();
        const bool MainDebugActive = GBufferDebugMode > 0 || DebugTargetIndex < DbgAll.size();
        const bool PreviewActive   = DebugPreviewEnabled && !DebugSelection.empty();
        // A janela e uma ferramenta interativa: capturar so 1/3 dos frames fazia o jitter
        // temporal aparecer como tremedeira e deixava o movimento visivelmente defasado.
        const bool CapturePreview  = PreviewActive;

        // Conteudo que NAO e a cena entrando no Targets.HDRColorBuffer envenena a ENTRADA do Ray
        // Reconstruction: o visualizador fullscreen SOBRESCREVE o buffer com falsa cor, e o overlay
        // de sondas do DDGI desenha geometria de ferramenta por cima. Em qualquer dos dois a rede
        // reconstruiria isso guiada pelos buffers de material da cena real — lixo, e ainda contamina
        // o historico temporal dela. Enquanto durar, o eval do RR e PULADO (ver o bloco do upscale).
        // Por que pular em vez de mover o debug p/ depois do RR: a OUT do RR so tem UAV (sem RTV,
        // DlssRRPass.cpp), o FDebugView e instanciado POR FORMATO de render target, e o overlay do
        // DDGI precisa de depth por hardware na resolucao de render, que nao existe mais depois do
        // upscale. Pulando, o debug continua no caminho de cor de sempre (HDR -> tonemap); a unica
        // diferenca e que sai em resolucao de render, o que p/ um visualizador e irrelevante.
        // Espirito do GPU Zen 3 cap. 7 §7.13.3: nao entregar ao denoiser o que ele nao sabe ler.
        const bool RRPoisoned = Modes.RRMode && (MainDebugActive || DDGIDebugDrew);

        if ((MainDebugActive || CapturePreview) &&
            GBuffer.IsInitialized() && DebugViewPass.IsInitialized()) {
            GBuffer.TransitionToRead(CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Os alvos de timer sao os unicos publicados que saem do frame em UAV; o visualizador
            // nao emite barreira por conta propria (ver o comentario no fim do RecordTrace do
            // ReSTIR GI). Sem captura ativa eles ficam no estado de leitura e isto e no-op.
            TimerGI.ToRead(CommandList);
            TimerReflections.ToRead(CommandList);
            BvhDebug.ToRead(CommandList); // idem: sai do dispatch em UAV
            // Guides do RR: saem do frame anterior em NON_PIXEL (TransitionForRR) e aqui vao ser
            // lidos por um passe GRAFICO. Sem restore: o TransitionForRR, mais abaixo no mesmo
            // frame, devolve todos a NON_PIXEL — os estados sao rastreados dentro da classe.
            if (RRGuides.IsReady()) RRGuides.TransitionForDebug(CommandList);

            // Depth e normal podem ser escolhidos tanto no toolbar quanto na janela. Deixa-os
            // legiveis durante os dois draws e restaura exatamente os estados de entrada.
            const bool NeedPublishedInputs =
                CapturePreview || DebugTargetIndex < DbgAll.size();
            const D3D12_RESOURCE_STATES NormalBefore = Targets.NormalBufferState;
            if (NeedPublishedInputs) {
                FBarrierBatch InputBatch;
                InputBatch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                InputBatch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                InputBatch.Flush(CommandList);
            }

            if (CapturePreview && DebugPreviewPass.IsInitialized() && DebugPreviewTarget) {
                FDebugTile PreviewTiles[16]{};
                u32 PreviewTileCount = 0;
                for (u32 Idx : DebugSelection) {
                    if (PreviewTileCount >= 16u || Idx >= DbgAll.size()) continue;
                    const FDebugTarget& T = DbgAll[Idx];
                    FDebugTile& Tile   = PreviewTiles[PreviewTileCount++];
                    Tile.SrvSlot       = T.SrvSlot;
                    Tile.Decode        = T.Decode;
                    Tile.SubIndex      = T.SubIndex;
                    // Escala do heatmap de distancia. A registrada e GLOBAL e sai da cascata
                    // GROSSA, porque a vista do atlas inteiro mistura tiles de todas. Quando UM
                    // tile e inspecionado, a escala vira a DAQUELA cascata: no overview a fina
                    // fica escura mas legivel, e no tile ampliado ela usa a propria regua.
                    f32 ExposureScale = 1.0f;
                    if (DebugProbeIndex != kNoDebugProbe && T.AtlasTilePx > 0 &&
                        T.Name.rfind("DDGI", 0) == 0) {
                        // Zero significa overview; +1 permite selecionar fisicamente o tile 0.
                        Tile.SubIndex = DDGI.AtlasTileFromProbe(DebugProbeIndex) + 1u;
                        if (T.Decode == EDebugDecode::DDGIDistance && DDGI.ProbesPerCascade() > 0) {
                            const u32 Casc = DebugProbeIndex / DDGI.ProbesPerCascade();
                            const f32 TileMax = DDGI.CascadeDistanceMomentMax(Casc);
                            const f32 GlobalMax = DDGI.DistanceMomentMax();
                            if (TileMax > 0.0f) ExposureScale = GlobalMax / TileMax;
                        }
                    }
                    Tile.AtlasTilePx   = T.AtlasTilePx;
                    Tile.Mip           = DebugMip < T.MipCount ? DebugMip : 0;
                    Tile.ChannelWeight = DebugChannelWeight;
                    Tile.Exposure      = T.Exposure * DebugExposure * ExposureScale;
                    Tile.NearZ         = Vw.NearZ;
                    Tile.FarZ          = Vw.FarZ;
                    Tile.LinearFilter  = T.LinearFilter;
                }

                auto PreviewRTV = DebugPreviewRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &PreviewRTV, FALSE, nullptr);
                const f32 Clear[4] = { 0.035f, 0.037f, 0.031f, 1.0f };
                CommandList->ClearRenderTargetView(PreviewRTV, Clear, 0, nullptr);

                D3D12_VIEWPORT PreviewViewport{
                    0.0f, 0.0f,
                    static_cast<f32>(kDebugPreviewWidth),
                    static_cast<f32>(kDebugPreviewHeight),
                    0.0f, 1.0f
                };
                DebugPreviewPass.Execute(
                    CommandList, SRVHeap, PreviewTiles, PreviewTileCount, DebugColumns,
                    GBuffer.SRVTableStart(), Targets.VelocitySRVSlot, PreviewViewport,
                    Vw.JitterUv, /*EncodeForDisplay=*/true);

                D3D12_RESOURCE_BARRIER ToCopy{};
                ToCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                ToCopy.Transition.pResource   = DebugPreviewTarget.Get();
                ToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                ToCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                CommandList->ResourceBarrier(1, &ToCopy);

                D3D12_TEXTURE_COPY_LOCATION Src{};
                Src.pResource        = DebugPreviewTarget.Get();
                Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                Src.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION Dst{};
                Dst.pResource = DebugPreviewReadback[FrameSlot].Get();
                Dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                Dst.PlacedFootprint.Offset             = 0;
                Dst.PlacedFootprint.Footprint.Format   = kDebugPreviewFormat;
                Dst.PlacedFootprint.Footprint.Width    = kDebugPreviewWidth;
                Dst.PlacedFootprint.Footprint.Height   = kDebugPreviewHeight;
                Dst.PlacedFootprint.Footprint.Depth    = 1;
                Dst.PlacedFootprint.Footprint.RowPitch = kDebugPreviewRowPitch;
                CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

                std::swap(ToCopy.Transition.StateBefore, ToCopy.Transition.StateAfter);
                CommandList->ResourceBarrier(1, &ToCopy);
                DebugPreviewReadbackPending[FrameSlot] = true;
                DebugPreviewReadbackVersion[FrameSlot] = DebugPreviewConfigVersion;
                DebugPreviewLastCapturedVersion = DebugPreviewConfigVersion;

                if (DebugProbeIndex != kNoDebugProbe && DDGI.IsReady() &&
                    DebugProbeSampleU >= 0.0f && DebugProbeSampleV >= 0.0f) {
                    ID3D12Resource* IrrResource  = DDGI.IrradianceAtlasResource();
                    ID3D12Resource* DistResource = DDGI.DistanceAtlasResource();
                    constexpr D3D12_RESOURCE_STATES AtlasRead =
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    D3D12_RESOURCE_BARRIER AtlasBarriers[2]{};
                    ID3D12Resource* Resources[2] = { IrrResource, DistResource };
                    for (u32 I = 0; I < 2; ++I) {
                        AtlasBarriers[I].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        AtlasBarriers[I].Transition.pResource = Resources[I];
                        AtlasBarriers[I].Transition.Subresource =
                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        AtlasBarriers[I].Transition.StateBefore = AtlasRead;
                        AtlasBarriers[I].Transition.StateAfter =
                            D3D12_RESOURCE_STATE_COPY_SOURCE;
                    }
                    CommandList->ResourceBarrier(2, AtlasBarriers);

                    // Tiles por linha vem do DDGI, e nao de CountX*CountZ: com o empacotamento 2D
                    // a largura do atlas deixou de ser um par de eixos do grid (ver
                    // DDGI_TileOrigin). O shader do DebugView deriva o mesmo numero da largura da
                    // textura, entao os dois continuam concordando.
                    const u32 TilesX = DDGI.AtlasTilesPerRow();
                    const u32 AtlasTile = DDGI.AtlasTileFromProbe(DebugProbeIndex);
                    const u32 TileX = AtlasTile % std::max(TilesX, 1u);
                    const u32 TileY = AtlasTile / std::max(TilesX, 1u);
                    const u32 IrrLocalX = std::min(
                        static_cast<u32>(DebugProbeSampleU * FDDGI::kTileSize),
                        static_cast<u32>(FDDGI::kTileSize - 1));
                    const u32 IrrLocalY = std::min(
                        static_cast<u32>(DebugProbeSampleV * FDDGI::kTileSize),
                        static_cast<u32>(FDDGI::kTileSize - 1));
                    const u32 DistLocalX = std::min(
                        static_cast<u32>(DebugProbeSampleU * FDDGI::kDistTileSize),
                        static_cast<u32>(FDDGI::kDistTileSize - 1));
                    const u32 DistLocalY = std::min(
                        static_cast<u32>(DebugProbeSampleV * FDDGI::kDistTileSize),
                        static_cast<u32>(FDDGI::kDistTileSize - 1));

                    auto CopyTexel = [&](ID3D12Resource* Resource, DXGI_FORMAT Format,
                                         u32 X, u32 Y, u64 Offset) {
                        D3D12_TEXTURE_COPY_LOCATION SrcLoc{};
                        SrcLoc.pResource = Resource;
                        SrcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        SrcLoc.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION DstLoc{};
                        DstLoc.pResource = DebugProbeSampleReadback[FrameSlot].Get();
                        DstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        DstLoc.PlacedFootprint.Offset = Offset;
                        DstLoc.PlacedFootprint.Footprint.Format = Format;
                        DstLoc.PlacedFootprint.Footprint.Width = 1;
                        DstLoc.PlacedFootprint.Footprint.Height = 1;
                        DstLoc.PlacedFootprint.Footprint.Depth = 1;
                        DstLoc.PlacedFootprint.Footprint.RowPitch =
                            D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
                        const D3D12_BOX Box{ X, Y, 0, X + 1u, Y + 1u, 1 };
                        CommandList->CopyTextureRegion(&DstLoc, 0, 0, 0, &SrcLoc, &Box);
                    };
                    CopyTexel(
                        IrrResource, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        TileX * static_cast<u32>(FDDGI::kTileSize + 2) + 1u + IrrLocalX,
                        TileY * static_cast<u32>(FDDGI::kTileSize + 2) + 1u + IrrLocalY,
                        kDebugProbeIrrOffset);
                    CopyTexel(
                        DistResource, DXGI_FORMAT_R16G16_FLOAT,
                        TileX * static_cast<u32>(FDDGI::kDistTileSize + 2) + 1u + DistLocalX,
                        TileY * static_cast<u32>(FDDGI::kDistTileSize + 2) + 1u + DistLocalY,
                        kDebugProbeDistOffset);

                    for (D3D12_RESOURCE_BARRIER& Barrier : AtlasBarriers)
                        std::swap(Barrier.Transition.StateBefore,
                                  Barrier.Transition.StateAfter);
                    CommandList->ResourceBarrier(2, AtlasBarriers);
                    DebugProbeSamplePending[FrameSlot] = true;
                    DebugProbeSampleVersion[FrameSlot] = DebugPreviewConfigVersion;
                    DebugProbeSampleIndex[FrameSlot]   = DebugProbeIndex;
                }
            }

            if (MainDebugActive) {
                auto HDRDbgRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &HDRDbgRTV, FALSE, nullptr);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);

                FDebugTile Tile{};
                if (DebugTargetIndex < DbgAll.size()) {
                    const FDebugTarget& T = DbgAll[DebugTargetIndex];
                    Tile.SrvSlot       = T.SrvSlot;
                    Tile.Decode        = T.Decode;
                    Tile.SubIndex      = T.SubIndex;
                    Tile.Mip           = DebugMip < T.MipCount ? DebugMip : 0;
                    Tile.AtlasTilePx   = T.AtlasTilePx;
                    Tile.ChannelWeight = DebugChannelWeight;
                    Tile.Exposure      = T.Exposure * DebugExposure;
                    Tile.NearZ         = Vw.NearZ;
                    Tile.FarZ          = Vw.FarZ;
                    Tile.LinearFilter  = T.LinearFilter;
                } else if (GBufferDebugMode == 8) {
                    Tile.SrvSlot = Targets.VelocitySRVSlot;
                    Tile.Decode  = EDebugDecode::Velocity;
                } else {
                    Tile.SrvSlot  = GBuffer.SRVTableStart();
                    Tile.Decode   = EDebugDecode::GBufferField;
                    Tile.SubIndex = GBufferDebugMode;
                }
                DebugViewPass.Execute(CommandList, SRVHeap, &Tile, 1, 1,
                                      GBuffer.SRVTableStart(), Targets.VelocitySRVSlot, Viewport,
                                      Vec2{ 0.0f, 0.0f }, /*EncodeForDisplay=*/false);
            }

            if (NeedPublishedInputs) {
                FBarrierBatch RestoreBatch;
                RestoreBatch.Transition(
                    Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
                RestoreBatch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                               NormalBefore);
                RestoreBatch.Flush(CommandList);
            }
            GBuffer.TransitionToWrite(CommandList);
        }
        return RRPoisoned;
    }

    // Iluminacao da cena: ReSTIR GI e o trace de reflexao, o denoise do indireto, ReSTIR DI com
    // o denoiser dedicado da direta, e o deferred lighting fullscreen que soma tudo sobre o
    // emissivo que o G-buffer ja deixou no HDR.
    void Renderer::RecordSceneLighting(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx; // sites que ja liam Ctx.FrameCB
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        if (Modes.ReSTIRGIActive) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "ReSTIR GI");
                ReSTIRGI.RecordTrace(CommandList, SRVHeap, &GpuProfiler);
            }

            if (Modes.NrdIndirectMode) {
                if (Modes.ReflectionsActive) {
                    FGpuScope Scope(GpuProfiler, CommandList, "Reflexos (trace)");
                    Reflections.RecordTrace(CommandList, SRVHeap, &GpuProfiler);
                }
                GpuProfiler.Begin(CommandList, "NRD denoise");
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "Pack GI + reflexos");
                    Nrd.TransitionInputsToWrite(CommandList);
                    ReSTIRGI.RecordNrdPack(CommandList, SRVHeap);
                    if (Modes.ReflectionsActive) Reflections.RecordNrdPack(CommandList, SRVHeap);
                    else                   Reflections.RecordNrdSpecZero(CommandList, SRVHeap);
                }
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "RELAX indireto");
                    Nrd.SetFrame(Vw.ProjUnjittered, NrdPrevProj, Vw.View, NrdPrevView,
                                 Vw.JitterPx, PrevJitterPx, TemporalSampleIndex);
                    Nrd.Denoise(CommandList);
                }
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "Saida NRD indireta");
                    Nrd.TransitionOutputToRead(CommandList);
                }
                GpuProfiler.End(CommandList); // NRD denoise
                ID3D12DescriptorHeap* ReHeaps[] = { SRVHeap.Native() };
                CommandList->SetDescriptorHeaps(_countof(ReHeaps), ReHeaps);
            }

            // Velocity volta p/ PIXEL porque o TAA a le num passe grafico. Depth e G-buffer NAO
            // voltam p/ DEPTH_WRITE / RENDER_TARGET: ninguem escreve neles daqui ate o deferred
            // logo abaixo, que os quer em leitura combinada. O round-trip custava 2 barreiras por
            // MRT + 2 no depth por frame sem nenhum escritor no meio, e o DEPTH_WRITE ainda podia
            // disparar decompress/resummarize em alguns drivers.
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }

        {
            constexpr D3D12_RESOURCE_STATES DeferredReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            const bool ReSTIRDIOn = Modes.ReSTIRDIActiveFrame;
            // O G-buffer rastreia o proprio estado (AppendTransitions so recebe o alvo); o depth
            // nao, entao o estado de origem depende de o bloco do ReSTIR ter rodado — ele deixa o
            // depth em NON_PIXEL de proposito, em vez de devolver p/ DEPTH_WRITE.
            const D3D12_RESOURCE_STATES DepthBefore =
                Modes.ReSTIRGIActive ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                               : D3D12_RESOURCE_STATE_DEPTH_WRITE;
            FBarrierBatch Batch;
            GBuffer.AppendTransitions(Batch, DeferredReadState);
            Batch.Transition(Targets.DepthBuffer.Get(), DepthBefore, DeferredReadState);
            // O PS deferred nao usa velocity, mas o Pass A do ReSTIR DI usa em compute. So amplie
            // o estado quando esse consumidor existir, evitando uma barreira no caminho legado.
            if (ReSTIRDIOn)
                Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState, DeferredReadState);
            Batch.Flush(CommandList);

            // ReSTIR DI roda ANTES do deferred. DeferredReadState inclui NON_PIXEL, exigido para
            // G-buffer/depth, e o alvo precisa estar pronto quando o PS o somar. Um unico modo
            // manda no dispatch e no gate do shader: divergir dobra ou apaga a luz.
            if (ReSTIRDIOn) {
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "ReSTIR DI");
                    ReSTIRDI.SetRayEpsilons(RayEps);
                    ReSTIRDI.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.View, PrevViewProj,
                                            Vw.CameraPosition,
                                            RenderWidth(), RenderHeight(), TemporalSampleIndex,
                                            FrameLightCount, kRTMaskShadowFull,
                                            /*EnableTemporalPermutation=*/Modes.RRMode,
                                            TemporalMotion.InstanceCount(),
                                            Modes.MotionHistoryValidThisFrame,
                                            MeshLights.LightCount());
                    ReSTIRDI.Record(CommandList, SRVHeap, &GpuProfiler);
                }

                if (Modes.NrdDirectMode) {
                    FGpuScope Scope(GpuProfiler, CommandList, "NRD direta");
                    {
                        FGpuScope ChildScope(GpuProfiler, CommandList, "Pack direto");
                        NrdDirect.TransitionInputsToWrite(CommandList);
                        ReSTIRDI.RecordNrdPack(CommandList, SRVHeap);
                    }
                    {
                        FGpuScope ChildScope(GpuProfiler, CommandList, "RELAX direto");
                        NrdDirect.SetFrame(Vw.ProjUnjittered, NrdPrevProj, Vw.View, NrdPrevView,
                                           Vw.JitterPx, PrevJitterPx, TemporalSampleIndex);
                        NrdDirect.Denoise(CommandList);
                    }
                    {
                        FGpuScope ChildScope(GpuProfiler, CommandList, "Composite direto");
                        NrdDirect.TransitionOutputToRead(CommandList);
                        // O NRD liga seu heap privado; o composite pertence ao heap da engine.
                        ID3D12DescriptorHeap* ReHeaps[] = { SRVHeap.Native() };
                        CommandList->SetDescriptorHeaps(_countof(ReHeaps), ReHeaps);
                        ReSTIRDI.RecordNrdComposite(CommandList, SRVHeap);
                    }
                }

                // Os consumidores posteriores historicamente partem de PIXEL (inclusive o bloco
                // de upscale, que usa barreira explicita). Nao deixe o estado combinado vazar.
                FBarrierBatch RestoreVelocity;
                RestoreVelocity.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                RestoreVelocity.Flush(CommandList);
            }
            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, nullptr); 
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);
            CommandList->SetGraphicsRootDescriptorTable(2, SRVHeap.GpuHandle(GBuffer.SRVTableStart()));
            CommandList->SetGraphicsRootDescriptorTable(3, SRVHeap.GpuHandle(IBLTableStart));
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            {
                const u32 GITable = (UseGI && DDGI.IsReady()) ? DDGI.SceneGITableStart() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(7, SRVHeap.GpuHandle(GITable));
            }
            {
                const u32 AOTable = AO.IsReady() ? AO.AOSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(8, SRVHeap.GpuHandle(AOTable));
            }
            {
                const u32 ReSTIRTable = Modes.ReSTIRGIActive ? ReSTIRGI.GITexSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(9, SRVHeap.GpuHandle(ReSTIRTable));
            }
            {
                // t20. Fallback no IBLTableStart quando inativo — descritor VALIDO, exatamente como
                // o t16 do ReSTIR e o t14 do AO fazem: a root sig exige a tabela ligada mesmo que o
                // shader nao a leia (LightParams.w = 0 fecha a leitura).
                const u32 DirectLocalTable = ReSTIRDIOn ? ReSTIRDI.OutputSRVSlot()
                                                        : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(12, SRVHeap.GpuHandle(DirectLocalTable));
            }
            {
                // t21. Mesmo padrao: descritor VALIDO sempre; AtmoLightParams.w fecha a leitura
                // quando a atmosfera esta off ou o caminho por pixel esta desligado.
                const u32 AtmoTable = Atmosphere.IsInitialized()
                    ? Atmosphere.TransmittanceSRV() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(13, SRVHeap.GpuHandle(AtmoTable));
            }
            CommandList->SetGraphicsRootShaderResourceView(
                10, LightBuffer->GetGPUVirtualAddress() +
                    static_cast<u64>(FrameSlot) * kMaxLights * sizeof(FGPULight));
            {
                const u32 LocalShadowTable = LocalShadows.IsInitialized()
                    ? LocalShadows.ShadowSRVSlot() : IBLTableStart;
                CommandList->SetGraphicsRootDescriptorTable(11, SRVHeap.GpuHandle(LocalShadowTable));
            }
            GpuProfiler.Begin(CommandList, "Deferred lighting");
            // Aditivo (soma sobre o emissivo do geometry pass); nas views de debug SSAO/GI o
            // shader retorna a visualizacao inteira -> PSO opaco p/ substituir a tela.
            const bool DeferredDebugView = AODebug || (UseGI && DDGI.IsReady() && GIDebug);
            CommandList->SetPipelineState(DeferredDebugView
                ? PipelineState.PSODeferredLightingDebug()
                : PipelineState.PSODeferredLighting());
            CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            CommandList->IASetVertexBuffers(0, 0, nullptr);
            CommandList->IASetIndexBuffer(nullptr);
            CommandList->DrawInstanced(3, 1, 0, 0);
            GpuProfiler.End(CommandList); 

            const u32 PointGIFlags =
                (GIChebyshev ? 1u : 0u) |
                (GISkipInactiveProbes ? 2u : 0u) |
                (GISkipInactiveFallback ? 4u : 0u);
            DDGIDebugPass.RecordPointDiagnostic(
                FrameSlot, CommandList, SRVHeap, DDGI,
                Vw.InvViewProjFull, Vw.CameraPosition,
                RenderWidth(), RenderHeight(), PointGIFlags);

            Batch.Transition(Targets.DepthBuffer.Get(), DeferredReadState,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);
        }

        if (ObjectPicker.HasPendingRequest()) {
            {
                std::vector<FObjectPicker::FDrawItem> PickItems;
                PickItems.reserve(VisibleScratch.size());
                for (const FVisibleItem& V : VisibleScratch)
                    PickItems.push_back({ V.R->Mesh,
                                          ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants),
                                          V.SceneIndex + 1 });
                ObjectPicker.RecordIDPass(CommandList, DSV, PickItems.data(), PickItems.size(),
                                          RenderWidth(), RenderHeight());

                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            }
        }

        // Visualizador ANTES do resolve: ele le a tabela como os traces a deixaram neste frame.
        // Depois do resolve mostraria o estado ja envelhecido/despejado, que nao e o que as
        // consultas do frame enxergaram.
        if (Modes.RadianceCacheDebugActive) {
            FGpuScope Scope(GpuProfiler, CommandList, "Radiance cache (debug)");

            // O deferred devolve depth para DEPTH_WRITE e a normal pode estar em RENDER_TARGET
            // quando GTAO esta off. O compute precisa dos dois em NON_PIXEL; restaure exatamente
            // os estados de entrada porque os passes seguintes partem desse contrato.
            const D3D12_RESOURCE_STATES NormalBefore = Targets.NormalBufferState;
            FBarrierBatch Inputs;
            Inputs.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Inputs.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Inputs.Flush(CommandList);

            RadianceCache.RecordDebug(CommandList, SRVHeap, Vw.InvViewProjFull,
                                      Vw.CameraPosition, FrameSlot);

            FBarrierBatch Restore;
            Restore.Transition(Targets.DepthBuffer.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Restore.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                      NormalBefore);
            Restore.Flush(CommandList);
        }

        // Resolve do world radiance cache: DEPOIS de todos os traces do frame, porque e aqui que
        // a acumulacao vira o valor que o proximo frame vai consultar. Rodar antes leria um
        // acumulador vazio e, pior, zeraria o que os traces acabaram de escrever.
        if (RadianceCache.IsActive(Modes)) {
            FGpuScope Scope(GpuProfiler, CommandList, "Radiance cache (resolve)");
            RadianceCache.RecordResolve(CommandList, SRVHeap);
        }
    }

    // Tudo que compoe SOBRE a cena ja iluminada e ainda le/escreve o HDR em resolucao de render:
    // composite das reflexoes, superficie da agua (com as copias de cor/depth e a cadeia de mips
    // antes), translucidos em forward blend e as nuvens volumetricas.
    void Renderer::RecordForwardAndClouds(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        if (Modes.ReflectionsActive) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr); 

            const D3D12_RESOURCE_STATES ReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, ReadState);
            GBuffer.AppendTransitions(Batch, ReadState);
            Batch.Flush(CommandList);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "Reflexos (composite)");
                if (!Modes.NrdIndirectMode) Reflections.RecordTrace(CommandList, SRVHeap);
                // RR: extrai o hitDist especular do Resolved ENQUANTO ele esta NON_PIXEL (o composite
                // cru abaixo o transiciona p/ PIXEL). O RecordTrace acima parou no Resolved (RawSpec).
                if (Modes.RRMode)
                    RRGuides.RecordSpecHitDist(CommandList, SRVHeap,
                                               SRVHeap.GpuHandle(Reflections.GetResolvedSRVSlot()),
                                               Ctx.FrameCB);
                Reflections.RecordComposite(CommandList, SRVHeap, Targets.HDRRTVHeap.CpuHandle(0),
                                            RenderWidth(), RenderHeight());
            }

            Batch.Transition(Targets.DepthBuffer.Get(), ReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);

            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        }

        if (UseWater && Water.IsInitialized() && Modes.WaterSceneCopiesReady) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr); 

            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[0],
                                    D3D12_RESOURCE_STATE_COPY_DEST, 0);
            Batch.TransitionTracked(Targets.SceneDepthCopy.Get(), Targets.SceneDepthCopyState,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
            Batch.Flush(CommandList);

            D3D12_TEXTURE_COPY_LOCATION ColorDst{};
            ColorDst.pResource        = Targets.SceneColorCopy.Get();
            ColorDst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ColorDst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ColorSrc{};
            ColorSrc.pResource        = Targets.HDRColorBuffer.Get();
            ColorSrc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ColorSrc.SubresourceIndex = 0;
            CommandList->CopyTextureRegion(&ColorDst, 0, 0, 0, &ColorSrc, nullptr);
            CommandList->CopyResource(Targets.SceneDepthCopy.Get(), Targets.DepthBuffer.Get());

            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            constexpr D3D12_RESOURCE_STATES SceneCopyRead =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[0],
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
            Batch.TransitionTracked(Targets.SceneDepthCopy.Get(), Targets.SceneDepthCopyState, SceneCopyRead);
            Batch.Flush(CommandList);

            if (Targets.SceneColorMipCount > 1) {
                Targets.SceneColorMipPSO.Bind(CommandList);
                const u32 Constants[8] = {
                    RenderWidth(), RenderHeight(), 0u, 0u, 0u, 0u, 0u, 0u };
                CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

                for (u32 Mip = 1; Mip < Targets.SceneColorMipCount; ++Mip) {
                    Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[Mip],
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, Mip);
                    Batch.Flush(CommandList);

                    CommandList->SetComputeRootDescriptorTable(
                        1, SRVHeap.GpuHandle(Targets.SceneColorMipSRVStart + Mip - 1));
                    CommandList->SetComputeRootDescriptorTable(
                        2, SRVHeap.GpuHandle(Targets.SceneColorMipUAVStart + Mip));
                    const u32 MipWidth  = std::max(1u, RenderWidth() >> Mip);
                    const u32 MipHeight = std::max(1u, RenderHeight() >> Mip);
                    CommandList->Dispatch((MipWidth + 7u) / 8u, (MipHeight + 7u) / 8u, 1);

                    Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[Mip],
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, Mip);
                    Batch.Flush(CommandList);
                }
            }

            // O water base usa a mesma copia para refracao no PS, enquanto o trace de reflexo
            // usa a piramide no compute. Encerrar todos os subrecursos no estado combinado evita
            // uma transicao do recurso inteiro sobre estados divergentes.
            for (u32 Mip = 0; Mip < Targets.SceneColorMipCount; ++Mip)
                Batch.TransitionTracked(Targets.SceneColorCopy.Get(), Targets.SceneColorMipStates[Mip],
                                        SceneCopyRead, Mip);
            Batch.Flush(CommandList);

            auto HDR_RTV_Rebind = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV_Rebind, FALSE, &DSV);
        }

        if (UseWater && Water.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Água — superfície");

            FBarrierBatch WaterBatch;
            WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
            WaterBatch.Flush(CommandList);
            const bool WaterReflectionsActive = Modes.DedicatedWaterReflections;
            const bool ForceFullWaterOutputs = Modes.RRMode || Water.GetGuideInvisible() ||
                Water.GetDebugMode() == FWaterRenderer::EDebugMode::Wireframe;
            FWaterRenderer::EOutputMode WaterOutputMode;
            if (Modes.RRMode) {
                WaterOutputMode = FWaterRenderer::EOutputMode::RayReconstruction;
            } else if (WaterReflectionsActive) {
                WaterOutputMode = FWaterRenderer::EOutputMode::ReflectionGuides;
            } else {
                WaterOutputMode = ForceFullWaterOutputs
                    ? FWaterRenderer::EOutputMode::RayReconstruction
                    : (Modes.UpscaleActive ? FWaterRenderer::EOutputMode::TemporalMasks
                                     : FWaterRenderer::EOutputMode::Base);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE WaterRTVs[8] = {
                Targets.HDRRTVHeap.CpuHandle(0), Targets.VelocityRTVHeap.CpuHandle(0) };
            u32 WaterRTVCount = 2;
            if (WaterOutputMode == FWaterRenderer::EOutputMode::TemporalMasks) {
                WaterRTVs[2] = Targets.UpscaleMaskRTVHeap.CpuHandle(0);
                WaterRTVs[3] = Targets.UpscaleMaskRTVHeap.CpuHandle(1);
                WaterRTVCount = 4;
            } else if (WaterOutputMode == FWaterRenderer::EOutputMode::RayReconstruction) {
                RRGuides.PrepareSpecHitForWater(CommandList);
                WaterRTVs[2] = GBuffer.RTVHandle(0);
                WaterRTVs[3] = GBuffer.RTVHandle(1);
                WaterRTVs[4] = GBuffer.RTVHandle(2);
                WaterRTVs[5] = Targets.UpscaleMaskRTVHeap.CpuHandle(0);
                WaterRTVs[6] = Targets.UpscaleMaskRTVHeap.CpuHandle(1);
                WaterRTVs[7] = RRGuides.SpecHitRTV();
                WaterRTVCount = 8;
            } else if (WaterOutputMode == FWaterRenderer::EOutputMode::ReflectionGuides) {
                WaterRTVs[2] = GBuffer.RTVHandle(0);
                WaterRTVs[3] = GBuffer.RTVHandle(1);
                WaterRTVs[4] = GBuffer.RTVHandle(2);
                WaterRTVs[5] = Targets.UpscaleMaskRTVHeap.CpuHandle(0);
                WaterRTVs[6] = Targets.UpscaleMaskRTVHeap.CpuHandle(1);
                WaterRTVCount = 7;
            }
            CommandList->OMSetRenderTargets(WaterRTVCount, WaterRTVs, FALSE, &DSV);

            const u32 WaterReflCube =
                (UseAtmosphereSky && Atmosphere.IsInitialized())
                    ? Atmosphere.SkyReflectionSRV()
                    : HDREnv.SpecularSRV();
            const u32 OceanDispSlots[kOceanCascades] = {
                Ocean[0].SRVSlot(), Ocean[1].SRVSlot(), Ocean[2].SRVSlot() };
            const u32 OceanPreviousDispSlots[kOceanCascades] = {
                Ocean[0].PreviousSRVSlot(), Ocean[1].PreviousSRVSlot(),
                Ocean[2].PreviousSRVSlot() };
            const u32 OceanNormalSlots[kOceanCascades] = {
                Ocean[0].NormalSRVSlot(), Ocean[1].NormalSRVSlot(), Ocean[2].NormalSRVSlot() };
            Water.RenderSurface(CommandList, SRVHeap, WaterReflCube, OceanDispSlots,
                                OceanPreviousDispSlots, OceanNormalSlots, Targets.SceneCopyTableStart,
                                SunShadows.ConstantsAddress(),
                                SunShadows.ShadowSRVSlot(), WaterOutputMode);

            if (WaterReflectionsActive) {
                WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                WaterBatch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                GBuffer.AppendTransitions(WaterBatch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                WaterBatch.Flush(CommandList);

                Reflections.RecordWaterTrace(CommandList, SRVHeap, &GpuProfiler);
                if (Modes.RRMode) {
                    RRGuides.RecordWaterSpecHitDist(
                        CommandList, SRVHeap,
                        SRVHeap.GpuHandle(Reflections.GetWaterSpecHitTable()),
                        Ctx.FrameCB);
                }
                Reflections.RecordWaterComposite(CommandList, SRVHeap, Targets.HDRRTVHeap.CpuHandle(0),
                                                 RenderWidth(), RenderHeight());

                WaterBatch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_DEPTH_WRITE);
                GBuffer.AppendTransitions(WaterBatch, D3D12_RESOURCE_STATE_RENDER_TARGET);
                WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                WaterBatch.Flush(CommandList);
            } else {
                WaterBatch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                WaterBatch.Flush(CommandList);
            }
            auto PostWaterRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &PostWaterRTV, FALSE, &DSV);
        }

        // Transparencias foreground sao compostas depois da agua: nao contaminam a copia usada
        // pela refracao e passam a testar contra a profundidade real da superficie.
        {
            bool AnyBlend = false;
            for (const FVisibleItem& V : VisibleScratch)
                if (V.Mat->Blend) { AnyBlend = true; break; }
            if (AnyBlend) {
                FGpuScope Scope(GpuProfiler, CommandList, "Translúcidos");
                D3D12_CPU_DESCRIPTOR_HANDLE BlendRTVs[3] = {
                    Targets.HDRRTVHeap.CpuHandle(0), Targets.UpscaleMaskRTVHeap.CpuHandle(0),
                    Targets.UpscaleMaskRTVHeap.CpuHandle(1) };
                CommandList->OMSetRenderTargets(_countof(BlendRTVs), BlendRTVs, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
                CommandList->SetGraphicsRootDescriptorTable(3, SRVHeap.GpuHandle(IBLTableStart));
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(
                    6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
                {
                    const u32 GITable = (UseGI && DDGI.IsReady())
                        ? DDGI.SceneGITableStart() : IBLTableStart;
                    CommandList->SetGraphicsRootDescriptorTable(7, SRVHeap.GpuHandle(GITable));
                }
                {
                    const u32 AtmoTable = Atmosphere.IsInitialized()
                        ? Atmosphere.TransmittanceSRV() : IBLTableStart;
                    CommandList->SetGraphicsRootDescriptorTable(13, SRVHeap.GpuHandle(AtmoTable));
                }
                CommandList->SetPipelineState(PipelineState.PSOForwardBlend());
                for (auto It = VisibleScratch.rbegin(); It != VisibleScratch.rend(); ++It) {
                    if (!It->Mat->Blend) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(It->Slot) * sizeof(ObjectConstants));
                    It->Mat->Bind(CommandList, SRVHeap);
                    It->R->Mesh->Draw(CommandList);
                }
            }
        }

        if (UseClouds && VolumetricClouds.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Nuvens");

            const D3D12_RESOURCE_STATES CloudReadState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, CloudReadState);
            Batch.Flush(CommandList);

            VolumetricClouds.RecordRaymarch(CommandList, SRVHeap, Targets.DepthSRVSlot);

            auto CloudRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &CloudRTV, FALSE, nullptr); // sem DSV: depth e SRV
            VolumetricClouds.Composite(CommandList, SRVHeap, Targets.DepthSRVSlot);

            Batch.Transition(Targets.DepthBuffer.Get(), CloudReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }
    }

    // CSM do sol (com o cache de casters estaticos) e as sombras locais de spot/point.
    void Renderer::RecordShadows(FPassContext& _Ctx, const FLocalShadowJobs& _Jobs) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& AllItems           = _Ctx.All;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;
        const auto& LocalShadowJobs    = _Jobs.Spot;
        const auto& LocalCubeJobs      = _Jobs.Cube;

        {
            const f32 ShadowNoiseFrame = (Modes.TAAActive || Modes.UpscaleActive)
                ? static_cast<f32>(TemporalSampleIndex % 64u) : 0.0f;
            SunShadows.UpdatePerFrame(FrameSlot, UseSunShadows, Vw.View, Vw.CameraPosition,
                                      Vw.FovY, Vw.Aspect, Lt.KeyDir, Vw.NearZ, ShadowNoiseFrame,
                                      Scene.StaticCastersVersion());
            if (UseSunShadows) {
                std::vector<FSunShadows::FShadowDrawItem> Casters;
                Casters.reserve(AllItems.size());
                for (const FDrawItem& A : AllItems) {
                    // Translucido nao projeta sombra opaca (vidro deixa o sol entrar).
                    if (A.Mat && A.Mat->Blend) continue;
                    // Objeto sob arraste do gizmo conta como DINAMICO enquanto o arraste dura,
                    // qualquer que seja a mobilidade dele. Sem isto cada frame de mouse
                    // invalidaria o mapa estatico e o editor ficaria pior do que sem cache
                    // nenhum — a mesma razao pela qual a UE trata primitiva movida como
                    // movable e so re-cacheia quando ela para.
                    const bool Dyn = A.R->Mobility == EMobility::Dynamic ||
                                     (DraggingRenderableId != 0 && A.R->Id == DraggingRenderableId);
                    Casters.push_back({ A.R->Mesh, A.Mat,
                                        ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                        A.R->AABBMin, A.R->AABBMax, Dyn });
                }
                // Ordena UMA vez, fora do laco de cascatas: o RecordDepthPass varre esta
                // lista 4x e, em ordem de cena, alternava PSO opaco/masked a cada item que
                // trocasse de tipo. Opacos primeiro (PSO trocado uma unica vez, na fronteira)
                // e alpha-tested agrupados por material, o que tambem deixa o Bind repetido
                // ser pulado la dentro.
                //
                // Mobilidade entra como chave PRIMARIA: os estaticos ficam contiguos em
                // [0, N) e os dinamicos em [N, Count), que e a fronteira de onde o cache vai
                // rasterizar cada metade em um alvo diferente. Agrupar por material continua
                // valendo DENTRO de cada metade, que e onde o batching importa.
                std::sort(Casters.begin(), Casters.end(),
                          [](const FSunShadows::FShadowDrawItem& a,
                             const FSunShadows::FShadowDrawItem& b) {
                              if (a.Dynamic != b.Dynamic) return !a.Dynamic;
                              const bool am = a.Mat && a.Mat->Constants.AlphaTest != 0;
                              const bool bm = b.Mat && b.Mat->Constants.AlphaTest != 0;
                              if (am != bm) return !am;
                              return a.Mat < b.Mat;
                          });
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "Sombras — sol (CSM)");
                    FSunShadows::FExtraCascadeDraw TerrainCasters;
                    if (UseTerrain && Terrain.IsLoaded())
                        TerrainCasters = [this](ID3D12GraphicsCommandList* Cmd, u32,
                                                D3D12_GPU_VIRTUAL_ADDRESS CascadeCB,
                                                const Mat44& CascadeVP) {
                            Terrain.RenderShadowCascade(Cmd, SRVHeap, CascadeCB, CascadeVP);
                        };
                    SunShadows.RecordDepthPass(CommandList, SRVHeap, Casters.data(), Casters.size(),
                                               TerrainCasters, &GpuProfiler);
                }

                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->RSSetViewports(1, &Viewport);
                CommandList->RSSetScissorRects(1, &ScissorRect);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
            } else {
                SunShadows.EnsureReadable(CommandList);
            }

            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        }

        if (!LocalShadowJobs.empty() || !LocalCubeJobs.empty()) {
            std::vector<FLocalShadows::FShadowDrawItem> LocalCasters;
            LocalCasters.reserve(AllItems.size());
            for (const FDrawItem& A : AllItems) {
                if (A.Mat && A.Mat->Blend) continue; // vidro nao projeta sombra opaca
                LocalCasters.push_back({ A.R->Mesh, A.Mat,
                                         ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                         A.R->AABBMin, A.R->AABBMax });
            }
            {
                FGpuScope Scope(GpuProfiler, CommandList, "Sombras — locais");
                // Terreno tambem projeta nas luzes locais. Sem isto o terreno era iluminado
                // por point/spot (escreve G-buffer) mas nao aparecia nos shadow maps delas:
                // poste atravessava a colina, e a volumetrica — que le os MESMOS t18/t19 —
                // mostrava o god ray passando pelo terreno.
                FLocalShadows::FExtraLocalDraw TerrainCasters;
                if (UseTerrain && Terrain.IsLoaded())
                    TerrainCasters = [this](ID3D12GraphicsCommandList* Cmd,
                                            D3D12_GPU_VIRTUAL_ADDRESS SliceCB,
                                            const Mat44& SliceVP,
                                            const Vec3& LightPos, f32 Radius) {
                        // Perspectiva com depth clip (o near corta de verdade, ao contrario da
                        // cascata ortho do sol) + a esfera da luz, o mesmo filtro que os meshes
                        // recebem na broad phase.
                        Terrain.RenderShadowCascade(Cmd, SRVHeap, SliceCB, SliceVP, true,
                                                    LightPos, Radius);
                    };
                LocalShadows.RecordDepthPass(CommandList, SRVHeap, FrameSlot,
                                             LocalCasters.data(), LocalCasters.size(),
                                             LocalShadowJobs.data(),
                                             static_cast<u32>(LocalShadowJobs.size()),
                                             LocalCubeJobs.data(),
                                             static_cast<u32>(LocalCubeJobs.size()),
                                             TerrainCasters);
            }

            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);
            CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
            CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
        } else if (LocalShadows.IsInitialized()) {
            LocalShadows.EnsureReadable(CommandList);
        }
    }

    // Z-prepass (opacos, mascarados, terreno), build+teste do HZB e o GTAO. Tudo consome so
    // profundidade e a normal geometrica — o G-buffer ainda nem foi escrito.
    void Renderer::RecordDepthPrepass(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& AllItems           = _Ctx.All;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        const bool DoPrepass = true;
        if (DoPrepass) {
            GpuProfiler.Begin(CommandList, "Z-prepass");
            if (Modes.GeometricNormalWillRun) {
                FBarrierBatch Batch;
                Batch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                Batch.Flush(CommandList);
                auto NormalRTV = Targets.NormalRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &NormalRTV, FALSE, &DSV);
                const FLOAT NeutralN[4] = { 0.5f, 0.5f, 0.5f, 0.0f }; 
                CommandList->ClearRenderTargetView(NormalRTV, NeutralN, 0, nullptr);
                CommandList->SetPipelineState(PipelineState.PSODepthNormal());
            } else {
                CommandList->SetPipelineState(PipelineState.PSODepthOnly());
            }
            {
                FGpuScope Scope(GpuProfiler, CommandList, "Z - opacos");
                for (const FVisibleItem& V : VisibleScratch) {
                    if (V.Mat->TwoSided || V.Mat->Constants.AlphaTest || V.Mat->Blend) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                    V.R->Mesh->Draw(CommandList);
                }
            }

            CommandList->SetPipelineState(Modes.GeometricNormalWillRun
                                              ? PipelineState.PSODepthNormalMasked()
                                              : PipelineState.PSODepthOnlyMasked());
            {
                FGpuScope Scope(GpuProfiler, CommandList, "Z - mascarados");
                for (const FVisibleItem& V : VisibleScratch) {
                    if (V.Mat->Blend) continue;
                    if (!V.Mat->TwoSided && !V.Mat->Constants.AlphaTest) continue;
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                    V.Mat->Bind(CommandList, SRVHeap);
                    V.R->Mesh->Draw(CommandList);
                }
            }

            if (UseTerrain && Terrain.IsLoaded()) {
                FGpuScope Scope(GpuProfiler, CommandList, "Z - terreno");
                Terrain.RenderDepthPrepass(CommandList, SRVHeap, Modes.GeometricNormalWillRun);
            }
            GpuProfiler.End(CommandList); // Z-prepass
        }

        // HZB do depth do prepass (min-reduce reverse-Z) + teste dos AABBs com a VP
        // sem jitter DESTE frame; o resultado volta pela readback ring e e consumido
        // kFramesInFlight frames depois no filtro do VisibleScratch (acima).
        if (HiZ.IsReady() && UseOcclusionCulling) {
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            {
                FGpuScope Scope(GpuProfiler, CommandList, "HZB");
                HiZ.RecordBuild(CommandList, SRVHeap, Targets.DepthSRVSlot);
                HiZ.RecordTest(CommandList, SRVHeap, FrameSlot,
                               static_cast<u32>(Scene.Renderables().size()),
                               Vw.ViewProjUnjittered);
            }
            Batch.Transition(Targets.DepthBuffer.Get(),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.Flush(CommandList);
        }

        if (AO.IsReady()) {
            if (Modes.AOWillRun) {
                CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

                const f32 TanHalf = std::tan(0.5f * Vw.FovY);
                const f32 M11 = 1.0f / TanHalf;
                const f32 M00 = M11 / Vw.Aspect;
                const f32 M22 = Vw.Projection.M[2][2];
                const f32 M32 = Vw.Projection.M[3][2];
                AO.UpdatePerFrame(FrameSlot, M00, M11, M22, M32, Vw.View,
                                  RenderWidth(), RenderHeight(), TemporalSampleIndex);

                FBarrierBatch Batch;
                Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Batch.TransitionTracked(Targets.NormalBuffer.Get(), Targets.NormalBufferState,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Batch.Flush(CommandList);
                {
                    FGpuScope Scope(GpuProfiler, CommandList, "GTAO");
                    AO.Execute(CommandList, SRVHeap, Targets.DepthSRVSlot);
                }

                Batch.Transition(Targets.DepthBuffer.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
                Batch.Flush(CommandList);

                auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
                CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
                CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
                CommandList->SetGraphicsRootConstantBufferView(
                    0, Ctx.FrameCB);
                CommandList->SetGraphicsRootConstantBufferView(5, SunShadows.ConstantsAddress());
                CommandList->SetGraphicsRootDescriptorTable(6, SRVHeap.GpuHandle(SunShadows.ShadowSRVSlot()));
            } else {
                AO.ClearToWhite(CommandList, SRVHeap);
            }
        }
    }

    // Geometry pass (G-buffer + velocity + emissivo no HDR), velocity do background,
    // wetness da chuva sobre o G-buffer e o vetor de movimento temporal confiavel.
    void Renderer::RecordGBuffer(FPassContext& _Ctx) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const FFrameAmbient& Amb       = *_Ctx.Ambient; // wetness da chuva integra o ambiente
        const u32 FrameSlot            = _Ctx.FrameSlot;
        const auto& DSV                = _Ctx.DSV;
        const D3D12_VIEWPORT& Viewport = _Ctx.Viewport;
        const D3D12_RECT& ScissorRect  = _Ctx.Scissor;
        const auto& Ctx                = _Ctx;
        const auto& AllItems           = _Ctx.All;
        const auto& VisibleScratch     = _Ctx.Visible;
        const auto ObjectCBBase        = _Ctx.ObjectCBBase;

        {
            GpuProfiler.Begin(CommandList, "G-buffer (geometria)");
            FBarrierBatch Batch;
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            Batch.Flush(CommandList);
            // 5o MRT = SceneColor HDR: o emissivo e escrito direto nele (dieta do G-buffer).
            // O ceu ja esta la (desenhado antes do prepass); a geometria opaca sobrescreve so
            // os proprios pixels e o deferred lighting depois SOMA a luz (blend aditivo).
            D3D12_CPU_DESCRIPTOR_HANDLE GBufRTVs[FGBuffer::kTargetCount + 2] = {
                GBuffer.RTVHandle(0), GBuffer.RTVHandle(1), GBuffer.RTVHandle(2),
                Targets.VelocityRTVHeap.CpuHandle(0), Targets.HDRRTVHeap.CpuHandle(0) };
            CommandList->OMSetRenderTargets(FGBuffer::kTargetCount + 2, GBufRTVs, FALSE, &DSV);
            GBuffer.Clear(CommandList);
            const FLOAT VelClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            CommandList->ClearRenderTargetView(Targets.VelocityRTVHeap.CpuHandle(0), VelClear, 0, nullptr);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
            CommandList->SetGraphicsRootConstantBufferView(
                0, Ctx.FrameCB);

            {
                FGpuScope Scope(GpuProfiler, CommandList, "G-buffer - meshes");
                ID3D12PipelineState* CurGeomPSO = nullptr;
                for (const FVisibleItem& V : VisibleScratch) {
                    FMaterial* Mat = V.Mat;
                    if (Mat->Blend) continue;
                    const bool TwoSided = Mat->IsTwoSidedForRT();
                    ID3D12PipelineState* Want = TwoSided ? PipelineState.PSOGBufferTwoSided()
                                                         : PipelineState.PSOGBuffer();
                    if (Want != CurGeomPSO) { CommandList->SetPipelineState(Want); CurGeomPSO = Want; }
                    CommandList->SetGraphicsRootConstantBufferView(
                        4, ObjectCBBase + static_cast<u64>(V.Slot) * sizeof(ObjectConstants));
                    Mat->Bind(CommandList, SRVHeap);
                    V.R->Mesh->Draw(CommandList);
                }
            }

            if (UseTerrain && Terrain.IsLoaded()) {
                FGpuScope Scope(GpuProfiler, CommandList, "G-buffer - terreno");
                Terrain.RenderGBuffer(CommandList, SRVHeap);
            }
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            GpuProfiler.End(CommandList); // G-buffer (geometria)
        }

        // Motion vector do BACKGROUND: o G-buffer so escreveu velocity p/ geometria+terreno; ceu/nuvens/fog
        // ficaram ZERO (clear acima). Preenche o velocity do ceu (reproj rotacao-only, sem parallax) p/ o
        // DLSS/RR/TAA nao arrastar o historico do ceu ao girar a camera. So roda com consumidor temporal.
        if ((Modes.UpscaleActive || Modes.TAAActive) && BgVelocity.IsReady()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Velocity do background");
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Batch.Flush(CommandList);

            BgVelocity.Record(CommandList, FrameSlot, SRVHeap.GpuHandle(Targets.DepthSRVSlot),
                              SRVHeap.GpuHandle(Targets.VelocityUavSlot), Vw.SkyClipToPrevClip,
                              RenderWidth(), RenderHeight());

            FBarrierBatch Restore;
            Restore.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Restore.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Restore.Flush(CommandList);
        }

        if (Weather.Active() && RainWetness.IsInitialized()) {
            FGpuScope Scope(GpuProfiler, CommandList, "Chuva — wetness");
            if (Weather.GetRainOcclusion()) {
                std::vector<FRainWetness::FOccluderItem> RainOccluders;
                RainOccluders.reserve(AllItems.size());
                for (const FDrawItem& A : AllItems)
                    RainOccluders.push_back({ A.R->Mesh, A.Mat,
                                              ObjectCBBase + static_cast<u64>(A.Slot) * sizeof(ObjectConstants),
                                              A.R->AABBMin, A.R->AABBMax });
                RainWetness.RecordOcclusionMap(CommandList, SRVHeap, FrameSlot, Vw.CameraPosition,
                                               RainOccluders.data(), RainOccluders.size());
            }
            const Vec3 KeyColorInt = { Lt.KeyColor.X * Lt.KeyInt, Lt.KeyColor.Y * Lt.KeyInt,
                                       Lt.KeyColor.Z * Lt.KeyInt };
            RainWetness.UpdatePerFrame(FrameSlot, Vw.InvViewProjFull, Vw.ViewProjection,
                                       Vw.CameraPosition, ElapsedTime, Weather, Lt.KeyDir,
                                       KeyColorInt, Amb.Sky);
            RainWetness.Execute(CommandList, SRVHeap, GBuffer, Targets.DepthBuffer.Get(), Targets.DepthSRVSlot,
                                RenderWidth(), RenderHeight());
        }

        if (Modes.ReliableMotionActive) {
            FGpuScope Scope(GpuProfiler, CommandList, "Motion temporal confiavel");
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
            TemporalMotion.Record(CommandList, SRVHeap, &GpuProfiler);
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Batch.TransitionTracked(Targets.VelocityBuffer.Get(), Targets.VelocityState,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }
    }

    // Empacota as luzes puntuais da DIRETA (FGPULight, root SRV t17 do deferred): cull pelo
    // frustum, prioriza quem ganha slot de sombra e monta as matrizes. Devolve os jobs de sombra
    // local para o passe de sombras — eles saem daqui porque e aqui que o cull ja aconteceu.
    FLocalShadowJobs Renderer::PackDirectLights(FPassContext& _Ctx, FrameConstants* MappedCB) {
        auto* CommandList              = _Ctx.Cmd;
        const FFrameModes& Modes       = *_Ctx.Modes;
        const FFrameView& Vw           = *_Ctx.View;
        const FFrameLighting& Lt       = *_Ctx.Light;
        const u32 FrameSlot            = _Ctx.FrameSlot;

        const Vec4* Planes = Vw.FrustumPlanes; // resolvido no ResolveFrameView

        FLocalShadowJobs ShadowJobs;
        auto& LocalShadowJobs = ShadowJobs.Spot;
        auto& LocalCubeJobs   = ShadowJobs.Cube;
        {
            Vec4 NPlanes[6];
            for (int i = 0; i < 6; ++i) {
                const Vec4& p   = Planes[i];
                const f32   len = std::sqrt(p.X*p.X + p.Y*p.Y + p.Z*p.Z);
                const f32   inv = len > 1e-6f ? 1.0f / len : 0.0f;
                NPlanes[i] = { p.X*inv, p.Y*inv, p.Z*inv, p.W*inv };
            }

            FGPULight* DstLights = reinterpret_cast<FGPULight*>(
                MappedLightBase + static_cast<size_t>(FrameSlot) * kMaxLights * sizeof(FGPULight));
            u32 NumLights = 0;
            u64 LightSetSignature = 1469598103934665603ull; // FNV-1a sobre IDs na ordem do buffer

            struct ShadowCand { u32 Gpu; u32 LightIdx; u64 Id; f32 Key; };
            std::vector<ShadowCand> ShadowCands;
            std::vector<ShadowCand> CubeCands;

            // Importancia da luz p/ a camera: energia x fracao angular que ela ocupa. O termo
            // R^2/(d^2+R^2) satura em 1 quando a camera entra no raio (sem singularidade em
            // d=0) e vira inverse-square quando ela se afasta. So distancia da camera (o
            // criterio antigo) fazia uma vela a 2 m ganhar de um refletor a 8 m.
            // Nao e a formula do Flax: la o sort e por ScreenSize, com soma da cor de desempate
            // e hash do ID como criterio final (LightPass.cpp, SortLights). Aqui a area angular
            // faz o papel do ScreenSize e a luminancia entra multiplicando em vez de desempatar.
            auto ShadowScore = [&](const FLight& L) -> f32 {
                const f32 Energy = L.Intensity * (L.Color.X * 0.2126f + L.Color.Y * 0.7152f +
                                                  L.Color.Z * 0.0722f);
                const Vec3 ToCam = L.Position - Vw.CameraPosition;
                const f32 R2 = L.AttenuationRadius * L.AttenuationRadius;
                return Energy * R2 / (ToCam.LengthSq() + R2);
            };

            auto& SceneLights = Scene.Lights();
            for (u32 li = 0; li < static_cast<u32>(SceneLights.size()); ++li) {
                FLight& L = SceneLights[li];
                // Identidade estavel na primeira vez que vemos a luz. O editor faz push_back
                // direto em Lights(), entao a atribuicao mora aqui e nao no AddLight.
                if (L.Id == 0) L.Id = Scene.AllocObjectId();
                Vec3 PreviousLightPos = L.Position;
                if (const auto It = PreviousDirectLightPositions.find(L.Id);
                    It != PreviousDirectLightPositions.end())
                    PreviousLightPos = It->second;
                PreviousDirectLightPositions[L.Id] = L.Position;
                if (!L.Enabled || L.Intensity <= 0.0f || L.AttenuationRadius <= 0.0f) continue;
                if (NumLights >= kMaxLights) break;

                bool Outside = false;
                for (int i = 0; i < 6 && !Outside; ++i) {
                    const Vec4& p = NPlanes[i];
                    Outside = (p.X*L.Position.X + p.Y*L.Position.Y + p.Z*L.Position.Z + p.W)
                              < -L.AttenuationRadius;
                }
                if (Outside) continue;

                FGPULight G;
                G.PosInvRadius      = { L.Position.X, L.Position.Y, L.Position.Z,
                                        1.0f / L.AttenuationRadius };
                G.ColorSourceRadius = { L.Color.X * L.Intensity, L.Color.Y * L.Intensity,
                                        L.Color.Z * L.Intensity,
                                        std::max(L.SourceRadius, 0.01f) };
                G.ShadowMatrix      = Mat44::Identity();
                G.PrevPosInvRadius  = { PreviousLightPos.X, PreviousLightPos.Y,
                                        PreviousLightPos.Z, 1.0f / L.AttenuationRadius };
                if (L.Type == ELightType::Spot) {
                    const Vec3 D = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                    const f32 OuterDeg  = std::clamp(L.OuterConeDeg, 1.0f, 89.0f);
                    const f32 InnerDeg  = std::clamp(L.InnerConeDeg, 0.0f, OuterDeg);
                    const f32 CosOuter  = std::cos(OuterDeg * ToRad);
                    const f32 CosInner  = std::cos(InnerDeg * ToRad);
                    G.DirCosOuter = { D.X, D.Y, D.Z, CosOuter };
                    // w preserva a intencao do artista independentemente do orcamento de slices.
                    // O raster usa y/z; o ReSTIR DI usa w para decidir se traca shadow ray.
                    G.SpotParams  = { 1.0f / std::max(CosInner - CosOuter, 1e-4f),
                                      -1.0f, 0.0f, L.CastShadows ? 1.0f : 0.0f };
                    if (L.CastShadows && LocalShadows.IsInitialized()) {
                        // Histerese: quem ja tem o slot vale mais no ranking, entao o novato
                        // precisa ser sensivelmente melhor pra tomar. Sem isso duas luzes de
                        // importancia parecida trocam de slice todo frame.
                        const f32 Bias = LocalShadows.SpotOwns(L.Id)
                                       ? FLocalShadows::kHysteresis : 1.0f;
                        ShadowCands.push_back({ NumLights, li, L.Id, ShadowScore(L) * Bias });
                    }
                } else {
                    G.DirCosOuter = { 0.0f, -1.0f, 0.0f, -2.0f }; // -2 = sem mascara de cone
                    G.SpotParams  = { 0.0f, -1.0f, 0.0f, L.CastShadows ? 1.0f : 0.0f };
                    if (L.CastShadows && LocalShadows.IsInitialized()) {
                        const f32 Bias = LocalShadows.CubeOwns(L.Id)
                                       ? FLocalShadows::kHysteresis : 1.0f;
                        CubeCands.push_back({ NumLights, li, L.Id, ShadowScore(L) * Bias });
                    }
                }
                DstLights[NumLights++] = G;
                LightSetSignature ^= L.Id;
                LightSetSignature *= 1099511628211ull;
            }

            // Matrizes do slice de spot. Usadas por todo mundo que emite job — inclusive quem
            // esta em fade-out, que segue sendo redesenhado da pose atual.
            auto SpotShadowVP = [&](const FLight& L, Mat44& OutLVP, f32& OutFar) {
                const Vec3 D  = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                const Vec3 Up = std::fabs(D.Y) > 0.99f ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                       : Vec3{ 0.0f, 1.0f, 0.0f };
                const f32 OuterRad = std::clamp(L.OuterConeDeg, 1.0f, 89.0f) * ToRad;
                const f32 NearP    = FLocalShadows::kPointNear;
                OutFar = std::max(L.AttenuationRadius, NearP * 2.0f);
                OutLVP = Mat44::LookAtLH(L.Position, L.Position + D, Up) *
                         Mat44::PerspectiveFovLH(2.0f * OuterRad, 1.0f, NearP, OutFar);
            };
            auto ToShadowUV = [](const Mat44& LVP) {
                Mat44 BiasUV = Mat44::Identity();
                BiasUV.M[0][0] = 0.5f;  BiasUV.M[1][1] = -0.5f;
                BiasUV.M[3][0] = 0.5f;  BiasUV.M[3][1] = 0.5f;
                return LVP * BiasUV;
            };

            // Id como desempate (mesma ideia do hash do ID no SortLights do Flax): std::sort nao
            // e estavel, entao scores empatados poderiam trocar de ordem entre frames e mudar
            // quem fica com o slot livre.
            auto ByScore = [](const ShadowCand& a, const ShadowCand& b) {
                return a.Key != b.Key ? a.Key > b.Key : a.Id < b.Id;
            };
            std::sort(ShadowCands.begin(), ShadowCands.end(), ByScore);
            const u32 NumShadowed = std::min<u32>(static_cast<u32>(ShadowCands.size()),
                                                  FLocalShadows::kActiveShadows);
            // Slice pela IDENTIDADE da luz, nao por 's' (a posicao no ranking). Com 's' a mesma
            // luz trocava de slice sempre que a camera reordenava o sort — nenhum cache de
            // depth entre frames era possivel e nao havia como saber se a luz ja tinha sombra
            // no frame anterior (pre-requisito de histerese/fade). Resolve a selecao INTEIRA de
            // uma vez: um a um, o novato evicta um selecionado e dispara remapeamento em
            // cascata (ver TShadowSlotCache::AcquireBatch).
            u64 SpotIds[FLocalShadows::kActiveShadows];
            u32 SpotSlot[FLocalShadows::kActiveShadows];
            for (u32 s = 0; s < NumShadowed; ++s)
                SpotIds[s] = SceneLights[ShadowCands[s].LightIdx].Id;
            LocalShadows.AcquireSpotSlots(SpotIds, NumShadowed, SpotSlot);
            LocalShadows.UpdateSpotFades(SpotIds, NumShadowed, LastDeltaTime);

            for (u32 s = 0; s < NumShadowed; ++s) {
                const u32 Slice = SpotSlot[s];
                if (Slice == FLocalShadows::kNoSlot) continue;

                const FLight& L = SceneLights[ShadowCands[s].LightIdx];
                Mat44 LVP; f32 FarP;
                SpotShadowVP(L, LVP, FarP);

                FGPULight& G   = DstLights[ShadowCands[s].Gpu];
                G.ShadowMatrix = ToShadowUV(LVP);
                G.SpotParams.Y = static_cast<f32>(Slice);
                G.SpotParams.Z = LocalShadows.SpotFadeAt(Slice);
                LocalShadowJobs.push_back({ LVP, L.Position, FarP, Slice });
            }

            // Slices em FADE-OUT: a luz saiu da selecao mas o slot ainda nao zerou. Ela continua
            // sendo tratada como caster normal — matriz recalculada e JOB emitido — so que com o
            // fade caindo. Redesenhar (em vez de congelar o mapa do frame anterior) e o que
            // mantem a sombra correta se a luz ou os casters se moverem durante a transicao.
            // E o slice extra de kMaxShadows que paga por isso.
            for (u32 i = 0; i < FLocalShadows::kMaxShadows; ++i) {
                const u64 Owner = LocalShadows.SpotOwnerAt(i);
                if (Owner == 0 || LocalShadows.SpotFadeAt(i) <= 0.0f) continue;
                bool Active = false;
                for (u32 s = 0; s < NumShadowed && !Active; ++s) Active = (SpotIds[s] == Owner);
                if (Active) continue;
                // Precisa seguir visivel (ter entrada em DstLights) p/ valer o redesenho.
                for (const ShadowCand& C : ShadowCands) {
                    if (C.Id != Owner) continue;
                    const FLight& Lr = SceneLights[C.LightIdx];
                    Mat44 LVP; f32 FarP;
                    SpotShadowVP(Lr, LVP, FarP);
                    FGPULight& G   = DstLights[C.Gpu];
                    G.ShadowMatrix = ToShadowUV(LVP);
                    G.SpotParams.Y = static_cast<f32>(i);
                    G.SpotParams.Z = LocalShadows.SpotFadeAt(i);
                    LocalShadowJobs.push_back({ LVP, Lr.Position, FarP, i });
                    break;
                }
            }

            std::sort(CubeCands.begin(), CubeCands.end(), ByScore);
            const u32 NumCubes = std::min<u32>(static_cast<u32>(CubeCands.size()),
                                               FLocalShadows::kActiveCubes);
            u64 CubeIds[FLocalShadows::kActiveCubes];
            u32 CubeSlot[FLocalShadows::kActiveCubes];
            for (u32 c = 0; c < NumCubes; ++c)
                CubeIds[c] = SceneLights[CubeCands[c].LightIdx].Id;
            LocalShadows.AcquireCubeSlots(CubeIds, NumCubes, CubeSlot);
            LocalShadows.UpdateCubeFades(CubeIds, NumCubes, LastDeltaTime);

            for (u32 c = 0; c < NumCubes; ++c) {
                const u32 Cube = CubeSlot[c];
                if (Cube == FLocalShadows::kNoSlot) continue;

                const FLight& L = SceneLights[CubeCands[c].LightIdx];
                FGPULight& G   = DstLights[CubeCands[c].Gpu];
                G.SpotParams.Y = static_cast<f32>(Cube);
                G.SpotParams.Z = LocalShadows.CubeFadeAt(Cube);
                LocalCubeJobs.push_back({ L.Position, L.AttenuationRadius, Cube });
            }

            // Cubos em fade-out (mesma logica dos spots; o point nem matriz precisa).
            for (u32 i = 0; i < FLocalShadows::kMaxCubeShadows; ++i) {
                const u64 Owner = LocalShadows.CubeOwnerAt(i);
                if (Owner == 0 || LocalShadows.CubeFadeAt(i) <= 0.0f) continue;
                bool Active = false;
                for (u32 c = 0; c < NumCubes && !Active; ++c) Active = (CubeIds[c] == Owner);
                if (Active) continue;
                for (const ShadowCand& C : CubeCands) {
                    if (C.Id != Owner) continue;
                    const FLight& Lr = SceneLights[C.LightIdx];
                    FGPULight& G   = DstLights[C.Gpu];
                    G.SpotParams.Y = static_cast<f32>(i);
                    G.SpotParams.Z = LocalShadows.CubeFadeAt(i);
                    LocalCubeJobs.push_back({ Lr.Position, Lr.AttenuationRadius, i });
                    break;
                }
            }

            FrameLightCount = NumLights; // consumido pelos dispatches de direta local, mais adiante
            const u64 NewLightSetSignature = LightSetSignature ^ static_cast<u64>(NumLights);
            if (NewLightSetSignature != FrameLightSetSignature)
                NrdDirect.InvalidateHistory();
            FrameLightSetSignature = NewLightSetSignature;
            ReSTIRDI.SetLightSetSignature(FrameLightSetSignature);
            // Um bit escolhe o produtor da direta local: raster ou ReSTIR DI. O mesmo predicado
            // manda no dispatch e no deferred; divergir aqui apagaria ou dobraria luz.
            const f32 DirectLocalMode = Modes.ReSTIRDIActiveFrame ? 1.0f : 0.0f;
            MappedCB->LightParams  = { static_cast<f32>(NumLights),
                                       1.0f / static_cast<f32>(FLocalShadows::kResolution),
                                       LocalShadows.GetDepthBias(),
                                       DirectLocalMode };
            MappedCB->LightParams2 = { 1.0f / static_cast<f32>(FLocalShadows::kCubeResolution),
                                       FLocalShadows::kPointNear, 0.0f, 0.0f };

            if (Modes.VolFogActive)
                VolumetricFog.PatchLights(NumLights,
                                          1.0f / static_cast<f32>(FLocalShadows::kResolution),
                                          LocalShadows.GetDepthBias(),
                                          FLocalShadows::kPointNear);
        }
        return ShadowJobs;
    }

    // Monta as listas de draw do frame: escreve o ObjectConstants de cada renderavel, resolve a
    // selecao, aplica frustum + oclusao HZB e ordena front-to-back. E a 2a etapa de preenchimento
    // do FPassContext — daqui em diante Ctx.All / Ctx.Visible / Ctx.Selection sao validos.
    void Renderer::BuildDrawLists(FPassContext& _Ctx) {
        const FFrameModes& Modes = *_Ctx.Modes;
        const FFrameView& Vw     = *_Ctx.View;
        const FFrameLighting& Lt = *_Ctx.Light;
        const u32 FrameSlot      = _Ctx.FrameSlot;
        // Era um `Camera.GetPosition()` proprio; e a MESMA chamada que produziu o
        // Vw.CameraPosition no ResolveFrameView, e nada move a camera dentro do frame.
        const Vec3& CamPos       = _Ctx.View->CameraPosition;

        const u32 FrameObjectBase = FrameSlot * MaxObjects;

        auto& AllItems = _Ctx.All;

        u32             SelectedSlot  = kInvalidSlot;
        const FGpuMesh* SelectedMesh  = nullptr;
        Mat44           SelectedModel = Mat44::Identity();
        // Hasteado do loop: a selecao virou uma consulta (mesh OU luz), e o loop abaixo roda
        // por renderavel da cena.
        const int       SelectedRenderable = GetSelectedObject();
        {
            const std::vector<FRenderable>& RList = Scene.Renderables();
            AllItems.reserve(RList.size());
            const size_t PrevCount = PrevModels.size();
            PrevModels.resize(RList.size(), Mat44::Identity());
            const bool WriteOcclusionBounds = UseOcclusionCulling && HiZ.ObjectsReady();
            for (size_t si = 0; si < RList.size(); ++si) {
                const FRenderable& R = RList[si];
                if (WriteOcclusionBounds)
                    HiZ.WriteBounds(FrameSlot, static_cast<u32>(si), R.AABBMin, R.AABBMax);
                if (!R.Visible || R.RaytracingOnly || !R.Mesh || !R.Mesh->IsValid()) continue;
                if (AllItems.size() >= MaxObjects) break;
                FMaterial* Mat = (R.Material && R.Material->IsFinalized()) ? R.Material : ActiveMaterial;
                const u32 Slot = FrameObjectBase + static_cast<u32>(AllItems.size());
                const Mat44 Model = R.Transform.Matrix();
                const Mat44 PrevModel = (si < PrevCount) ? PrevModels[si] : Model;
                ObjectConstants OC;
                OC.MVP            = Model * Vw.ViewProjection;
                OC.ModelMatrix    = Model;
                OC.CurMVPNoJitter = Model * Vw.ViewProjUnjittered;
                OC.PrevMVP        = PrevModel * PrevViewProj;
                std::memcpy(MappedObjectCB + static_cast<size_t>(Slot) * sizeof(ObjectConstants),
                            &OC, sizeof(ObjectConstants));
                if (static_cast<int>(si) == SelectedRenderable) {
                    SelectedSlot = Slot; SelectedMesh = R.Mesh; SelectedModel = Model;
                }
                AllItems.push_back({ &R, Mat, Slot, static_cast<u32>(si) });
                PrevModels[si] = Model;
            }
        }
        // A selecao entra no contexto AQUI, no unico laco que ja varre a cena: o contorno a
        // consome ~1400 linhas abaixo, e recompor la exigiria varrer tudo de novo.
        _Ctx.Selection = { SelectedSlot, SelectedMesh, SelectedModel, SelectedRenderable };

        // Resultado do teste HZB gravado ha kFramesInFlight frames neste slot (a fence
        // ja foi esperada no BeginFrame). nullptr = sem teste valido -> tudo visivel.
        const u32* OcclusionVis = UseOcclusionCulling
            ? HiZ.ResolveResults(FrameSlot, static_cast<u32>(Scene.Renderables().size()))
            : nullptr;
        u32 OccludedCount = 0;

        auto& VisibleScratch = _Ctx.Visible;
        VisibleScratch.reserve(AllItems.size());
        for (const FDrawItem& A : AllItems) {
            if (UseFrustumCulling && Vw.AABBOutsideFrustum(A.R->AABBMin, A.R->AABBMax)) continue;
            // Objeto selecionado nunca e cullado (gizmo/drag move mais rapido que a
            // latencia do readback e o pop incomodaria bem aqui). O resultado so cobre
            // [0, Capacity); indices alem disso (ex.: proxy RT do terreno) ficam visiveis.
            if (OcclusionVis && A.SceneIndex < HiZ.Capacity() &&
                !OcclusionVis[A.SceneIndex] &&
                static_cast<int>(A.SceneIndex) != SelectedRenderable) {
                ++OccludedCount;
                continue;
            }
            const f32 cx = (A.R->AABBMin.X + A.R->AABBMax.X) * 0.5f - CamPos.X;
            const f32 cy = (A.R->AABBMin.Y + A.R->AABBMax.Y) * 0.5f - CamPos.Y;
            const f32 cz = (A.R->AABBMin.Z + A.R->AABBMax.Z) * 0.5f - CamPos.Z;
            VisibleScratch.push_back({ A.R, A.Mat, cx*cx + cy*cy + cz*cz, A.Slot, A.SceneIndex });
        }
        std::sort(VisibleScratch.begin(), VisibleScratch.end(),
                  [](const FVisibleItem& a, const FVisibleItem& b) { return a.Dist < b.Dist; });
        LastVisibleCount  = static_cast<u32>(VisibleScratch.size());
        LastOccludedCount = OccludedCount;

        if (UseTerrain && Terrain.IsLoaded())
            Terrain.UpdatePerFrame(FrameSlot, Vw.ViewProjection, Vw.ViewProjUnjittered, PrevViewProj,
                                   Vw.CameraPosition, Vw.FovY, Vw.MipBias);
    }

    void Renderer::RenderFrame() {
        // O DebugDraw e preenchido pelo EDITOR antes do frame e consumido aqui. Se o frame morrer
        // no meio (excecao — a render thread captura e tenta o proximo) ou sair pelo early return
        // abaixo, o buffer nao pode sobreviver: o editor submete de novo no tick seguinte e a
        // geometria duplicaria ate estourar o orcamento. A guarda faz do "limpa em toda saida"
        // uma invariante do tipo, em vez de uma linha no fim do metodo.
        struct FDebugDrawClearGuard {
            FDebugDraw& Target;
            ~FDebugDrawClearGuard() { Target.Clear(); }
        } DebugDrawGuard{ DebugDraw };

        if (!Initialized) return;

        // Edicoes de material chegam da UI ENTRE frames e varias caem no mesmo frame (um arraste
        // de slider dispara o setter a cada tick). Os setters so marcam; o refresh — que custa um
        // Flush da fila + reset de historicos — acontece uma vez aqui, coalescido e ANTES do
        // BeginFrame, ou seja, fora da gravacao do command list.
        if (MaterialRTStateDirty) {
            MaterialRTStateDirty = false;
            Settings().NotifyMaterialRTStateChanged();
        }
        // Idem para energia de luz no indireto. Fica DEPOIS e em if separado de proposito: se os
        // dois cairem no mesmo frame, o de material ja invalidou tudo e este vira no-op barato —
        // mas ele nao pode DEPENDER daquele, porque mexer so no peso da luz nao marca material.
        if (IndirectLightingDirty) {
            IndirectLightingDirty = false;
            Settings().NotifyIndirectLightingChanged();
        }
        // Idem para visibilidade de objeto e edicao de luz puntual. Tambem em if separado: e o
        // subconjunto sem DDGIAtlas (a caixa de regiao ja foi entregue no proprio setter, e
        // nao e coalescida — ela e barata e a uniao dentro do FDDGI ja faz o trabalho).
        if (SceneContentDirty) {
            SceneContentDirty = false;
            Settings().NotifySceneContentChanged();
        }

        // Captura deterministica: abre a sessao (preset + reset) ou desfaz o preset da anterior.
        // AQUI, junto das coalescencias acima e antes do BeginFrame, pelo mesmo motivo delas —
        // trocar upscaler/render scale realoca os alvos da cena, e isso nao pode acontecer com um
        // command list aberto.
        UpdateFrameCapture();

        CommandQueue.BeginFrame();

        GpuProfiler.BeginFrame(CommandQueue.FrameIndex());
        GpuProfiler.Begin(CommandQueue.List(), "Frame (GPU)");

        ObjectPicker.Tick();

        IUpscaler* ActiveUp = ActiveUpscaler();          // FSR ou DLSS-SR ativo (nullptr = None/indisponivel)
        const FFrameModes Modes = ResolveFrameModes();
        const FFrameView  Vw    = ResolveFrameView(Modes, ActiveUp);
        LastViewProj = Vw.ViewProjUnjittered;

        const u32 FrameSlot = CommandQueue.FrameIndex();
        // BeginFrame acabou de esperar a fence deste slot, portanto o readback gravado na
        // utilizacao anterior do slot ja pode ser mapeado sem stall.
        CollectDebugPreviewReadback(FrameSlot);
        RadianceCache.CollectStats(FrameSlot);
        DDGIDebugPass.CollectPointDiagnostic(FrameSlot);

        // Aquecimento global do cache, AQUI e nao junto do UpdatePerFrame dele. O tick le os
        // contadores que o CollectStats acabou de entregar, e uma mudanca da consulta derruba
        // o historico dos CONSUMIDORES — que publicam os cbuffers deles ao longo do frame, cada um
        // no seu momento. A nevoa volumetrica resolve o `UseHistory` dela dentro do
        // ResolveFrameLighting, bem antes do bloco de GI: notificada de la, ela ja teria decidido
        // reprojetar a historia que o reset acabou de invalidar, e o descarte so valeria no frame
        // seguinte. No topo do frame nao ha esse "depende de quem publicou primeiro".
        //
        // O latch cobre os dois sentidos: o aquecimento ABRINDO a consulta, e o reload de shader
        // FECHANDO-A (ele reseta a tabela de fora do funil, entre frames — ver
        // FRadianceCache::OnRecreatePipelines).
        RadianceCache.TickWarmup();
        if (RadianceCache.ConsumeQueryChange()) Settings().NotifyRadianceCacheQueryChanged();
        FrameConstants* MappedCB = reinterpret_cast<FrameConstants*>(
            MappedFrameBase + static_cast<size_t>(FrameSlot) * sizeof(FrameConstants));


        // O relogio do mundo vem ANTES da luz: o ResolveFrameLighting abaixo le a direcao de sol
        // que o Time-of-Day acabou de mover e a molhadura que a chuva acabou de integrar.
        TickWorldClock();
        const FFrameLighting Lt  = ResolveFrameLighting();
        // ANTES dos tres publicadores abaixo: eles capturam DDGI.CascadeConstants(), e a colocacao
        // das cascatas tem de estar decidida quando isso acontece. O DDGI.UpdatePerFrame roda bem
        // mais tarde (PrepareIndirectLighting) e apenas publica o que ja foi decidido aqui — ver
        // FDDGI::PrepareCascadePlacement.
        DDGI.PrepareCascadePlacement(Vw.CameraPosition);
        const FFrameAmbient  Amb = PublishFrameConstants(Vw, Lt, FrameSlot, MappedCB);
        PushRayTracingFrameState(Modes);

        MappedCB->ReflectionParams = { Reflections.GetMaxRoughness(), Reflections.GetRoughnessFade(),
                                       Modes.ReflectionsActive ? 1.0f : 0.0f,
                                       Modes.ReSTIRGIActive ? (Modes.NrdIndirectMode ? 2.0f : 1.0f) : 0.0f };

        MappedCB->InvViewProj  = Vw.InvViewProjFull;
        MappedCB->RenderParams = { Vw.MipBias, 0.0f, 0.0f, 0.0f };

        UpdateAtmosphereAndVolumetrics(Modes, Vw, Lt, Amb, FrameSlot, MappedCB);
        UpdateWaterAndOcean(Modes, Vw, Lt, Amb, FrameSlot);

        FPassContext Ctx = MakePassContext(Modes, Vw, Lt, Amb, FrameSlot);
        // Aliases sobre o contexto — NAO copias. Sobrevivem a migracao porque os poucos blocos que
        // continuam inline aqui embaixo sao estrutura de FRAME, nao passe: a espera do fence da
        // fila assincrona, o clear dos alvos de instrumentacao e os dois ganchos de debug. Nenhum
        // deles compoe imagem da cena, entao promove-los a fase so aumentaria a lista sem
        // aumentar a clareza.
        auto* CommandList         = Ctx.Cmd;
        const auto& DSV           = Ctx.DSV;
        D3D12_VIEWPORT& Viewport  = Ctx.Viewport;
        D3D12_RECT& ScissorRect   = Ctx.Scissor;
        const u64& GIComputeFence = Ctx.AsyncGIFence;

        BeginSceneRecording(Ctx);
        RecordSkyAndClouds(Ctx);

        PrepareIndirectLighting(Ctx);

        // Root signature + constantes do frame: os passes de compute acima trocaram o estado,
        // e tudo que grava raster daqui pra frente conta com estes dois amarrados.
        CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
        CommandList->SetGraphicsRootConstantBufferView(0, Ctx.FrameCB);

        const FLocalShadowJobs ShadowJobs = PackDirectLights(Ctx, MappedCB);

        BuildDrawLists(Ctx);

        RecordShadows(Ctx, ShadowJobs);

        RecordDepthPrepass(Ctx);

        RecordGBuffer(Ctx);

        // PRODUTOR DEDICADO DO RADIANCE CACHE (Fase 3). Aqui, e nao em PrepareIndirectLighting:
        // as origens dos caminhos sao pixels do G-buffer, que ate a linha de cima nao existiam.
        //
        // Antes da espera do DDGI assincrono de proposito — ele nao consome DDGI, entao este
        // trabalho na fila direta pode sobrepor o compute que ainda esta em voo. O preco e
        // devolver o depth a DEPTH_WRITE no fim do bloco: dali para frente o codigo assume esse
        // estado (o RecordSceneLighting so o move quando o ReSTIR GI roda). O G-buffer nao precisa
        // de restauracao — ele rastreia o proprio estado.
        if (Modes.RadianceCacheUpdateActive) {
            FGpuScope Scope(GpuProfiler, CommandList, "Radiance cache (update)");
            FBarrierBatch Batch;
            Batch.Transition(Targets.DepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GBuffer.AppendTransitions(Batch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);

            RadianceCache.RecordUpdate(CommandList, SRVHeap);

            FBarrierBatch Restore;
            Restore.Transition(Targets.DepthBuffer.Get(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Restore.Flush(CommandList);
        }

        if (GIComputeFence != 0) {
            // Bracket de STALL: par de timestamps na fila DIRETA em volta do wait. E a unica
            // medida honesta do que decide a contagem de sondas do DDGI — o compute assincrono
            // tem de terminar antes DESTE ponto, nao antes do fim do frame, entao a folga real e
            // `wait - fim do compute` e nao o periodo. Medir pelo periodo superestima o orcamento.
            //
            // Por que nao dava para ler isso do profiler: ha GetTimestampFrequency por fila mas
            // nenhum GetClockCalibration, entao os ticks da direta e os da compute estao em bases
            // de tempo NAO correlacionadas — subtrair um do outro nao significa nada. Aqui os dois
            // timestamps sao da MESMA fila: o primeiro fecha o segmento do G-buffer, o segundo so
            // executa depois que a fence liberou. A diferenca e o stall, em base unica.
            //
            // Como ler, e o piso NAO e zero: o valor inclui o custo minimo de fechar o segmento,
            // sinalizar e reenviar a fila, que no Bistro deu ~0,02 ms com o compute ja terminado.
            // Esse e o BASELINE — "stall indistinguivel do overhead", nao 0,02 ms de espera. O
            // criterio para achar o teto de sondas e subida SUSTENTADA acima desse baseline, e
            // nao "descolou de zero".
            //
            // E ele demora a assentar: o FGpuProfiler suaviza por EMA de 0.1 e so exibe duas
            // casas, entao um degrau leva ~30-40 frames para aparecer inteiro. Ler antes disso
            // mostra o meio da rampa.
            //
            // O metodo nao mede folga POSITIVA — quando o compute ja acabou, o valor fica no
            // baseline e nao diz por quanta margem. "Cabe N vezes mais sonda" so se prova
            // subindo as sondas, nunca deduzindo deste numero.
            GpuProfiler.Begin(CommandList, "Espera do DDGI (async)");
            CommandQueue.SubmitSegmentAndContinue();
            CommandQueue.GpuWait(ComputeQueue.NativeFence(), GIComputeFence);
            GpuProfiler.End(CommandList); // ja no segmento de DEPOIS do wait
            ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
            CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            DDGI.TransitionForRead(CommandList);
        }

        // Antes dos dois traces instrumentados: pixel que sai cedo nao escreve, e sem zerar o
        // alvo ele mostraria a medida de um frame antigo — quente onde nao houve trabalho nenhum.
        if (TimerCaptureActive) {
            TimerGI.Clear(CommandList, SRVHeap);
            TimerReflections.Clear(CommandList, SRVHeap);
        }

        // Debug da BVH (GPU Zen 3, 7.3.3). Dispatch proprio, independente do resto do frame: ele
        // traca os raios PRIMARIOS que o pipeline hibrido nunca traca, entao nao le G-buffer nem
        // depth e nao entra em nenhuma cadeia de dependencia. Sem o toggle, nao custa nada.
        // Nao precisa de clear: todo pixel do dominio escreve (o miss tem cor propria).
        if (BvhDebugEnabled && BvhDebug.IsReady()) {
            BvhDebug.Render(CommandList, SRVHeap, Vw.InvViewProjFull, Vw.CameraPosition,
                            BvhDebugMode, DDGI.MaxRayDistance(), BvhDebugComplexityMax,
                            FrameSlot);
        }

        RecordSceneLighting(Ctx);

        RecordForwardAndClouds(Ctx);

        // Registrado no proprio sitio (e nao reavaliando a condicao la embaixo) porque o que importa
        // p/ o RR e se a geometria de debug FOI desenhada no HDR, nao se ela estava habilitada.
        bool DDGIDebugDrew = false;
        if (Device.RaytracingSupported() && DDGIDebugPass.GetEnabled() && DDGI.IsReady()) {
            auto SceneRTV = Targets.HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &SceneRTV, FALSE, &DSV);
            CommandList->RSSetViewports(1, &Viewport);
            CommandList->RSSetScissorRects(1, &ScissorRect);
            DDGIDebugPass.Render(FrameSlot, CommandList, SRVHeap, DDGI, Vw.ViewProjection, Vw.CameraPosition,
                                 TemporalSampleIndex);
            DDGIDebugDrew = true;
        }

        RecordVolumetricsAndRain(Ctx);

        const bool RRPoisoned = RecordDebugViews(Ctx, DDGIDebugDrew);

        {
            FBarrierBatch Batch;
            Batch.Transition(Targets.HDRColorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Batch.Flush(CommandList);
        }

        const FPostInput PostSrc = RecordResolve(Ctx, ActiveUp, RRPoisoned);

        PrevViewProj  = Vw.ViewProjUnjittered;
        PrevCameraPosition = Vw.CameraPosition;
        PrevVPNoTrans = Vw.VPNoTransUnjit;   // frame anterior p/ a reprojecao do background (velocity do ceu)
        NrdPrevProj   = Vw.ProjUnjittered;
        NrdPrevView   = Vw.View;
        PrevJitterUv  = Vw.JitterUv;
        PrevJitterPx = Vw.JitterPx;

        RecordPost(Ctx, PostSrc);

        GpuProfiler.End(CommandList); 
        GpuProfiler.Resolve(CommandList);

        SMILE_HR(CommandList->Close());
        ID3D12CommandList* CommandLists[] = { CommandList };

        AsyncGIRanLastFrame = (GIComputeFence != 0);
        CommandQueue.EndFrame(CommandLists, 1);

        // Avanca o aquecimento e, no frame de captura, grava PNG + manifesto. ANTES do ++ dos
        // contadores logo abaixo: o manifesto grava o TemporalSampleIndex com que este frame
        // amostrou, e ele e a prova de que o contrato "aquece 0..N-1, captura em N" valeu.
        // Recebe os Modes deste frame porque o manifesto registra o que RODOU, nao o que foi
        // pedido — ver CollectCaptureState.
        FinishFrameCapture(Modes, FrameSlot);

        // === Avanco dos contadores de frame ==================================================
        // AQUI, e nao no meio do frame, e a posicao e que faz o contrato valer.
        //
        // O incremento vivia logo depois do ResolveFrameView. Como aquele ja tinha consumido o
        // indice para o JITTER, o resto do frame — nevoa, ReGIR, DDGI, reflexoes, os dois ReSTIR,
        // NRD, sombras, AO — rodava com o valor SEGUINTE. Um frame amostrava em duas sementes
        // diferentes.
        //
        // Isso passou despercebido enquanto era so o FrameIndex, porque ninguem comparava os dois
        // grupos. Deixa de passar com a captura deterministica: apos zerar, o aquecimento usaria
        // 0..N-1 no jitter e 1..N em todo o resto, e o frame capturado misturaria N com N+1 — ou
        // seja, o contrato de warm-up seria falso por construcao, e a captura "deterministica"
        // dependeria de qual metade do frame se olhasse.
        //
        // Com o avanco no fim, todo consumidor do frame le o MESMO valor, e o `0..N-1 / captura em
        // N` sai da definicao em vez de sair da sorte de onde o `++` estava.
        //
        // ⚠️ Muda semente: alguns passes recebem um valor a menos que antes deste commit, entao a
        // imagem NAO e bit a bit igual a do commit anterior. E a correcao da semantica, nao um
        // efeito colateral dela — e e melhor pagar isso agora, antes de o capturador existir e de
        // haver capturas de referencia gravadas com a semantica errada.
        //
        // Os dois andam juntos: so o reset deterministico os separa, zerando o de amostragem e
        // deixando o absoluto correr.
        ++FrameIndex;
        ++TemporalSampleIndex;
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
