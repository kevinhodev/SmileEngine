#pragma once

#include <Windows.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Debug/GpuProfiler.h"
#include "Smile/Graphics/Backend/D3D12/PipelineState.h"
#include "Smile/Graphics/Scene/Camera.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Scene/GBuffer.h"
#include "Smile/Graphics/Renderer/SceneTargets.h"
#include "Smile/Graphics/Renderer/FrameContext.h"
#include "Smile/Graphics/Debug/FrameCapture.h"
#include "Smile/Graphics/Renderer/PassContext.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include "Smile/Graphics/Debug/DebugView.h"
#include "Smile/Graphics/Debug/ShaderTimer.h"
#include "Smile/Graphics/Debug/BvhDebugView.h"
#include "Smile/Graphics/Environment/HDREnvironment.h"
#include "Smile/Graphics/Environment/Atmosphere.h"
#include "Smile/Graphics/Environment/TimeOfDay.h"
#include "Smile/Graphics/Environment/CloudNoise.h"
#include "Smile/Graphics/Environment/VolumetricClouds.h"
#include "Smile/Graphics/Environment/Skybox.h"
#include "Smile/Graphics/Environment/Fog.h"
#include "Smile/Graphics/Environment/VolumetricFog.h"
#include "Smile/Graphics/Environment/SunShafts.h"
#include "Smile/Graphics/Environment/Weather.h"
#include "Smile/Graphics/Environment/RainWetness.h"
#include "Smile/Graphics/Lighting/SunShadows.h"
#include "Smile/Graphics/Lighting/LocalShadows.h"
#include "Smile/Graphics/RayTracing/RaytracingScene.h"
#include "Smile/Graphics/GI/GIFallback.h"
#include "Smile/Graphics/GI/IndirectPolicy.h"
#include "Smile/Graphics/GI/DDGI.h"
#include "Smile/Graphics/GI/DDGIDebug.h"
#include "Smile/Graphics/GI/ReSTIRGI.h"
#include "Smile/Graphics/GI/ReGIR.h"
#include "Smile/Graphics/GI/RadianceCache.h"
#include "Smile/Graphics/Lighting/MeshLights.h"
#include "Smile/Graphics/Lighting/ReSTIRDI.h"
#include "Smile/Graphics/GI/NrdDenoiser.h"
#include "Smile/Graphics/GI/Reflections.h"
#include "Smile/Graphics/GI/AmbientOcclusion.h"
#include "Smile/Graphics/Scene/HiZOcclusion.h"
#include "Smile/Graphics/PostProcess/PostProcess.h"
#include "Smile/Graphics/Editor/MaterialPreview.h"
#include "Smile/Graphics/Editor/Picking.h"
#include "Smile/Graphics/Editor/SelectionOutline.h"
#include "Smile/Graphics/Editor/DebugDraw.h"
#include "Smile/Graphics/PostProcess/TemporalAA.h"
#include "Smile/Graphics/PostProcess/FsrPass.h"
#include "Smile/Graphics/PostProcess/DlssPass.h"
#include "Smile/Graphics/PostProcess/DlssRRPass.h"
#include "Smile/Graphics/PostProcess/DlssRRGuides.h"
#include "Smile/Graphics/PostProcess/BackgroundVelocity.h"
#include "Smile/Graphics/PostProcess/TemporalMotionVectors.h"
#include "Smile/Graphics/Debug/FlickerHeatmap.h"
#include "Smile/Graphics/Water/OceanFFT.h"
#include "Smile/Graphics/Water/Water.h"
#include "Smile/Graphics/Scene/Terrain.h"
#include "Smile/Scene/Scene.h"

namespace Smile {
    struct CameraInput;
    struct FPreparedCookedScene;
    class FRenderBackend;
    using FPreparedCookedScenePtr = std::shared_ptr<FPreparedCookedScene>;

    // Fachada de configuracao. Fica incompleta aqui para evitar dependencia circular.
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

        // x = escala do self-shadow bias; y = teto em metros (0 = sem teto).
        Vec4  DDGIBiasParams;

        // Transmitancia atmosferica por pixel. w = habilitado.
        // x = raio do planeta (km), y = raio do topo da atmosfera (km),
        // z = km por unidade de mundo.
        Vec4  AtmoLightParams;
        // Cores sem transmitancia nem HorizonFade, usadas no caminho por pixel.
        Vec4  SunColorRaw;        // rgb = cor base * dim de chuva, w = -
        Vec4  MoonColorRaw;       // rgb = tint da lua * dim de chuva, w = -

        // Ambiente do ceu em SH-L1: um float4 de coeficientes por canal.
        Vec4  SkyAmbientSHR;
        Vec4  SkyAmbientSHG;
        Vec4  SkyAmbientSHB;
        Vec4  SkyAmbientSHParams; // x = usar SH (0 = 2 cores chapadas), yzw = -

        // Cascatas compartilhadas pelos consumidores de GI. Deve permanecer no fim do cbuffer.
        FDDGICascadeConstants DDGICascades;
    };
    // Protege o layout compartilhado com os shaders.
    static_assert(offsetof(FrameConstants, DDGICascades) == 496,
                  "o bloco de cascatas deve permanecer anexado ao fim do FrameConstants");

    // Espelha FGPULight em DeferredLighting.ps.hlsl.
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

    // Formato compacto de luz para GI; visibilidade e resolvida por shadow ray.
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

    // Snapshot independente do backend para telemetria de memoria da GPU.
    struct FRendererGpuMemoryInfo {
        u64  LocalUsage     = 0;
        u64  LocalBudget    = 0;
        u64  NonLocalUsage  = 0;
        u64  NonLocalBudget = 0;
        u64  DemotedBytes   = 0;
        bool OverBudget     = false;
        bool Valid          = false;
    };

    // Trabalhos de sombra local produzidos e consumidos dentro do frame.
    struct FLocalShadowJobs {
        std::vector<FLocalShadows::FShadowJob>     Spot;
        std::vector<FLocalShadows::FCubeShadowJob> Cube;
    };

    class Renderer {
        // FRenderSettings roteia configuracao e invalidacao sem expor o estado interno.
        friend class FRenderSettings;

    public:
        Renderer();
        ~Renderer() noexcept;

        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Ponto unico para parametros de render e suas invalidacoes.
        FRenderSettings&       Settings();
        const FRenderSettings& Settings() const;

        // Executado na thread de Initialize; o callback nao deve reentrar no Renderer.
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
        // Cancela capturas antes da compilacao assincrona; ReloadShaders faz a recriacao.
        void NotifyShaderReloadQueued(const std::string& ChangedStem = "");

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

        // Telemetria de culling (os toggles moraram p/ o FRenderSettings).
        u32  GetOccludedCount() const    { return LastOccludedCount; }
        u32  GetVisibleCount() const     { return LastVisibleCount; }
        u32  GetDrawCount() const        { return static_cast<u32>(Scene.Renderables().size()); }

        // Telemetria do CSM por cascata (contagem + frequencia de atualizacao). Const, so
        // leitura: e a base de medida da separacao static/dynamic dos casters.
        const FSunShadows& GetSunShadows() const { return SunShadows; }

        // Durante o arraste, o objeto e tratado como caster dinamico. Zero = nenhum.
        void SetDraggingRenderable(u64 Id) { DraggingRenderableId = Id; }
        u64  GetDraggingRenderable() const { return DraggingRenderableId; }


        bool IsInitialized() const { return Initialized; }

        // Resolucao interna e de saida. DLSS RR define sua propria escala de entrada.
        u32  RenderWidth() const;
        u32  RenderHeight() const;
        u32  OutputWidth() const;
        u32  OutputHeight() const;
        u32  GetFrameIndex() const { return FrameIndex; }

        // Picking: o ID pass roda em res interna -> escala a coord do mouse (nativa) por RenderScale.
        void RequestPick(u32 X, u32 Y) {
            ObjectPicker.RequestPick(static_cast<u32>(X * RenderScale + 0.5f),
                                     static_cast<u32>(Y * RenderScale + 0.5f));
        }
        bool TryGetPickResult(int& OutIndex) { return ObjectPicker.TryResolve(OutIndex); }
        // O indice atende ao draw; o Id mantem a selecao estavel apos alteracoes na cena.
        void SetSelectedObject(int Index);
        int  GetSelectedObject() const;
        u64  GetSelectedObjectId() const; // 0 quando o selecionado nao e um renderavel
        void ClearSelection();
        // Alteram a cena e sincronizam os subsistemas indexados por renderable.
        bool RemoveRenderable(u64 Id);
        u64  DuplicateRenderable(u64 Id);

        // Invalida apenas a regiao afetada do DDGI. Em mudancas espaciais, informe os bounds
        // antigo e novo; mudancas de energia tambem exigem MarkSceneContentDirty().
        void NotifyGIRegionChanged(const Vec3& Min, const Vec3& Max, EGIRegionChange Change);

        // Drena as filas e recria o volume DDGI com os bounds atuais da cena.
        void RebuildGIVolume() { SetupGIForScene(SceneBoundsMin, SceneBoundsMax); }

        // Selecao exclusiva de luz. Indice em Scene.Lights(); -1 = nenhuma.
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

        // Visualizador de render targets. O alvo escolhido tem prioridade sobre GBufferDebugMode.
        static constexpr u32 kNoDebugTarget = 0xFFFFFFFFu;
        static constexpr u32 kNoDebugProbe  = 0xFFFFFFFFu;
        void SetDebugTargetIndex(u32 Index)  { DebugTargetIndex = Index; }
        u32  GetDebugTargetIndex() const     { return DebugTargetIndex; }

        // Timer por pixel dos passes RT; disponivel apenas com suporte NVAPI.
        void SetRtShaderTimer(bool V)        { RtShaderTimer = V; }
        bool GetRtShaderTimer() const        { return RtShaderTimer; }
        // false em GPU nao-NVIDIA ou build sem o SDK: o editor deve desabilitar o toggle.
        bool IsRtShaderTimerAvailable() const;

        // Visualizacao portatil de conteudo e densidade da TLAS.
        void SetBvhDebug(bool V)                       { BvhDebugEnabled = V; }
        bool GetBvhDebug() const                       { return BvhDebugEnabled; }
        void SetBvhDebugMode(FBvhDebugView::EMode V)   { BvhDebugMode = V; }
        FBvhDebugView::EMode GetBvhDebugMode() const   { return BvhDebugMode; }
        // Teto do heatmap do modo Complexidade, em triangulos testados por raio.
        void SetBvhDebugComplexityMax(f32 V)           { BvhDebugComplexityMax = V < 1.0f ? 1.0f : V; }
        f32  GetBvhDebugComplexityMax() const          { return BvhDebugComplexityMax; }
        // false sem suporte a RT ou antes da TLAS existir: o editor desabilita o toggle.
        bool IsBvhDebugAvailable() const;

        // Preview offscreen de multiplos alvos. Colunas 0 escolhe uma grade automatica.
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
        // Limpa contribuintes preservando a probe-base da sessao.
        void ClearDebugProbeContributors();
        void SetDebugPreviewEnabled(bool Enabled) {
            if (DebugPreviewEnabled == Enabled) return;
            DebugPreviewEnabled = Enabled;
            ++DebugPreviewConfigVersion;
        }
        // Consome a captura mais recente em RGBA8. Retorna false quando nenhum readback novo
        // ficou pronto desde a ultima chamada.
        bool ConsumeDebugPreview(std::vector<u8>& OutPixels);

        // Captura assincrona e deterministica. O chamador define a pose antes do request.
        bool RequestCapture(const FCaptureRequest& Request) { return Capture.Request(Request); }
        bool CaptureBusy() const              { return Capture.Busy(); }
        u32  CaptureWarmupRemaining() const   { return Capture.WarmupRemaining(); }
        bool ConsumeCaptureResult(FFrameCapture::FResult& Out) { return Capture.ConsumeResult(Out); }

        u32  GetDepthSRVSlot() const         { return Targets.DepthSRVSlot; }

        // Terreno (F1: renderizacao apenas). Carregado pelo sidecar <cena>.terrain.json no
        // LoadCookedScene, ou direto via LoadTerrain. O olho do outliner mora no FRenderSettings.
        const FTerrain& GetTerrain() const   { return Terrain; }
        bool LoadTerrain(const FTerrainDesc& Desc);

        // Telemetria da agua (janela de stats). Os knobs moraram p/ o FRenderSettings.
        const FWaterRenderer& GetWater() const { return Water; }

        Vec3 GetCameraPos() const { return Camera.GetPosition(); }
        f32  GetPitch()     const { return Camera.GetPitch(); }
        f32  GetYaw()       const { return Camera.GetYaw(); }
        // Fonte unica do FOV vertical da viewport.
        static constexpr f32 kFovYDegrees = 60.0f;
        f32  GetFovY() const { return kFovYDegrees * ToRad; }
        // Eixo direito da camera em mundo (coluna da view row-vector).
        Vec3 GetCameraRight() const {
            const Mat44 V = Camera.GetViewMatrix();
            return Vec3{ V.M[0][0], V.M[1][0], V.M[2][0] };
        }
        // Foco de camera do editor (duplo-clique no Scene Outliner): teleporta mantendo
        // a orientacao atual. Definido no .cpp porque avisa o corte de camera, e o
        // FRenderSettings so e completo no RenderSettings.h.
        void SetCameraPose(const Vec3& Pos, f32 PitchDeg, f32 YawDeg);

        const std::wstring& GetGpuDescription() const;
        u64 GetDedicatedVideoMemory() const;
        FRendererGpuMemoryInfo GetGpuMemoryInfo() const;

        // Atualiza os descritores de um material sem expor device ou heap ao chamador.
        void UpdateMaterialTextureSlot(FMaterial& Material, u32 LocalSlot, FTexture* Texture);

        const FGpuProfiler& GetGpuProfiler() const;

        // Passes medidos na fila de COMPUTE assincrona (frequencia/readback proprios).
        // Vazio quando o DDGI rodou na fila direta (async off/relocation) — a UI nao
        // mostra linha velha de um modo que nao esta mais rodando.
        std::vector<FGpuProfiler::FScopeResult> GetAsyncComputeTimings() const;
        // Telemetria do DDGI (spacing, momentos de distancia, contagem de sondas) p/ o painel
        // de GI. Os knobs — inclusive os de amostragem — moraram p/ o FRenderSettings.
        const FDDGI& GetDDGI() const { return DDGI; }

        // As flags de edicao de material/luz (Mark*/Notify*) moraram p/ o FRenderSettings; o
        // RenderFrame consome MaterialRTStateDirty/IndirectLightingDirty uma vez, antes do
        // BeginFrame, ou seja, fora da gravacao do command list.

    private:
        // Construido primeiro e destruido por ultimo; todos os recursos GPU dependem dele.
        std::unique_ptr<FRenderBackend> Backend;

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

        // Resolve a politica efetiva e invalida historicos nas transicoes de dominio.
        FEffectiveIndirectPolicy ResolveIndirectPolicy();
        // Congela os modos que todos os passes observarao neste frame.
        FFrameModes ResolveFrameModes(const FEffectiveIndirectPolicy& Policy);
        // Camera/matrizes/jitter do frame (FrameContext.h). Precisa do upscaler ativo p/ o jitter.
        FFrameView  ResolveFrameView(const FFrameModes& Modes, IUpscaler* ActiveUp);
        // Sol, lua, chuva e a luz-chave. So calculo; as publicacoes ficam no RenderFrame.
        FFrameLighting ResolveFrameLighting();

        // === Fase de UPDATE do frame — tudo que corre ANTES de existir command list =======
        // Estas quatro sao a primeira fase extraida do RenderFrame. O criterio do corte nao foi
        // estetico: e que ate a linha do `CommandQueue.List()` nao ha gravacao nenhuma, so CPU
        // mexendo em estado e em constant buffer. Isso torna a fase inteira testavel e movivel
        // sem tocar em barreira, RTV ou root signature — que e o que faz dela a primeira.

        // Avanca o relogio do mundo. Roda ANTES do ResolveFrameLighting porque a direcao do sol
        // e a molhadura que ele le saem daqui.
        void TickWorldClock();

        // Publica no constant buffer do frame tudo que ja esta resolvido, e nos parametros de
        // noite/estrelas da atmosfera. Devolve o ambiente hemisferico porque ele e calculado no
        // mesmo integral que produz a SH publicada aqui — e cinco subsistemas o consomem depois.
        FFrameAmbient PublishFrameConstants(const FFrameView& View, const FFrameLighting& Light,
                                            const FEffectiveIndirectPolicy& Policy,
                                            u32 FrameSlot, FrameConstants* MappedCB);

        // Empurra o estado do frame para os passes de RT (slot do timer, perfil de epsilons,
        // amostragem do 2o bounce, quem denoisa). Um lugar so: sem isto cada passe teria a
        // propria copia e um sweep de calibracao mexeria em metade deles.
        void PushRayTracingFrameState(const FFrameModes& Modes,
                                      const FEffectiveIndirectPolicy& Policy);

        // Atmosfera, height fog, froxel, sun shafts, nuvens e a projecao da sombra de nuvem.
        // Recebe o ambiente porque o froxel e as nuvens integram sobre ele.
        void UpdateAtmosphereAndVolumetrics(const FFrameModes& Modes,
                                            const FEffectiveIndirectPolicy& Policy,
                                            const FFrameView& View,
                                            const FFrameLighting& Light,
                                            const FFrameAmbient& Ambient, u32 FrameSlot,
                                            FrameConstants* MappedCB);

        // Agua e as tres cascatas de FFT do oceano.
        void UpdateWaterAndOcean(const FFrameModes& Modes, const FFrameView& View,
                                 const FFrameLighting& Light, const FFrameAmbient& Ambient,
                                 u32 FrameSlot);

        // Fases de gravacao: todas consomem o mesmo snapshot imutavel do frame.
        FPassContext MakePassContext(const FFrameModes& Modes,
                                     const FEffectiveIndirectPolicy& Policy,
                                     const FFrameView& View,
                                     const FFrameLighting& Light, const FFrameAmbient& Ambient,
                                     u32 FrameSlot);

        void BeginSceneRecording(FPassContext& Ctx);
        void RecordSkyAndClouds(FPassContext& Ctx);
        void PrepareIndirectLighting(FPassContext& Ctx);
        FLocalShadowJobs PackDirectLights(FPassContext& Ctx, FrameConstants* MappedCB);
        void       BuildDrawLists(FPassContext& Ctx);
        void       RecordShadows(FPassContext& Ctx, const FLocalShadowJobs& Jobs);
        void       RecordDepthPrepass(FPassContext& Ctx);
        void       RecordGBuffer(FPassContext& Ctx);
        void       RecordSceneLighting(FPassContext& Ctx);
        void       RecordForwardAndClouds(FPassContext& Ctx);
        void       RecordVolumetricsAndRain(FPassContext& Ctx);
        bool       RecordDebugViews(FPassContext& Ctx, bool DDGIDebugDrew);
        FPostInput RecordResolve(FPassContext& Ctx, IUpscaler* ActiveUp, bool RRPoisoned);
        void       RecordPost(FPassContext& Ctx, FPostInput In);

        void RecreateAllPSOs();
        void BuildDefaultScene();
        void BuildRaytracingScene();
        // Reconstrucao completa; drena as filas e exige command list fechado.
        void RebuildMeshLights();
        void SetupGIForScene(const Vec3& AABBMin, const Vec3& AABBMax);
        // Reancora tudo que enderecava a cena por indice depois de a lista mudar de tamanho.
        // A caixa opcional e a AABB de mundo do objeto que nasceu ou morreu: so as sondas do
        // DDGI ali dentro reavaliam, em vez do atlas inteiro (ver FDDGI::InvalidateRegion).
        void OnSceneStructureChanged(const Vec3* ChangedMin = nullptr,
                                     const Vec3* ChangedMax = nullptr);
        void CreateConstantBuffer();
        void RecreateInternalTargets(); // recria RTs de cena em RenderWidth/Height (resize + render scale)

        // Registro nao-proprietario dos subsistemas que implementam FRenderPass.
        FPassRegistry     Passes;
        void              RegisterPasses();          // chamado uma vez no Initialize
        FPassInitContext  MakePassInitContext();     // device + heap + alvos + render-res

        // NRD e alocado sob demanda; cada instancia existe apenas com consumidor ativo.
        bool WantNrdIndirect() const;
        bool WantNrdDirect() const;   // NRD selecionado + ReSTIR DI ligado
        // Bindings de setup sempre validos: DDGI pronto ou recursos neutros de fallback.
        FGIFallbackBindings GIFallbackBindingsForSetup() const;
        void SetupNrdIndirect();      // (re)aloca a instancia indireta e os packs que a leem
        void SetupNrdDirect();        // idem p/ a direta
        // Reconcilia desejado x alocado. Chamada pelos setters de denoiser e dos dois ReSTIR
        // (ver FRenderSettings); no-op quando nada mudou, entao e barata de chamar a mais.
        void ReconcileNrdAllocation();
        void CreateDefaultMaterial();
        void CreateIBLDescriptorTable();
        void CreateDebugPreviewTargets();
        void CollectDebugPreviewReadback(u32 FrameSlot);

        // Captura: prepara antes de BeginFrame, copia apos tonemap e finaliza antes dos contadores.
        void UpdateFrameCapture();
        // Modes deste frame: o manifesto registra o que RODOU (IsReady, gates, TAA acendendo por
        // falta de upscaler), nao o que o operador selecionou.
        void FinishFrameCapture(const FFrameModes& Modes, const FEffectiveIndirectPolicy& Policy,
                                u32 FrameSlot);
        // A politica vem pelo MESMO snapshot que rendeu o frame, e nao re-resolvida aqui: o
        // manifesto tem de descrever a politica com que a imagem foi feita. Invariante (3).
        FCaptureState    CollectCaptureState(const FFrameModes& Modes,
                                             const FEffectiveIndirectPolicy& Policy) const;
        FCaptureSettings CurrentCaptureSettings() const;
        void             ApplyCaptureSettings(const FCaptureSettings& S);
        void             RestoreCaptureState(const FCaptureSettings& S);
        f32              SettledWetness() const;
        FFrameCapture    Capture;
        // Sol da sessao, reafirmado a cada frame pelo TickWorldClock. Ver a nota la: o painel de
        // TOD escreve direto na referencia do GetTimeOfDay(), sem passar por setter, entao a unica
        // defesa que vale por construcao e reescrever.
        f32              CaptureSunHours = 0.0f;
        Vec3             CaptureSunDir{ 0.0f, 1.0f, 0.0f };
        // Hora que a sessao FIXOU de fato (negativa = nenhuma). Diferente do pin PEDIDO: com o
        // Time-of-Day desligado o pedido e ignorado, e o manifesto tem de dizer isso.
        f32              CapturePinApplied = -1.0f;
        // Enquanto verdadeiro, o funil de invalidacao NAO cancela a captura: e o proprio
        // capturador mexendo no renderer (preset, reset, restauracao). Ver UpdateFrameCapture.
        bool             CaptureSetupGuard = false;
        // O ReGIR so constroi com consumidor E luz na cena; o gate real e montado no meio do
        // frame, longe do FFrameModes. Guardado aqui para o manifesto registrar o que rodou.
        bool             ReGIRRanThisFrame = false;
        // Registra execucao real, distinta do modo apenas selecionado.
        bool             RRRanThisFrame    = false;
        // Luzes puntuais elegiveis empacotadas para o indireto neste frame.
        u32              GILightCountThisFrame = 0;
        // Ocupacao lida da copia POS-resolve do frame capturado. Separada do Stats() do painel,
        // que vem do anel e carrega query/hits — as duas metades tem origens diferentes.
        FRadianceCacheStats CaptureCacheStats{};

        bool            UseAsyncCompute = true;
        bool            AsyncGIRanLastFrame = false;
        FPipelineState  PipelineState;

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
        // Escalas independentes porque os passes operam em ordens de grandeza distintas.
        static constexpr f32 kShaderTimerScaleGI = 1.0f / 250000.0f;
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

        // Upscaler pronto para dispatch; nullptr ativa o caminho nativo/TAA.
        IUpscaler* ActiveUpscaler();
        // Aplica a razao render/display do modo selecionado.
        void ApplyUpscalerScale();
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
        // Recursos neutros quando nao existe volume DDGI.
        FGIFallbackResources GIFallback;
        u64              TlasTransformsVersion = 0; // versao da cena na ultima (re)build da TLAS
        // Versao separada: mesh lights aguardam o fim do arraste; a TLAS nao.
        u64              MeshLightTransformsVersion = 0;
        bool             TlasFlagsDirty        = false; // flags de instancia mudaram (edicao de material)
        bool             MaterialRTStateDirty  = false;
        // Persiste enquanto uma extracao de mesh lights estiver em voo.
        bool             MeshLightEmissiveDirty = false; // pedido de refresh coalescido p/ o proximo frame
        bool             IndirectLightingDirty = false; // idem, so invalidacao (ver MarkIndirectLightingDirty)
        bool             SceneContentDirty     = false; // idem (ver MarkSceneContentDirty)
        FDDGI            DDGI;
        FReGIR           ReGIR;
        // Terminador esparso dos raios secundarios; nao substitui o atlas DDGI.
        FRadianceCache   RadianceCache;
        FMeshLights      MeshLights;
        bool             UseReGIR = false; // bring-up: hits secundarios; default OFF para A/B
        FDDGIDebug       DDGIDebugPass; 
        // Controla apenas o volume DDGI; a politica do indireto vive nos enums abaixo.
        bool             UseGI       = true;
        // Trocar o produtor primario invalida o acumulador.
        EIndirectPrimary  IndirectPrimary  = EIndirectPrimary::ReSTIR_SHaRC;
        EIndirectFallback IndirectFallback = EIndirectFallback::DDGI;
        // Disponibilidade fisica do volume, independente de seu papel na politica.
        bool             DDGIVolumeLive() const;
        // Capacidades consultadas pela politica; nao representam o modo selecionado.
        bool             ReSTIRGIReady() const;
        bool             ReflectionsReady() const;
        // Politica pedida, degradada conforme os recursos disponiveis.
        EIndirectFallback EffectiveFallback() const;
        // Produtor primario que o pipeline executou de fato.
        EIndirectPrimary  EffectivePrimary() const;
        // Volumetria pode consumir o atlas mesmo quando a politica de superficie nao o usa.
        bool              DDGIVolumetricAvailable() const;
        // Superficies auxiliares ainda usam DDGI com SHaRC; apenas Primary::Off as desliga.
        bool              DDGISurfaceAvailable() const;
        // Os cinco efetivos num valor so, para o detector de borda comparar por frame.
        FEffectiveIndirectPolicy EffectiveIndirectPolicy() const;
        // Primeiro valor inicializa o detector; somente transicoes posteriores geram borda.
        FEffectiveIndirectPolicy PrevIndirectPolicy;
        bool                     HasPrevIndirectPolicy = false;
        bool             GIDebug     = false;
        bool             GIChebyshev = true;  
        bool             GISkipInactiveProbes = true;
        bool             GISkipInactiveFallback = false;
        // Gate de medicao: desliga DDGI apenas como terminador de hits RT.
        bool             GIMeasureTerminatorOff = false;

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

        // O vetor padrao e normalizado uma unica vez e todo setter preserva a invariante.
        static Vec3 DefaultSunDirection() { return Vec3{ 0.3f, 0.6f, 0.5f }.Normalized(); }
        Vec3 SunDir       = DefaultSunDirection();
        Vec3 SunColorRGB  = { 1.0f, 0.96f, 0.9f };
        f32  SunIntensity = 5.0f;

        FTimeOfDay TimeOfDay;

        bool UseAtmosphereAmbient  = true;
        f32  AtmoAmbientIntensity  = 1.0f;

        f32  ElapsedTime   = 0.0f;
        f32  LastDeltaTime = 0.0f;
        // Relogio canonico de captura: independente do tempo de parede.
        static constexpr f32 kCaptureDeltaSeconds = 1.0f / 60.0f;
        static constexpr f32 kCaptureElapsedSeconds = 0.0f;
        // FrameIndex e monotonico; TemporalSampleIndex pode reiniciar para capturas repetiveis.
        u32  FrameIndex    = 0;
        // Semente comum de jitter e RNGs temporais; avanca ao fim do frame.
        u32  TemporalSampleIndex = 0;

        bool Initialized = false;

        // Vive so durante Initialize: o editor a instala antes e limpa depois de inicializar.
        FInitProgressCallback InitProgressCallback;

        // Ponteiro permite manter FRenderSettings incompleto neste header.
        std::unique_ptr<FRenderSettings> SettingsImpl;
    };
} 
