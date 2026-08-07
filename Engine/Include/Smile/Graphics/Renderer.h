#pragma once

#include <Windows.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include "Smile/Math/Math.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/UploadQueue.h"
#include "Smile/Graphics/ComputeQueue.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/SwapChain.h"
#include "Smile/Graphics/PipelineState.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/Camera.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/SceneTargets.h"
#include "Smile/Graphics/FrameContext.h"
#include "Smile/Graphics/DebugView.h"
#include "Smile/Graphics/ShaderTimer.h"
#include "Smile/Graphics/BvhDebugView.h"
#include "Smile/Graphics/HDREnvironment.h"
#include "Smile/Graphics/Atmosphere.h"
#include "Smile/Graphics/TimeOfDay.h"
#include "Smile/Graphics/CloudNoise.h"
#include "Smile/Graphics/VolumetricClouds.h"
#include "Smile/Graphics/Skybox.h"
#include "Smile/Graphics/Fog.h"
#include "Smile/Graphics/VolumetricFog.h"
#include "Smile/Graphics/SunShafts.h"
#include "Smile/Graphics/Weather.h"
#include "Smile/Graphics/RainWetness.h"
#include "Smile/Graphics/SunShadows.h"
#include "Smile/Graphics/LocalShadows.h"
#include "Smile/Graphics/RaytracingScene.h"
#include "Smile/Graphics/DDGI.h"
#include "Smile/Graphics/DDGIDebug.h"
#include "Smile/Graphics/ReSTIRGI.h"
#include "Smile/Graphics/ReGIR.h"
#include "Smile/Graphics/MeshLights.h"
#include "Smile/Graphics/ReSTIRDI.h"
#include "Smile/Graphics/NrdDenoiser.h"
#include "Smile/Graphics/Reflections.h"
#include "Smile/Graphics/AmbientOcclusion.h"
#include "Smile/Graphics/HiZOcclusion.h"
#include "Smile/Graphics/PostProcess.h"
#include "Smile/Graphics/MaterialPreview.h"
#include "Smile/Graphics/Picking.h"
#include "Smile/Graphics/SelectionOutline.h"
#include "Smile/Graphics/DebugDraw.h"
#include "Smile/Graphics/TemporalAA.h"
#include "Smile/Graphics/FsrPass.h"
#include "Smile/Graphics/DlssPass.h"
#include "Smile/Graphics/DlssRRPass.h"
#include "Smile/Graphics/DlssRRGuides.h"
#include "Smile/Graphics/BackgroundVelocity.h"
#include "Smile/Graphics/TemporalMotionVectors.h"
#include "Smile/Graphics/FlickerHeatmap.h"
#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Graphics/Water.h"
#include "Smile/Graphics/Terrain.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Scene/Scene.h"

namespace Smile {
    struct FPreparedCookedScene;
    using FPreparedCookedScenePtr = std::shared_ptr<FPreparedCookedScene>;

    // Fachada dos knobs de render (RenderSettings.h). Forward-declarada de proposito: o
    // Renderer a possui por ponteiro para que quem so mexe em parametro nao precise arrastar
    // este header e seus 69 includes.
    class FRenderSettings;

    struct alignas(256) FrameConstants {
        Vec4  CameraPosition;  // 16 bytes
        Vec4  IBLParams;       // 16 bytes — x=intensity, y=rotation(rad), z=maxMip, w=enabled
        Vec4  Time;            // 16 bytes — x=elapsed sec, y=delta sec, z=frameIndex, w=unused
        Vec4  SunDirection;    // 16 bytes — xyz = direction TO sun (normalized), w = intensity
        Vec4  SunColor;        // 16 bytes — rgb = color, w = unused
        Vec4  SkyAmbientColor;    // 16 bytes — rgb = sky (zenith) ambient, w = enabled (0/1)
        Vec4  GroundAmbientColor; // 16 bytes — rgb = ground (nadir) ambient, w = intensity

        Vec4  DDGIGridMin;        // 16 bytes — xyz = origem do grid (mundo), w = espacamento
        Vec4  DDGIGridCount;      // 16 bytes — xyz = nº de probes por eixo, w = enabled (0=off,1=on,2=debug)
        Vec4  DDGIParams;         // 16 bytes — x = intensity, y = tileSize, z = atlasW, w = atlasH
        Vec4  DDGIDistParams;     // 16 bytes — x = distTile, y = distAtlasW, z = distAtlasH, w = chebyshev (0/1)

        Vec4  ReflectionParams;   // 16 bytes — x = maxRoughnessToTrace, y = roughnessFadeLength, z = enabled (0/1), w = -

        Vec4  MoonDirection;      // 16 bytes — xyz = direction TO moon (normalized), w = intensity
        Vec4  MoonColor;          // 16 bytes — rgb = cor do luar (fria, tingida pela transmitancia), w = -

        Mat44 InvViewProj;        // 64 bytes — inversa FULL da view-proj (jittered); deferred lighting
                                  // reconstroi worldPos do depth. Append no fim: nao mexe nos offsets acima.

        Vec4  RenderParams;       // 16 bytes (c18) — x = mip bias global de textura (FSR upscale:
                                  // log2(render/display) - 1; 0 quando nativo/SSAA), yzw = -

        Vec4  CloudShadowParams;  // 16 bytes — xy = centro XZ do shadow map de nuvens (km),
                                  // z = 1/extent (km), w = forca (0 = off)
        Vec4  CloudShadowParams2; // 16 bytes — x = km/unidade de mundo, y = altura da base (km),
                                  // zw = keyDir.xz/keyDir.y (projecao ate a base da camada)

        Vec4  LightParams;        // 16 bytes — x = nº de luzes puntuais no buffer t17,
                                  // y = 1/res do atlas de sombra local, z = bias (NDC),
                                  // w = direta local: 0 raster, 1 ReSTIR DI
        Vec4  LightParams2;       // 16 bytes — x = 1/res do cube shadow (point), y = near das
                                  // faces do cubo (formula do refZ), zw = -

        // Amostragem do DDGI (append no fim: nao mexe nos offsets acima).
        // x = escala do self-shadow bias (0.2 = Flax/legado)
        // y = TETO do bias em metros (0 = sem teto = comportamento historico). Ver
        //     DDGI_SurfaceBias em DDGICommon.hlsli: a formula escala com o espacamento do grid,
        //     que aqui vem da AABB da cena inteira (8 m medidos no Bistro = 1,20 m de bias).
        // zw = reservados (fallback fora do volume)
        Vec4  DDGIBiasParams;

        // --- Transmitancia do sol/lua POR PIXEL (append no fim: offsets acima intactos) -----
        // x = raio do planeta (km), y = raio do topo da atmosfera (km),
        // z = km por unidade de mundo, w = liga o caminho por pixel (0 = usa SunColor/MoonColor
        //     ja transmitidos pela CPU, comportamento historico bit a bit).
        Vec4  AtmoLightParams;
        // Cor do sol/lua SEM transmitancia e SEM HorizonFade. So o deferred e o ForwardBlend
        // consomem estas variantes; fog volumetrico, nuvens, agua, sun shafts e o readout do
        // editor seguem lendo SunColor/MoonColor (transmitidos na CPU) sem edicao nenhuma —
        // e isso que torna o .w um A/B de verdade em vez de um rewrite.
        Vec4  SunColorRaw;        // rgb = cor base * dim de chuva, w = -
        Vec4  MoonColorRaw;       // rgb = tint da lua * dim de chuva, w = -

        // --- Ambiente do ceu em SH-L1 (append no fim) --------------------------------------
        // Um float4 por CANAL, cada um com (c0, c1, c2, c3) na base real l=0/l=1. As 2 cores
        // chapadas (SkyAmbientColor/GroundAmbientColor) continuam preenchidas e sao o fallback
        // e o botao do A/B — elas so variam com o Y da normal, a SH tem o termo direcional.
        Vec4  SkyAmbientSHR;
        Vec4  SkyAmbientSHG;
        Vec4  SkyAmbientSHB;
        Vec4  SkyAmbientSHParams; // x = usar SH (0 = 2 cores chapadas), yzw = -
    };

    // Luz puntual no formato do shader — espelha o FGPULight do DeferredLighting.ps.hlsl
    // (StructuredBuffer t17, root SRV). PrevPosInvRadius fica no fim para os shaders raster
    // continuarem com o prefixo historico intacto.
    struct FGPULight {
        Vec4  PosInvRadius;      // xyz = posicao, w = 1/AttenuationRadius
        Vec4  ColorSourceRadius; // rgb = Color*Intensity, w = bulb (distancia minima)
        Vec4  DirCosOuter;       // xyz = eixo do spot, w = cos(outer); -2 = point (sem cone)
        Vec4  SpotParams;        // x = 1/(cosInner - cosOuter), y = slice de sombra (-1 = sem),
                                 // z = fade do slot [0..1] (0 = sombra apagada, 1 = cheia),
                                 // w = CastShadows pedido pelo artista (0/1). O raster usa y/z;
                                 // o ReSTIR DI usa w para decidir se emite o shadow ray.
        Mat44 ShadowMatrix;      // world -> UVZ do slice (perspectiva: dividir por w no shader)
        Vec4  PrevPosInvRadius;  // xyz = posicao no frame anterior (shadow motion vector)
    };

    // Luz puntual COMPACTA pro mundo indireto (F5) — espelha o FPunctualLight do
    // LightsCommon.hlsli (DDGI/reflexoes/ReSTIR leem nos hits de RT). Sem matriz de sombra:
    // a visibilidade la e por shadow ray inline. Lista SEM frustum cull (luz atras da camera
    // ilumina GI).
    struct FGPULightGI {
        Vec4 PosInvRadius;
        Vec4 ColorSourceRadius;
        Vec4 DirCosOuter;
        Vec4 SpotParams;
    };

    struct alignas(256) ObjectConstants {
        Mat44 MVP;            // 64 bytes — Model * View * Projection (jittered, p/ SV_POSITION)
        Mat44 ModelMatrix;    // 64 bytes — world (para worldPos/worldNormal)
        Mat44 CurMVPNoJitter; // 64 bytes — Model * ViewProjUnjittered (atual) — motion vector
        Mat44 PrevMVP;        // 64 bytes — PrevModel * PrevViewProjUnjittered — motion vector
    };

    class Renderer {
        // A fachada roteia os knobs para os subsistemas e carrega a politica de invalidacao de
        // historico. E friend porque neste passo os corpos foram MOVIDOS bit a bit, sem mudar
        // a posse do estado — ver a nota de transicao no RenderSettings.h.
        friend class FRenderSettings;

    public:
        Renderer();
        ~Renderer() noexcept;

        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Caminho UNICO para parametro de render. O que nao esta aqui nao e knob: selecao,
        // picking, camera, carga de cena, recursos e o visualizador de debug seguem no Renderer.
        FRenderSettings&       Settings();
        const FRenderSettings& Settings() const;

        // Progresso do boot, para a splash screen do editor. Label = etapa corrente, Detail =
        // texto auxiliar opcional (adaptador, contagens), Fraction = 0..1 monotonica.
        // Chamada DE DENTRO de Initialize, portanto na thread que a chamou (render thread) e com
        // o lock do RendererHandle preso: o receptor nao pode tocar no Renderer, so postar p/ a GUI.
        using FInitProgressCallback =
            std::function<void(std::string_view Label, std::string_view Detail, f32 Fraction)>;
        void SetInitProgressCallback(FInitProgressCallback Callback) {
            InitProgressCallback = std::move(Callback);
        }

        void Initialize(HWND hWnd, u32 Width, u32 Height);
        void Shutdown();

        void Resize(u32 Width, u32 Height);
        // Recarrega os PSOs cujo shader mudou. ChangedStem = nome do .cso sem perfil
        // nem extensao (ex.: "WaterSurface.ps"). Vazio (ou stem nao mapeado / .hlsli
        // incluido por varios shaders) => reload completo.
        bool ReloadShaders(const std::string& ChangedStem = "");

        void UpdateCamera(const CameraInput& Input, f32 DeltaTime);
        // Grava e submete o frame. A apresentacao fica separada para que o owner da render
        // thread solte o lock compartilhado antes de entrar no DXGI.
        void RenderFrame();
        // Deve ser chamado exatamente uma vez apos RenderFrame, pela thread proprietaria.
        void PresentFrame();

        void SetMaterial(FMaterial* Material);

        FScene& GetScene() { return Scene; }

        // Materiais importados da(s) cena(s) cozida(s) — o Editor de Materiais edita
        // Constants direto (CBV upload mapeado) e chama UpdateConstants() p/ aplicar.
        std::vector<std::unique_ptr<FMaterial>>& GetMaterials() { return ImportedMaterials; }

        // Carga avulsa de textura em runtime (Editor de Materiais: troca de mapa num slot).
        // .dds direto; resto via WIC (png/jpg/bmp) com mips por decimacao. sRGB so muda a
        // view (albedo/emissivo). Dona da textura: ImportedTextures. nullptr em falha.
        FTexture* ImportRuntimeTexture(const std::wstring& Path, bool IsNormalMap, bool sRGB);

        // Preview offscreen do Editor de Materiais (FMaterialPreview: HDRI proprio, nao
        // interfere no IBL da cena). Submit nunca espera a GPU; Consume so retorna slots prontos.
        FMaterialPreview::ESubmitResult SubmitMaterialPreview(
            FMaterial* Material, const FMaterialPreview::FParams& Params, u64 RequestId);
        bool ConsumeMaterialPreview(FMaterialPreview::FResult& Out) {
            return MaterialPreview.ConsumeCompleted(Out);
        }
        bool LoadMaterialPreviewEnvironment(const std::wstring& Path);
        bool MaterialPreviewReady() const { return MaterialPreview.HasEnvironment(); }

        // A preparacao e CPU-only (I/O, validacao, decode de texturas e copia dos meshes),
        // portanto pode rodar em worker enquanto o Renderer continua exibindo a cena atual.
        // O commit cria/muta recursos D3D12 e deve rodar na thread proprietaria do Renderer.
        static FPreparedCookedScenePtr PrepareCookedScene(const std::wstring& ScenePath);
        bool CommitCookedScene(FPreparedCookedScenePtr Prepared, bool Additive = false);

        // Atalho sincrono mantido para consumidores sem event loop. Additive=true acrescenta
        // a cena cozida sobre a atual sem limpar meshes/materiais/camera ja carregados.
        bool LoadCookedScene(const std::wstring& ScenePath, bool Additive = false);

        FTexture& GetDefaultWhite()  { return TexDefaultWhite; }
        FTexture& GetDefaultNormal() { return TexDefaultNormal; }
        FTexture& GetDefaultBlack()  { return TexDefaultBlack; }


        // Telemetria de culling (os toggles moraram p/ o FRenderSettings).
        u32  GetOccludedCount() const    { return LastOccludedCount; }
        u32  GetVisibleCount() const     { return LastVisibleCount; }
        u32  GetDrawCount() const        { return static_cast<u32>(Scene.Renderables().size()); }

        // Telemetria do CSM por cascata (contagem + frequencia de atualizacao). Const, so
        // leitura: e a base de medida da separacao static/dynamic dos casters.
        const FSunShadows& GetSunShadows() const { return SunShadows; }

        // Objeto sob arraste do gizmo (0 = nenhum). Enquanto dura, ele e tratado como caster
        // DINAMICO qualquer que seja a mobilidade dele: um estatico sendo arrastado invalidaria
        // o mapa cacheado a cada frame de mouse. O editor liga no begin do arraste e desliga no
        // release, e e no release que a invalidacao do conjunto estatico acontece — uma vez, no
        // lugar final, em vez de uma por frame ao longo do caminho.
        void SetDraggingRenderable(u64 Id) { DraggingRenderableId = Id; }
        u64  GetDraggingRenderable() const { return DraggingRenderableId; }


        bool IsInitialized() const { return Initialized; }

        // Supersampling (SSAA): a cena renderiza em RenderWidth/Height = swapchain * RenderScale;
        // o PostProcessor faz o downsample pro backbuffer nativo. >1.0 = mais amostras/pixel.
        // IGNORADO com o denoiser em DLSS_RR: ali a resolucao de ENTRADA e ditada pelo modo de
        // qualidade (o RR nao suporta DRS e a feature NGX e criada na res otima do modo), entao uma
        // escala arbitraria faria o render subrect divergir do buffer criado. A UI ja esconde o
        // slider nesse estado (o RR forca upscaler=DLSS); isto blinda a invariante no motor.
        u32  RenderWidth()  const { return static_cast<u32>(SwapChain.GetWidth()  * RenderScale + 0.5f); }
        u32  RenderHeight() const { return static_cast<u32>(SwapChain.GetHeight() * RenderScale + 0.5f); }
        u32  OutputWidth()  const { return SwapChain.GetWidth(); }
        u32  OutputHeight() const { return SwapChain.GetHeight(); }

        // Picking: o ID pass roda em res interna -> escala a coord do mouse (nativa) por RenderScale.
        void RequestPick(u32 X, u32 Y) {
            ObjectPicker.RequestPick(static_cast<u32>(X * RenderScale + 0.5f),
                                     static_cast<u32>(Y * RenderScale + 0.5f));
        }
        bool TryGetPickResult(int& OutIndex) { return ObjectPicker.TryResolve(OutIndex); }
        // A selecao viaja por INDICE (e o que o picking devolve e o que o loop de draw compara)
        // mas e GUARDADA tambem por Id, para o OnSceneStructureChanged reancora-la depois de a
        // lista andar. Sem isso, apagar um objeto acima do selecionado passava a selecao para o
        // vizinho em silencio.
        void SetSelectedObject(int Index);
        int  GetSelectedObject() const;
        u64  GetSelectedObjectId() const; // 0 quando o selecionado nao e um renderavel
        void ClearSelection();
        // A selecao inteira, sem o chamador precisar saber o tipo. Fonte de verdade dos
        // acessores acima e dos de luz.
        FSceneObjectRef GetSelection() const { return Selection; }

        // Criar/remover objeto com a cena JA carregada. Fecham o ciclo que a FScene sozinha nao
        // fecha: la a lista fica consistente, aqui os cinco sistemas dimensionados ou indexados
        // por indice de cena voltam a concordar com ela. Devolvem false/0 se o Id nao existe.
        bool RemoveRenderable(u64 Id);
        u64  DuplicateRenderable(u64 Id);

        // Selecao de LUZ. Indice em Scene.Lights(); -1 = nenhuma.
        //
        // A exclusividade com a selecao de renderavel deixou de ser combinada: as duas sao a
        // MESMA variavel (Selection), entao selecionar luz derruba a mesh e vice-versa por
        // construcao. Antes eram dois campos e cada call site tinha de lembrar de limpar o
        // outro — os ClearSelection()/ClearLightSelection() espalhados pelo editor sao os
        // restos disso, e agora sao no-ops quando o tipo ja nao bate.
        void SetSelectedLight(int Index);
        int  GetSelectedLight() const;
        void ClearLightSelection();
        void SetOutlineColor(const Vec3& C) { SelectionOutline.SetColor(C); }
        void SetOutlineThickness(f32 T)     { SelectionOutline.SetThickness(T); }
        void SetOutlineIntensity(f32 I)     { SelectionOutline.SetIntensity(I); }
        void SetOutlineFill(f32 F)          { SelectionOutline.SetFillStrength(F); } 

        FDebugDraw& GetDebugDraw() { return DebugDraw; }
        bool WorldToScreen(const Vec3& World, f32& OutX, f32& OutY) const;
        bool ScreenToRay(u32 X, u32 Y, Vec3& OutOrigin, Vec3& OutDir) const;
        Vec3 GetOutlineColor() const        { return const_cast<FSelectionOutline&>(SelectionOutline).GetColor(); }
        f32  GetOutlineThickness() const    { return const_cast<FSelectionOutline&>(SelectionOutline).GetThickness(); }

        bool LoadHDREnvironment(const std::wstring& Path);

        // Estado do Time-of-Day, exposto p/ o painel TOD do editor (leitura e escrita).
        FTimeOfDay&       GetTimeOfDay()       { return TimeOfDay; }
        const FTimeOfDay& GetTimeOfDay() const { return TimeOfDay; }

        void LoadMoonTexture(const std::wstring& Path);
        void LoadStarCatalog(const std::wstring& Path);

        void SetFlickerMode(u32 Mode)        { if (Mode > 0 && FlickerMode == 0) FlickerResetPending = true; FlickerMode = Mode; }
        u32  GetFlickerMode() const          { return FlickerMode; }

        // View modes/debug views exposed to the editor viewport toolbar.
        void SetGBufferDebugMode(u32 Mode)   { GBufferDebugMode = Mode > 8 ? 8 : Mode; }
        u32  GetGBufferDebugMode() const     { return GBufferDebugMode; }

        // === Visualizador de render targets ==============================================
        // Seleciona QUALQUER alvo publicado em DebugTargets pelo indice em All(). kNoDebugTarget
        // desliga. Independente do GBufferDebugMode (que continua servindo o menu de view modes
        // do toolbar); quando os dois estao ativos, o alvo escolhido aqui tem prioridade.
        static constexpr u32 kNoDebugTarget = 0xFFFFFFFFu;
        static constexpr u32 kNoDebugProbe  = 0xFFFFFFFFu;
        void SetDebugTargetIndex(u32 Index)  { DebugTargetIndex = Index; }
        u32  GetDebugTargetIndex() const     { return DebugTargetIndex; }

        // === Instrumentacao de timer nos passes de RT (NVAPI) ============================
        // Liga a captura: os traces de ReSTIR GI e de reflexao passam a rodar a permutacao
        // instrumentada e publicam um heatmap de custo POR PIXEL em DebugTargets. Custo zero
        // desligado (a instrumentacao e permutacao, nao branch). Ver FShaderTimer.
        void SetRtShaderTimer(bool V)        { RtShaderTimer = V; }
        bool GetRtShaderTimer() const        { return RtShaderTimer; }
        // false em GPU nao-NVIDIA ou build sem o SDK: o editor deve desabilitar o toggle.
        bool IsRtShaderTimerAvailable() const;

        // Debug da BVH (GPU Zen 3, 7.3.3): raio primario por pixel na TLAS, publicado como alvo
        // em DebugTargets. Complementa o timer acima — aquele mede ciclos nos passes reais e
        // pede NVAPI; este e portatil e mostra o CONTEUDO e a densidade da estrutura.
        void SetBvhDebug(bool V)                       { BvhDebugEnabled = V; }
        bool GetBvhDebug() const                       { return BvhDebugEnabled; }
        void SetBvhDebugMode(FBvhDebugView::EMode V)   { BvhDebugMode = V; }
        FBvhDebugView::EMode GetBvhDebugMode() const   { return BvhDebugMode; }
        // Teto do heatmap do modo Complexidade, em triangulos testados por raio.
        void SetBvhDebugComplexityMax(f32 V)           { BvhDebugComplexityMax = V < 1.0f ? 1.0f : V; }
        f32  GetBvhDebugComplexityMax() const          { return BvhDebugComplexityMax; }
        // false sem suporte a RT ou antes da TLAS existir: o editor desabilita o toggle.
        bool IsBvhDebugAvailable() const;

        // Selecao MULTIPLA da janela de debug. Diferente do alvo unico acima, esta selecao
        // e composta numa textura offscreen e nunca substitui a imagem do viewport principal.
        // Colunas 0 = o passe escolhe uma grade aproximadamente quadrada.
        // Mantem um alvo 16:9 grande o bastante para a janela maximizada. O preview era
        // 1024x576 e acabava ampliado pelo QML, degradando todos os RTs screen-space.
        static constexpr u32 kDebugPreviewWidth  = 1600;
        static constexpr u32 kDebugPreviewHeight = 900;
        struct FDebugProbeSample {
            u32 ProbeIndex = kNoDebugProbe;
            f32 Irradiance[3] = {};
            f32 MeanDistance = 0.0f;
            f32 DistanceDeviation = 0.0f;
        };
        void SetDebugSelection(const std::vector<u32>& Sel) {
            if (DebugSelection == Sel) return;
            DebugSelection = Sel;
            ++DebugPreviewConfigVersion;
        }
        const std::vector<u32>& GetDebugSelection() const   { return DebugSelection; }
        void SetDebugColumns(u32 C) {
            if (DebugColumns == C) return;
            DebugColumns = C;
            ++DebugPreviewConfigVersion;
        }
        u32  GetDebugColumns() const         { return DebugColumns; }
        // Peso por canal do alvo selecionado (isolar r/g/b/a, multiplicar). Ver FDebugTile.
        void SetDebugChannelWeight(const Vec4& W) { DebugChannelWeight = W; }
        Vec4 GetDebugChannelWeight() const   { return DebugChannelWeight; }
        void SetDebugMip(u32 Mip)            { DebugMip = Mip; }
        u32  GetDebugMip() const             { return DebugMip; }
        void SetDebugExposure(f32 E) {
            if (DebugExposure == E) return;
            DebugExposure = E;
            ++DebugPreviewConfigVersion;
        }
        f32  GetDebugExposure() const        { return DebugExposure; }
        void SetDebugProbeIndex(i32 Index);
        u32  GetDebugProbeIndex() const       { return DebugProbeIndex; }
        void SetDebugProbeSampleUV(f32 U, f32 V);
        bool ConsumeDebugProbeSample(FDebugProbeSample& OutSample);
        bool RequestDebugProbePoint(u32 X, u32 Y);
        // Reexecuta o ultimo ponto diagnosticado. Chamado pelos knobs que mudam o peso das
        // probes: sem isso o painel continua com os numeros do estado anterior do knob.
        void RepeatDebugProbePoint();
        void CancelDebugProbePoint();
        bool ConsumeDebugProbePoint(FDDGIPointDiagnostic& OutDiagnostic);
        void SetDebugProbeContributors(const FDDGIPointDiagnostic& Diagnostic);
        void SetDebugProbeContributors(const u32* Indices, const f32* Weights,
                                       u32 Count, i32 RiskSlot);
        // Descarta os contribuintes de um point-pick e volta a destacar so a probe da sessao,
        // preservando-a (ver .cpp): contagem zero no setter acima NAO limpa, e
        // SetDebugProbeIndex(-1) encerraria a sessao. Restaure a probe-base ANTES de chamar.
        void ClearDebugProbeContributors();
        void SetDebugPreviewEnabled(bool Enabled) {
            if (DebugPreviewEnabled == Enabled) return;
            DebugPreviewEnabled = Enabled;
            ++DebugPreviewConfigVersion;
        }
        // Consome a captura mais recente em RGBA8. Retorna false quando nenhum readback novo
        // ficou pronto desde a ultima chamada.
        bool ConsumeDebugPreview(std::vector<u8>& OutPixels);

        u32  GetDepthSRVSlot() const         { return Targets.DepthSRVSlot; }

        // Terreno (F1: renderizacao apenas). Carregado pelo sidecar <cena>.terrain.json no
        // LoadCookedScene, ou direto via LoadTerrain. O olho do outliner mora no FRenderSettings.
        const FTerrain& GetTerrain() const   { return Terrain; }
        bool LoadTerrain(const FTerrainDesc& Desc) {
            return Terrain.Load(Device.Native(), UploadQueue, SRVHeap, Desc);
        }

        // Telemetria da agua (janela de stats). Os knobs moraram p/ o FRenderSettings.
        const FWaterRenderer& GetWater() const { return Water; }

        Vec3 GetCameraPos() const { return Camera.GetPosition(); }
        f32  GetPitch()     const { return Camera.GetPitch(); }
        f32  GetYaw()       const { return Camera.GetYaw(); }
        // FOV vertical da camera da viewport, em radianos. FONTE UNICA: o RenderFrame monta a
        // Projection com ele e o editor dimensiona o que precisa ser constante em tela (seta do
        // gizmo, icone de luz) com ele. Eram dois literais de 60 graus em arquivos diferentes,
        // amarrados so por um comentario. Quando virar configuravel, vira membro aqui e todo
        // mundo acompanha.
        static constexpr f32 kFovYDegrees = 60.0f;
        f32  GetFovY() const { return kFovYDegrees * ToRad; }
        // Eixos da camera em mundo (colunas da view row-vector) — a mesma base que o VS dos
        // icones usa p/ expandir o billboard. O editor le p/ medir a extensao do icone EM
        // TELA (projeta a borda), em vez de carregar um raio em pixels hardcoded.
        Vec3 GetCameraRight() const {
            const Mat44 V = Camera.GetViewMatrix();
            return Vec3{ V.M[0][0], V.M[1][0], V.M[2][0] };
        }
        Vec3 GetCameraUp() const {
            const Mat44 V = Camera.GetViewMatrix();
            return Vec3{ V.M[0][1], V.M[1][1], V.M[2][1] };
        }
        // Foco de camera do editor (duplo-clique no Scene Outliner): teleporta mantendo
        // a orientacao atual. Definido no .cpp porque avisa o corte de camera, e o
        // FRenderSettings so e completo no RenderSettings.h.
        void SetCameraPose(const Vec3& Pos, f32 PitchDeg, f32 YawDeg);

        const FD3D12Device& GetDevice()  const { return Device; }
        FCommandQueue&      GetCmdQueue()      { return CommandQueue; }
        FUploadQueue&       GetUploadQueue()   { return UploadQueue; }
        const FGpuProfiler& GetGpuProfiler() const { return GpuProfiler; }

        // Passes medidos na fila de COMPUTE assincrona (frequencia/readback proprios).
        // Vazio quando o DDGI rodou na fila direta (async off/relocation) — a UI nao
        // mostra linha velha de um modo que nao esta mais rodando.
        std::vector<FGpuProfiler::FScopeResult> GetAsyncComputeTimings() const {
            if (!AsyncGIRanLastFrame) return {};
            return GpuProfilerCompute.Results();
        }
        FTextureSRVHeap&    GetSRVHeap()       { return SRVHeap; }

        FRaytracingScene&   GetRaytracingScene() { return RaytracingScene; }

        // Telemetria do DDGI (spacing, momentos de distancia, contagem de sondas) p/ o painel
        // de GI. Os knobs — inclusive os de amostragem — moraram p/ o FRenderSettings.
        const FDDGI& GetDDGI() const { return DDGI; }

        // As flags de edicao de material/luz (Mark*/Notify*) moraram p/ o FRenderSettings; o
        // RenderFrame consome MaterialRTStateDirty/IndirectLightingDirty uma vez, antes do
        // BeginFrame, ou seja, fora da gravacao do command list.

    private:
        // Knobs consumidos pelo FRenderSettings (corpos no Renderer.cpp). Ficam privados p/ que
        // exista UM caminho publico: o editor passa pela fachada, que carrega a invalidacao.
        void SetRenderScale(f32 V); // recria os RTs internos (so a cena; backbuffer fica nativo)
        void SetUseWater(bool Use);
        void SetSunDirection(const Vec3& Dir);
        void SetSunAzimuthElevation(f32 AzimuthDeg, f32 ElevationDeg);
        void SetCloudsHalfRes(bool HalfRes); // recria o RT das nuvens (flush da fila)
        void SetCloudWeatherSeed(u32 Seed);  // re-bakeia na hora (flush + dispatch sincrono)
        void SetCloudWeatherCells(u32 Mult);
        bool LoadCloudWeatherTexture(const std::wstring& Path);
        void ClearCloudWeatherTexture();

        // No-op quando ninguem registrou callback (todo caminho que nao seja o editor).
        void ReportInitProgress(std::string_view Label, std::string_view Detail,
                                f32 Fraction) const;

        // Resolve "que passes rodam neste frame" (FrameContext.h). Chamado uma vez no topo do
        // RenderFrame; todos os insumos sao membros estaveis durante a gravacao.
        FFrameModes ResolveFrameModes();
        // Camera/matrizes/jitter do frame (FrameContext.h). Precisa do upscaler ativo p/ o jitter.
        FFrameView  ResolveFrameView(const FFrameModes& Modes, IUpscaler* ActiveUp);
        // Sol, lua, chuva e a luz-chave. So calculo; as publicacoes ficam no RenderFrame.
        FFrameLighting ResolveFrameLighting();

        void RecreateAllPSOs();
        void BuildDefaultScene();
        void BuildRaytracingScene();
        void SetupGIForScene(const Vec3& AABBMin, const Vec3& AABBMax);
        // Reancora tudo que enderecava a cena por indice depois de a lista mudar de tamanho.
        // A caixa opcional e a AABB de mundo do objeto que nasceu ou morreu: so as sondas do
        // DDGI ali dentro reavaliam, em vez do atlas inteiro (ver FDDGI::InvalidateRegion).
        void OnSceneStructureChanged(const Vec3* ChangedMin = nullptr,
                                     const Vec3* ChangedMax = nullptr);
        void CreateConstantBuffer();
        void RecreateInternalTargets(); // recria RTs de cena em RenderWidth/Height (resize + render scale)

        // === Alocacao sob demanda do NRD ==================================================
        // As duas instancias (indireta e direta) somam ~336 MB de pools + IO na resolucao cheia
        // — mais da METADE da categoria "GI e reflexos" — e eram alocadas incondicionalmente,
        // inclusive no estado padrao, onde o denoiser pode ser DLSS-RR/None e os dois ReSTIR
        // nascem desligados. Aqui cada instancia so existe enquanto tem consumidor.
        bool WantNrdIndirect() const; // NRD selecionado + ReSTIR GI ligado + volume DDGI pronto
        bool WantNrdDirect() const;   // NRD selecionado + ReSTIR DI ligado
        void SetupNrdIndirect();      // (re)aloca a instancia indireta e os packs que a leem
        void SetupNrdDirect();        // idem p/ a direta
        // Reconcilia desejado x alocado. Chamada pelos setters de denoiser e dos dois ReSTIR
        // (ver FRenderSettings); no-op quando nada mudou, entao e barata de chamar a mais.
        void ReconcileNrdAllocation();
        void CreateDefaultMaterial();
        void CreateIBLDescriptorTable();
        void CreateDebugPreviewTargets();
        void CollectDebugPreviewReadback(u32 FrameSlot);

        FD3D12Device    Device;
        FCommandQueue   CommandQueue;
        FUploadQueue    UploadQueue; // fila COPY p/ uploads (texturas/meshes) sem stall
        FAsyncComputeQueue ComputeQueue; // fila COMPUTE p/ DDGI async (F3)
        FGpuProfiler    GpuProfilerCompute; // timestamps da fila de compute (DDGI async)
        bool            UseAsyncCompute = true;
        bool            AsyncGIRanLastFrame = false;
        FGpuProfiler    GpuProfiler;
        FSwapChain      SwapChain;
        FPipelineState  PipelineState;
        FTextureSRVHeap SRVHeap;

        FMaterialPreview MaterialPreview; // preview offscreen do Editor de Materiais

        FCamera Camera;

        FTexture TexDefaultWhite;
        FTexture TexDefaultNormal;
        FTexture TexDefaultBlack;
        FTexture TexDefaultORM;

        FMaterial  DefaultMaterial;
        FMaterial* ActiveMaterial = nullptr;

        FScene Scene;

        static constexpr u32     kInvalidSlot = 0xFFFFFFFFu;

        // Alvos da cena: profundidade, normais, velocity, mascaras do upscaler, cor HDR e as
        // copias p/ refracao/SSR. Vivem em RenderWidth/Height e sao recriados juntos no resize
        // e na troca de render scale — ver RecreateInternalTargets.
        FSceneTargets            Targets;


        void SetupReflectionsForScene();

        // Deferred shading: o G-buffer e a unica fonte de geometria opaca. GBufferB (OctNormal +
        // Roughness + Metallic) e byte-a-byte o antigo ReflectionGBuffer -> as reflexoes leem dele.
        FGBuffer       GBuffer;
        FDebugView     DebugViewPass;
        FDebugView     DebugPreviewPass;
        // Registra em DebugTargets os alvos que ja tem SRV. Chamado no fim de
        // RecreateInternalTargets(), pois o resize realoca slots (o registro sobrescreve por nome).
        void RegisterDebugTargets();
        // Instrumentacao de timer: um alvo por passe instrumentado, cada um no dominio do seu
        // dispatch (GI full-res, reflexao half-res).
        FShaderTimer   TimerGI;
        FShaderTimer   TimerReflections;
        // Debug da BVH: dispatch proprio, so com o toggle ligado.
        FBvhDebugView        BvhDebug;
        bool                 BvhDebugEnabled = false; // toggle do editor
        FBvhDebugView::EMode BvhDebugMode    = FBvhDebugView::EMode::Category;
        f32                  BvhDebugComplexityMax = FBvhDebugView::kDefaultComplexityMax;
        bool           RtShaderTimer      = false; // toggle do editor
        bool           TimerCaptureActive = false; // resolvido por frame (toggle && disponivel)
        // Escala do heatmap: 1/valor considerado "quente" em ciclos. UMA POR PASSE, porque os
        // dois vivem em ordens de grandeza diferentes — com o 65000 do artigo nos dois, o gather
        // do ReSTIR saturava a tela inteira de vermelho e o trace de reflexao afundava todo no
        // azul do piso. Valores lidos da primeira captura na 3060 Ti; o slider de exposicao do
        // visualizador multiplica por cima, entao recalibrar nao exige recompilar.
        // GI: full-res, 1 raio + reuso temporal + revalidacao — o pixel tipico ja passa de 65k.
        static constexpr f32 kShaderTimerScaleGI = 1.0f / 250000.0f;
        // Reflexao: half-res e a maioria dos pixels sai cedo por rugosidade (sem traçar raio),
        // entao o interessante mora numa faixa bem mais baixa.
        static constexpr f32 kShaderTimerScaleReflections = 1.0f / 32000.0f;

        u32            GBufferDebugMode = 0;
        u32            DebugTargetIndex   = kNoDebugTarget;
        std::vector<u32> DebugSelection;          // janela de debug: varios alvos em grade offscreen
        u32            DebugColumns       = 0;    // 0 = grade automatica ~quadrada
        Vec4           DebugChannelWeight = Vec4{ 1.0f, 1.0f, 1.0f, 1.0f };
        u32            DebugMip           = 0;
        u32            DebugProbeIndex    = kNoDebugProbe;
        f32            DebugProbeSampleU  = -1.0f;
        f32            DebugProbeSampleV  = -1.0f;
        FDebugProbeSample DebugProbeSampleResult{};
        bool            DebugProbeSampleReady = false;
        // MULTIPLICADOR sobre a exposicao padrao de cada alvo (FDebugTarget::Exposure). Fica
        // em 1.0 ate existir o slider na janela de debug; um valor global unico nao servia,
        // porque cada sinal vive numa magnitude diferente.
        f32            DebugExposure      = 1.0f;
        ComPtr<ID3D12Resource> DebugPreviewTarget;
        ComPtr<ID3D12Resource> DebugPreviewReadback[FCommandQueue::kFramesInFlight];
        ComPtr<ID3D12Resource> DebugProbeSampleReadback[FCommandQueue::kFramesInFlight];
        FDescriptorHeap        DebugPreviewRTVHeap;
        bool                   DebugPreviewReadbackPending[FCommandQueue::kFramesInFlight] = {};
        u64                    DebugPreviewReadbackVersion[FCommandQueue::kFramesInFlight] = {};
        bool                   DebugProbeSamplePending[FCommandQueue::kFramesInFlight] = {};
        u64                    DebugProbeSampleVersion[FCommandQueue::kFramesInFlight] = {};
        u32                    DebugProbeSampleIndex[FCommandQueue::kFramesInFlight] = {};
        std::vector<u8>        DebugPreviewPixels;
        bool                   DebugPreviewPixelsReady = false;
        bool                   DebugPreviewEnabled = false;
        u64                    DebugPreviewConfigVersion = 1;
        u64                    DebugPreviewLastCapturedVersion = 0;

        // Motion vector buffer (RG16F): escrito no geometry pass (SV_Target3), lido pelo TAA.
        // RT proprio (lifecycle desacoplado das transicoes do GBuffer, que fazem ping-pong p/ as
        // reflexoes). Velocidade em UV = curUV(sem jitter) - prevUV.
        // Perfil unico de epsilons de raio, empurrado p/ ReSTIR/Reflexoes/DDGI todo frame.
        FRayEpsilonProfile       RayEps;
        // Transform por-objeto do frame anterior, indexado pelo indice da cena (Scene.Renderables()).
        // Estatico -> PrevModel == Model -> motion vector reduz ao termo de camera.
        std::vector<Mat44>       PrevModels;

        ComPtr<ID3D12Resource>   ConstantBuffer;
        u8*                      MappedFrameBase = nullptr;

        // Luzes puntuais: upload persistente com kMaxLights por frame em voo, escrito no
        // RenderFrame (coleta+cull da FScene) e lido pelo deferred lighting via root SRV t17.
        static constexpr u32     kMaxLights = 256;
        ComPtr<ID3D12Resource>   LightBuffer;
        u8*                      MappedLightBase = nullptr;
        // Posicao anterior por identidade estavel. Alimenta o shadow motion vector; separar por
        // Id evita que frustum culling/reordenacao da lista transforme uma luz em outra.
        std::unordered_map<u64, Vec3> PreviousDirectLightPositions;

        // F5: lista compacta pro mundo indireto (sem cull/sombra), um slice por frame em voo,
        // com um SRV de staging por slice — copiado por frame pras tabelas de trace do
        // DDGI/reflexoes/ReSTIR (SetPunctualLightsSRV de cada um).
        ComPtr<ID3D12Resource>   GILightBuffer;
        u8*                      MappedGILightBase = nullptr;
        u32                      GILightSRVSlot[FCommandQueue::kFramesInFlight] = {};

        u32                      MaxObjects = 1024;
        ComPtr<ID3D12Resource>   ObjectCB;          
        u8*                      MappedObjectCB = nullptr;
     
        void RecreateObjectCB();

        std::vector<std::unique_ptr<FTexture>>  ImportedTextures;
        std::vector<std::unique_ptr<FMaterial>> ImportedMaterials;

        bool UseFrustumCulling = true;
        bool UseDepthPrepass   = false;
        f32  RenderScale       = 1.0f; // SSAA: cena em swapchain*RenderScale; backbuffer nativo
        u32  LastVisibleCount  = 0;

        FPostProcessor           PostProcessor;

        FObjectPicker            ObjectPicker;
        FSelectionOutline        SelectionOutline;
        // Selecao UNICA da cena (mesh ou luz — nunca as duas). O Index dentro dela e o cache
        // que o loop de draw compara por frame; o Id e o que sobrevive a lista mudar e o que o
        // OnSceneStructureChanged usa para reancorar.
        FSceneObjectRef          Selection;
        // AABB de uniao da cena carregada, como o SceneLoader calculou. Guardado porque um
        // re-setup de GI fora do load (objeto criado estourando alguma capacidade) precisa do
        // mesmo volume — recalcular ali daria um grid de DDGI diferente do que a cena vinha
        // usando, e todo o indireto se deslocaria por causa de uma copia de objeto.
        Vec3                     SceneBoundsMin{ 0.0f, 0.0f, 0.0f };
        Vec3                     SceneBoundsMax{ 0.0f, 0.0f, 0.0f };

        FDebugDraw               DebugDraw;
        Mat44                    LastViewProj{}; 

        FTemporalAA              TemporalAA;
        bool                     UseTAA           = true;
        f32                      TAAHistoryBlend  = 0.9f;
        f32                      TAAVarianceGamma = 1.25f; 
        f32                      TAASharpness     = 0.2f;  
        f32                      TAAMotionBlend   = 0.7f;
        f32                      TAAAntiFlicker   = 0.6f;
        f32                      TAAStationaryMargin = 4.0f; // margem do AABB do history parado (ref Flax); 0 desliga
        u32                      TAADebugMode     = 0;
        Mat44                    PrevViewProj{};
        Vec3                     PrevCameraPosition{ 0.0f, 0.0f, 0.0f };
        bool                     TAARanLastFrame = false;

        // === Upscaler (None/FSR/DLSS) — substitui o TAA custom quando != None ===
        // FSR (ffx-api) e DLSS (Streamline) implementam IUpscaler; ambos viram stub se o SDK nao for
        // achado no CMake. DLSS so fica disponivel em NVIDIA c/ suporte (senao cai p/ FSR/None).
        FFsrPass                 Fsr;
        FDlssPass                Dlss;
        EUpscaler                Upscaler = EUpscaler::FSR; // selecionado (cai p/ None se indisponivel)
        // Padrao = 0 (100%): o upscaler reconstroi/faz AA na resolucao nativa, sem upscale. Decisao
        // de produto — os tiers de upscale ficam como opt-in do usuario. Manter em sincronia com
        // ViewportWidget::ResetRenderSettings().
        int                      UpscalerQuality = 0;       // 0=100% 1=Quality 2=Balanced 3=Perf 4=Ultra

        // Upscaler pronto p/ dispatch (output criado). None/indisponivel/nao-inicializado => nullptr.
        // Com o denoiser em DLSS_RR, o passe ATIVO e o proprio RR (faz denoise+upscale) — reusa todo o
        // plumbing display-res -> post do FSR/DLSS-SR.
        IUpscaler* ActiveUpscaler() {
            // Os guides entram na conta: sem eles o Dispatch do RR aborta (guides nulos) sem escrever o
            // output, e o PostInput apontaria p/ textura estagnada. Devolver nullptr aqui degrada p/ o
            // caminho sem upscale (TAA/nativo), que e coerente, em vez de uma tela suja + log por frame.
            if (Denoiser == EDenoiser::DLSS_RR)
                return (DlssRR.IsInitialized() && RRGuides.IsReady()) ? static_cast<IUpscaler*>(&DlssRR)
                                                                      : nullptr;
            switch (Upscaler) {
                case EUpscaler::FSR:  return Fsr.IsInitialized()  ? static_cast<IUpscaler*>(&Fsr)  : nullptr;
                case EUpscaler::DLSS: return Dlss.IsInitialized() ? static_cast<IUpscaler*>(&Dlss) : nullptr;
                default:              return nullptr;
            }
        }
        // Razao render/display pura do upscaler/denoiser SELECIONADO (independe de estar inicializado).
        void ApplyUpscalerScale() {
            f32 R = 1.0f;
            if      (Denoiser == EDenoiser::DLSS_RR) R = DlssRR.RenderRatioForQuality(UpscalerQuality);
            else if (Upscaler == EUpscaler::FSR)     R = Fsr.RenderRatioForQuality(UpscalerQuality);
            else if (Upscaler == EUpscaler::DLSS)    R = Dlss.RenderRatioForQuality(UpscalerQuality);
            ApplyRenderScale(R);   // worker: escapa o gate do RR (esta razao E a ditada pelo modo)
        }
        void ApplyRenderScale(f32 V);   // aplica de fato (clamp + flush + RecreateInternalTargets)

        FFlickerHeatmap          Flicker;
        u32                      FlickerMode         = 0;      
        f32                      FlickerScale        = 0.30f;  
        f32                      FlickerAlpha        = 0.08f;  
        bool                     FlickerResetPending = false;  

        FHDREnvironment HDREnv;
        FSkybox         Skybox;
        FAtmosphere     Atmosphere;
        bool            UseAtmosphereSky = true;
        // Sol/lua atenuados por pixel na altitude da SUPERFICIE, em vez de uma cor unica por
        // frame calculada na altitude da camera. Botao do A/B (AtmoLightParams.w).
        bool            UsePerPixelAtmoTransmittance = true;
        // Ambiente do ceu em SH-L1 (direcional) no lugar das 2 cores chapadas. Botao do A/B.
        bool            UseSkyAmbientSH = true;

        FFogPass        Fog;
        FVolumetricFogPass VolumetricFog;
        bool            UseVolumetricFog     = true; // froxel fog; exige height fog ON
        bool            UseAerialPerspective = true;
        bool            UseHeightFog         = true;

        FSunShafts      SunShafts;
        bool            UseSunShafts = true; // raymarch meia-res + temporal; exige height fog ON

        FWeather        Weather;     // estado de clima (chuva) — editor escreve, chuva le
        FRainWetness    RainWetness; // F1: wetness deferred no G-buffer (pos-geometry pass)

        FSunShadows     SunShadows;
        u64             DraggingRenderableId = 0; // ver SetDraggingRenderable
        bool            UseSunShadows = true;

        FLocalShadows   LocalShadows; // sombras de spot (F3a); budget kMaxShadows/frame
 
        FRaytracingScene RaytracingScene;
        u64              TlasTransformsVersion = 0; // versao da cena na ultima (re)build da TLAS
        bool             TlasFlagsDirty        = false; // flags de instancia mudaram (edicao de material)
        bool             MaterialRTStateDirty  = false; // pedido de refresh coalescido p/ o proximo frame
        bool             IndirectLightingDirty = false; // idem, so invalidacao (ver MarkIndirectLightingDirty)
        FDDGI            DDGI;
        FReGIR           ReGIR;
        // Fase 1 do projeto de mesh lights: so levanta a contagem de triangulos emissivos por
        // cena. Ainda nao produz luz — existe para medir antes de escolher a amostragem.
        FMeshLights      MeshLights;
        bool             UseReGIR = false; // bring-up: hits secundarios; default OFF para A/B
        FDDGIDebug       DDGIDebugPass; 
        bool             UseGI       = true;
        bool             GIDebug     = false; 
        bool             GIChebyshev = true;  
        bool             GISkipInactiveProbes = true; 
        bool             GISkipInactiveFallback = false; 

        FReSTIRGI        ReSTIRGI;
        bool             UseReSTIRGI = false; // experimental; default OFF (nao toca o estado padrao)
        FNrdDenoiser     Nrd;                 // denoiser do ReSTIR GI (RELAX difuso+especular)
        FDlssRRPass      DlssRR;              // DLSS Ray Reconstruction (denoise+upscale num eval)
        FDlssRRGuides    RRGuides;            // buffers de material que o RR consome (albedo/normal/hitDist)
        FBackgroundVelocity BgVelocity;       // motion vector do ceu/nuvens/fog (velocity ZERO do G-buffer)
        FTemporalMotionVectors TemporalMotion; // vetor dual + historico de superficies (RT Gems II cap. 25)
        Mat44            PrevVPNoTrans{};      // frame anterior: ViewNoTrans * ProjUnjittered (reproj do ceu)
        bool             RRResetPending = true;// descarta o historico do RR (troca de modo/scene/resize)
        // Borda de log do "RR pulado por debug na cena" (ver RRPoisoned no RenderFrame): sem isto o
        // aviso sairia todo frame enquanto o visualizador estivesse ligado.
        bool             RRSkipLogged   = false;
        EDenoiser        Denoiser = EDenoiser::None; // {None, NRD, DLSS_RR}; default = sem denoise
        Mat44            NrdPrevView{};        // prev view/proj NAO-jitteradas p/ a reprojecao do NRD
        Mat44            NrdPrevProj{};
        Vec2             PrevJitterUv{ 0.0f, 0.0f }; // jitter do frame anterior em UV (y ja invertido
                                                     // p/ uv y-down) — reprojecao do ReSTIR GI
        Vec2             PrevJitterPx{ 0.0f, 0.0f }; // idem em pixels — cameraJitterPrev do NRD

        FReflections     Reflections;
        bool             UseReflections = true;

        FReSTIRDI        ReSTIRDI;
        FNrdDenoiser     NrdDirect; // RELAX dedicado ao DI: historico/tuning nao contaminam GI/refl
        bool             UseReSTIRDI = false; // bring-up: substitui TODA a direta local; default OFF
        // SRV do LightBuffer (FGPULight) por frame em voo. O deferred le esse buffer como root SRV
        // e por isso nao precisava de slot no heap; o ReSTIR DI le por tabela.
        u32              DirectLightSRVSlot[FCommandQueue::kFramesInFlight] = { kInvalidSlot, kInvalidSlot };
        // Nº de luzes escritas no LightBuffer deste frame. Sai do bloco de empacotamento para os
        // dispatches de direta local; ler alem traria luz de lixo do frame anterior.
        u32              FrameLightCount = 0;
        u64              FrameLightSetSignature = 0; // IDs na ordem do buffer; invalida indices antigos

        FAmbientOcclusion AO;
        bool              UseAO   = true;
        bool              AODebug = false;

        FHiZOcclusion     HiZ; // occlusion culling HZB (build + teste + readback ring)
        bool              UseOcclusionCulling = true;
        u32               LastOccludedCount   = 0;
        static constexpr f32 kKmPerWorldUnit = 0.001f;

        FCloudNoise       CloudNoise;
        FVolumetricClouds VolumetricClouds;
        bool              UseClouds = false; // off por padrao: caro e ainda nao otimizado

        // Multi-cascata: 3 sims FFT em escalas de tile T0/6·T0/24·T0 com bandas de
        // espectro disjuntas — detalhe, mar médio e swell (mata o tiling de escala única).
        static constexpr u32 kOceanCascades = FWaterRenderer::kFFTCascades;
        FOceanFFT         Ocean[kOceanCascades];
        FWaterRenderer    Water;
        bool              UseWater  = false;

        FTerrain          Terrain;
        bool              UseTerrain = true; // olho do Scene Outliner (so raster; proxy RT
                                             // e escondido pelo Visible do renderable proxy)


        bool            ShowSkybox    = true;
        f32             IBLIntensity  = 1.0f;
        f32             IBLRotation   = 0.0f; 
        u32             IBLTableStart = 0;

        Vec3 SunDir       = { 0.3f, 0.6f, 0.5f }; 
        Vec3 SunColorRGB  = { 1.0f, 0.96f, 0.9f };
        f32  SunIntensity = 5.0f;

        FTimeOfDay TimeOfDay;

        bool UseAtmosphereAmbient  = true;
        f32  AtmoAmbientIntensity  = 1.0f;

        f32  ElapsedTime   = 0.0f;
        f32  LastDeltaTime = 0.0f;
        u32  FrameIndex    = 0;

        bool Initialized = false;

        // Vive so durante Initialize: o editor a instala antes e limpa depois de inicializar.
        FInitProgressCallback InitProgressCallback;

        // Por ponteiro porque o FRenderSettings so e completo no RenderSettings.h, que por sua
        // vez precisa deste header. Construida no ctor; o dtor do Renderer e out-of-line (ja
        // era) para que o unique_ptr veja o tipo completo.
        std::unique_ptr<FRenderSettings> SettingsImpl;
    };
} 
