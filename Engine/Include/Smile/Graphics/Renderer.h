#pragma once

#include <Windows.h>
#include <string>
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
#include "Smile/Graphics/Camera.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/GBuffer.h"
#include "Smile/Graphics/DebugView.h"
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
    public:
        Renderer();
        ~Renderer() noexcept;

        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;

        void Initialize(HWND hWnd, u32 Width, u32 Height);
        void Shutdown();

        void Resize(u32 Width, u32 Height);
        // Recarrega os PSOs cujo shader mudou. ChangedStem = nome do .cso sem perfil
        // nem extensao (ex.: "WaterSurface.ps"). Vazio (ou stem nao mapeado / .hlsli
        // incluido por varios shaders) => reload completo.
        bool ReloadShaders(const std::string& ChangedStem = "");

        void SetVSync(bool Enabled) { SwapChain.SetVSync(Enabled); }
        bool GetVSync() const       { return SwapChain.GetVSync(); }

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


        void SetFrustumCulling(bool Use) { UseFrustumCulling = Use; }
        bool GetFrustumCulling() const   { return UseFrustumCulling; }

        // Occlusion culling (HZB): ao religar, descarta resultados velhos do readback
        // ring — os proximos kFramesInFlight frames desenham tudo ate ter teste fresco.
        void SetOcclusionCulling(bool Use) {
            if (Use && !UseOcclusionCulling) HiZ.InvalidateResults();
            UseOcclusionCulling = Use;
        }
        bool GetOcclusionCulling() const { return UseOcclusionCulling; }
        u32  GetOccludedCount() const    { return LastOccludedCount; }

        void SetDepthPrepass(bool Use)   { UseDepthPrepass = Use; }
        bool GetDepthPrepass() const     { return UseDepthPrepass; }
        u32  GetVisibleCount() const     { return LastVisibleCount; } 
        u32  GetDrawCount() const        { return static_cast<u32>(Scene.Renderables().size()); }


        bool IsInitialized() const { return Initialized; }

        // Supersampling (SSAA): a cena renderiza em RenderWidth/Height = swapchain * RenderScale;
        // o PostProcessor faz o downsample pro backbuffer nativo. >1.0 = mais amostras/pixel.
        // IGNORADO com o denoiser em DLSS_RR: ali a resolucao de ENTRADA e ditada pelo modo de
        // qualidade (o RR nao suporta DRS e a feature NGX e criada na res otima do modo), entao uma
        // escala arbitraria faria o render subrect divergir do buffer criado. A UI ja esconde o
        // slider nesse estado (o RR forca upscaler=DLSS); isto blinda a invariante no motor.
        void SetRenderScale(f32 V); // recria os RTs internos (so a cena; backbuffer fica nativo)
        f32  GetRenderScale() const { return RenderScale; }
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
        void SetSelectedObject(int Index) { SelectedIndex = Index; }
        int  GetSelectedObject() const    { return SelectedIndex; }
        void ClearSelection()             { SelectedIndex = -1; }

        // Selecao de LUZ (independente da selecao de renderavel; o editor mantem as duas
        // mutuamente exclusivas). Indice em Scene.Lights(); -1 = nenhuma.
        void SetSelectedLight(int Index)  { SelectedLightIdx = Index; }
        int  GetSelectedLight() const     { return SelectedLightIdx; }
        void ClearLightSelection()        { SelectedLightIdx = -1; }
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

        void SetSunDirection(const Vec3& Dir);
        void SetSunColor(const Vec3& Color)  { SunColorRGB = Color; }
        Vec3 GetSunColor()     const         { return SunColorRGB; }
        Vec3 GetSunDirection() const         { return SunDir; }

        // Estado do Time-of-Day, exposto p/ o painel TOD do editor (leitura e escrita).
        FTimeOfDay&       GetTimeOfDay()       { return TimeOfDay; }
        const FTimeOfDay& GetTimeOfDay() const { return TimeOfDay; }

        // Estado de clima (chuva), exposto p/ a secao Clima do painel TOD (leitura e escrita).
        FWeather&       GetWeather()       { return Weather; }
        const FWeather& GetWeather() const { return Weather; }

        void LoadMoonTexture(const std::wstring& Path);
        void LoadStarCatalog(const std::wstring& Path);

        // === Seletor de upscaler (None / FSR / DLSS) — substitui o TAA quando != None ===
        // Cai automaticamente p/ None se o upscaler pedido nao estiver disponivel (sem reconstrutor nao
        // da p/ renderizar sub-nativo). A qualidade (0=100%..4=UltraPerf) e compartilhada entre eles.
        void SetUpscaler(EUpscaler U) {
            if (U != EUpscaler::None && !UpscalerAvailable(U)) U = EUpscaler::None;
            // Acoplamento: o RR faz o upscale via DLSS. Trocar o upscaler p/ fora de DLSS desliga o
            // RR (cai p/ NRD). Vai pelo SetDenoiser em vez de atribuir Denoiser direto: a
            // atribuicao pulava Nrd.InvalidateHistory(), ReSTIRGI.InvalidateHistory() e o
            // RRResetPending, entao o NRD reaproveitava acumulacao de outro denoiser e os
            // reservoirs entravam no NRD ainda com o teto de firefly do modo cru (4 em vez de 8).
            // Nao recursa: o SetDenoiser so chama SetUpscaler quando o denoiser alvo e DLSS_RR, e
            // aqui o alvo e sempre NRD.
            const bool LeavingRR = (Denoiser == EDenoiser::DLSS_RR && U != EUpscaler::DLSS);
            Upscaler = U; TAARanLastFrame = false;
            if (LeavingRR) SetDenoiser(EDenoiser::NRD); // ja faz o ApplyUpscalerScale()
            else           ApplyUpscalerScale();
        }
        EUpscaler GetUpscaler() const        { return Upscaler; }
        bool UpscalerAvailable(EUpscaler U) const {
            switch (U) {
                case EUpscaler::FSR:  return Fsr.Available();
                case EUpscaler::DLSS: return Dlss.Available();
                default:              return true; // None sempre disponivel
            }
        }
        void SetUpscalerQuality(int Q) {
            UpscalerQuality = Q < 0 ? 0 : (Q > 4 ? 4 : Q);
            ApplyUpscalerScale();
        }
        int  GetUpscalerQuality() const      { return UpscalerQuality; }
        void SetUseTAA(bool V)               { UseTAA = V; TAARanLastFrame = false; }
        bool GetUseTAA() const               { return UseTAA; }
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

        u32  GetDepthSRVSlot() const         { return DepthSRVSlot; }

        FFogPass& GetFog()                     { return Fog; }

        // Froxel volumetric fog (F1): exige height fog ON (mesma densidade).
        void SetUseVolumetricFog(bool Use)     { UseVolumetricFog = Use; }
        bool GetUseVolumetricFog() const       { return UseVolumetricFog; }
        FVolumetricFogPass& GetVolumetricFog()             { return VolumetricFog; }
        const FVolumetricFogPass& GetVolumetricFog() const { return VolumetricFog; }

        void SetUseSunShafts(bool Use)         { UseSunShafts = Use; }
        bool GetUseSunShafts() const           { return UseSunShafts; }
        FSunShafts& GetSunShafts()             { return SunShafts; }
        const FSunShafts& GetSunShafts() const { return SunShafts; }

        FSunShadows& GetSunShadows()           { return SunShadows; }
        void SetUseSunShadows(bool Use)        { UseSunShadows = Use; }
        bool GetUseSunShadows() const          { return UseSunShadows; }

        void SetUseWater(bool Use);
        bool GetUseWater() const             { return UseWater; }
        FWaterRenderer& GetWater()           { return Water; }

        // Terreno (F1: renderizacao apenas). Carregado pelo sidecar <cena>.terrain.json no
        // LoadCookedScene, ou direto via LoadTerrain.
        FTerrain&       GetTerrain()         { return Terrain; }
        const FTerrain& GetTerrain() const   { return Terrain; }
        bool LoadTerrain(const FTerrainDesc& Desc) {
            return Terrain.Load(Device.Native(), UploadQueue, SRVHeap, Desc);
        }
        void SetUseTerrain(bool Use)         { UseTerrain = Use; }
        bool GetUseTerrain() const           { return UseTerrain; }

        void SetUseClouds(bool Use)          { if (Use && !UseClouds) VolumetricClouds.InvalidateHistory();
                                               UseClouds = Use; }
        bool GetUseClouds() const            { return UseClouds; }
        FVolumetricClouds& GetVolumetricClouds() { return VolumetricClouds; }
        const FVolumetricClouds& GetVolumetricClouds() const { return VolumetricClouds; }
        void SetCloudsHalfRes(bool HalfRes); // recria o RT das nuvens (flush da fila)

        // Weather map das nuvens: parametros do bake procedural + textura autorada.
        // Setters re-bakeam na hora (flush + dispatch sincrono, ~ms).
        void SetCloudWeatherSeed(u32 Seed);
        u32  GetCloudWeatherSeed() const  { return CloudNoise.GetSeed(); }
        void SetCloudWeatherCells(u32 Mult);
        u32  GetCloudWeatherCells() const { return CloudNoise.GetCellMult(); }
        bool LoadCloudWeatherTexture(const std::wstring& Path);
        void ClearCloudWeatherTexture();
        bool CloudWeatherAuthored() const { return CloudNoise.HasWeatherOverride(); }

        void SetSunAzimuthElevation(f32 AzimuthDeg, f32 ElevationDeg);

        Vec3 GetCameraPos() const { return Camera.GetPosition(); }
        f32  GetPitch()     const { return Camera.GetPitch(); }
        f32  GetYaw()       const { return Camera.GetYaw(); }
        // Foco de camera do editor (duplo-clique no Scene Outliner): teleporta mantendo
        // a orientacao atual.
        void SetCameraPose(const Vec3& Pos, f32 PitchDeg, f32 YawDeg) {
            Camera.SetPose(Pos, PitchDeg, YawDeg);
        }

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

        FDDGI& GetDDGI()               { return DDGI; }
        void SetUseGI(bool V)           { UseGI = V; }
        bool GetUseGI() const           { return UseGI; }

        // ReGIR e um sampler de luzes do MUNDO para hits secundarios. Nao substitui o ReSTIR DI
        // de tela; troca apenas o loop de luzes dentro de ShadeSurfaceHit.
        void SetUseReGIR(bool V) {
            if (V == UseReGIR) return;
            UseReGIR = V;
            ReGIR.InvalidateHistory();
            DDGI.ResetHistoryOnce();
            ReSTIRGI.InvalidateHistory();
            Reflections.InvalidateHistory();
            Nrd.InvalidateHistory();
            RRResetPending = true;
            TAARanLastFrame = false;
        }
        bool GetUseReGIR() const { return UseReGIR; }
        bool ReGIRActive() const { return UseReGIR && ReGIR.IsReady(); }

        // F3: DDGI na fila de compute, sobrepondo CSM/prepass/G-buffer (default ON).
        void SetUseAsyncCompute(bool V) { UseAsyncCompute = V; }
        bool GetUseAsyncCompute() const { return UseAsyncCompute; }

        FReSTIRGI&       GetReSTIRGI()       { return ReSTIRGI; }
        const FReSTIRGI& GetReSTIRGI() const { return ReSTIRGI; }
        FReflections& GetReflections()     { return Reflections; }
        // Borda de subida invalida o historico do NRD: com reflexoes off o spec acumula sinal
        // zero — religar sem reset arrastaria esse historico vazio pro especular real.
        void SetUseReflections(bool V) {
            if (V && !UseReflections) Nrd.InvalidateHistory();
            UseReflections = V;
        }
        bool GetUseReflections() const     { return UseReflections; }

        bool GetUseReSTIRDI() const { return UseReSTIRDI; }
        bool ReSTIRDIActive() const { return UseReSTIRDI && ReSTIRDI.IsReady(); }
        void SetUseReSTIRDI(bool V) {
            if (V == UseReSTIRDI) return;
            if (V) ReSTIRDI.InvalidateHistory();
            UseReSTIRDI = V;
            NrdDirect.InvalidateHistory();
            Nrd.InvalidateHistory();
            RRResetPending = true;
            TAARanLastFrame = false;
        }

        // Religar invalida o historico: reservoirs/acumulacao guardam radiancia do frame em que
        // o toggle desligou (sol/emissivos/DDGI antigos sobreviveriam por tempo indeterminado).
        void SetUseReSTIRGI(bool V) {
            if (V && !UseReSTIRGI) { ReSTIRGI.InvalidateHistory(); Nrd.InvalidateHistory(); }
            UseReSTIRGI = V;
        }
        bool GetUseReSTIRGI() const    { return UseReSTIRGI; }

        // Eixo de denoiser {None, NRD, DLSS_RR}. NRD/None e o toggle antigo; DLSS_RR (Ray Reconstruction)
        // substitui o NRD E o passe de SR num eval so, entao SELECIONAR RR FORCA o upscaler p/ DLSS.
        // === Perfil de epsilons de raio (knobs de calibracao da pagina "Iluminacao global") ===
        // Mudar geometria de raio invalida TUDO que acumula: os reservoirs do ReSTIR guardam Lo e
        // x2 medidos com os epsilons antigos, e o NRD acumula sobre eles. Sem o clear o A/B compara
        // um estado misturado e nao mede nada.
        const FRayEpsilonProfile& GetRayEpsilons() const { return RayEps; }
        void SetRayEpsilons(const FRayEpsilonProfile& P) {
            RayEps = P;
            // TUDO que acumula precisa cair junto, senao o knob parece inerte enquanto o valor
            // antigo vaza pelo historico:
            ReSTIRGI.InvalidateHistory();   // reservoirs guardam Lo/x2 medidos com o epsilon velho
            ReSTIRDI.InvalidateHistory();   // shadow ray final usa o mesmo perfil
            NrdDirect.InvalidateHistory();  // acumula a direta resolvida por esse shadow ray
            Nrd.InvalidateHistory();        // acumula sobre esses reservoirs
            RRResetPending    = true;       // historico neural do Ray Reconstruction
            DDGI.ResetHistoryOnce();        // Hysteresis 0.99 -> 99% do atlas velho sobrevive por update
            Reflections.InvalidateHistory();// historico temporal proprio (caminho legado, sem NRD)
            TAARanLastFrame   = false;      // sem upscaler, o TAA acumula por conta propria
        }

        // Invalidacao dos knobs de amostragem do DDGI. TODOS eles entram hoje no ShadeSurfaceHit
        // — o bias desde que o 2o bounce passou a usar o gather completo, o fade de borda desde
        // que ele tambem parou de extrapolar la — e por isso mudam o que fica GRAVADO, nao so o
        // que aparece na tela: o valor sombreado no hit volta para o atlas do DDGI (hysteresis
        // 0,99), para os reservoirs do ReSTIR, para o historico das reflexoes, para o inscatter
        // acumulado do fog e, por cima de tudo isso, para NRD/RR/TAA.
        //
        // Houve aqui uma variante LEVE, so com TAA, para quando o knob fosse exclusivo do
        // consumidor final. Ela ficou sem usuarios e saiu — e a licao vale a nota: os dois knobs
        // NASCERAM sendo de sampler puro e deixaram de ser DEPOIS, em commits que nao voltaram
        // no setter. Ao levar um parametro para dentro do hit, reveja a invalidacao dele.
        //
        // O preco e o mesmo do SetRayEpsilons: o reset faz a tomada passar de novo pela
        // realizacao aleatoria de um frame (as direcoes giram com o frameIndex), entao para
        // medir A/B e preciso esperar convergir. Sem o reset seria pior: estados misturados.
        void OnGIHitSamplingChanged() {
            DDGI.ResetHistoryOnce();
            ReSTIRGI.InvalidateHistory();
            Reflections.InvalidateHistory();
            Nrd.InvalidateHistory();
            VolumetricFog.ResetHistory(); // acumula o proprio inscatter, que le o DDGI
            RRResetPending = true;
            TAARanLastFrame = false;
            // O diagnostico pontual e one-shot por clique: sem reexecutar, o painel exibiria os
            // numeros do knob ANTERIOR e a ferramenta mentiria justamente durante o A/B.
            RepeatDebugProbePoint();
        }

        // Teto do self-shadow bias, em metros (0 = sem teto = comportamento historico). Segue
        // como knob porque o 0,40 m saiu de raciocinio, nao de varredura — a escala relevante e a
        // espessura de parede da cena, nao o espacamento do grid. Vira constante depois do sweep.
        // Invalidacao PESADA: o bias entra no 2o bounce (ver OnGIHitSamplingChanged).
        f32  GetGISurfaceBiasMax() const { return DDGI.GetSurfaceBiasMax(); }
        void SetGISurfaceBiasMax(f32 V) {
            if (V == DDGI.GetSurfaceBiasMax()) return;
            DDGI.SetSurfaceBiasMax(V);
            OnGIHitSamplingChanged();
        }
        f32  GetGISurfaceBiasScale() const { return DDGI.GetSurfaceBiasScale(); }
        void SetGISurfaceBiasScale(f32 V) {
            if (V == DDGI.GetSurfaceBiasScale()) return;
            DDGI.SetSurfaceBiasScale(V);
            OnGIHitSamplingChanged();
        }
        // Fade para o ambiente hemisferico nas bordas do volume (em celulas; 0 = desligado).
        // Invalidacao PESADA: o fade tambem entra no 2o bounce (ver OnGIHitSamplingChanged) —
        // la ele multiplica o indireto do hit, que e parte do valor devolvido as sondas.
        f32  GetGIVolumeFadeProbes() const { return DDGI.GetVolumeFadeProbes(); }
        void SetGIVolumeFadeProbes(f32 V) {
            if (V == DDGI.GetVolumeFadeProbes()) return;
            DDGI.SetVolumeFadeProbes(V);
            OnGIHitSamplingChanged();
        }

        // Culling nos raios de REFLEXAO. Substituiu a antiga chave global da TLAS: aquela mexia em
        // todos os passes de uma vez, e a TLAS agora descreve so a geometria (two-sided de
        // verdade), com cada passe escolhendo a ray flag. Nao precisa de rebuild da TLAS — e
        // parametro de shader. Invalida so o que acumula reflexo: o ReSTIR e o DDGI nao veem
        // esta chave.
        bool GetReflectionsCullBackface() const { return Reflections.GetBackfaceCull(); }
        void SetReflectionsCullBackface(bool V) {
            if (V == Reflections.GetBackfaceCull()) return;
            Reflections.SetBackfaceCull(V);
            Reflections.InvalidateHistory(); // acumulacao temporal propria (caminho sem NRD)
            Nrd.InvalidateHistory();         // o specular do NRD acumula sobre o mesmo sinal
            RRResetPending  = true;
            TAARanLastFrame = false;
        }

        // Agua "invisivel aos guides" — A/B do SEGUNDO eixo do problema da agua sob Ray
        // Reconstruction. A agua e o unico passe que escreve depth e velocity SEM escrever
        // G-buffer: no pixel de agua o RR recebe profundidade e movimento da superficie com
        // albedo/normal/roughness do fundo, um jogo de guides contraditorio entre si — pior que um
        // uniformemente errado. Ligando, a agua vira overlay so de cor e os guides voltam a
        // descrever coerentemente o que esta atras; a cor sai identica, entao o A/B mexe so nisso.
        // Complementa o eixo do WARP (a refracao amostrando o SceneColor ruidoso em UV deslocada):
        // sao problemas independentes, e um resultado negativo num nao absolve o outro.
        // NAO e knob de look — sem escrita de depth a agua deixa de ocluir quem le o depth buffer.
        bool GetWaterGuideInvisible() const { return Water.GetGuideInvisible(); }
        void SetWaterGuideInvisible(bool V) {
            if (V == Water.GetGuideInvisible()) return;
            Water.SetGuideInvisible(V);
            RRResetPending  = true;  // muda os guides do RR: historico neural velho mente
            TAARanLastFrame = false;
        }

        // Politica de backface do gather do ReSTIR. Passa pelo Renderer, e nao direto no
        // FReSTIRGI, porque o clear dos reservoirs sozinho nao basta: o NRD e o RR acumulam SOBRE
        // eles e o TAA sobre o resultado, entao um A/B feito so com o clear compararia um estado
        // misturado. DDGI e reflexoes ficam de fora de proposito — a politica so toca no gather.
        bool GetGIBackfacePolicy() const { return ReSTIRGI.GetBackfacePolicy(); }
        void SetGIBackfacePolicy(bool V) {
            if (V == ReSTIRGI.GetBackfacePolicy()) return;
            ReSTIRGI.SetBackfacePolicy(V); // ja marca NeedsClear nos reservoirs
            Nrd.InvalidateHistory();
            RRResetPending  = true;
            TAARanLastFrame = false;
        }

        // Editar no editor uma propriedade de material que o RAY TRACING enxerga (AlphaTest,
        // TwoSided, emissivo...) deixava tres estados obsoletos de uma vez, porque o setter do
        // material so reescrevia o constant buffer dele:
        //   - a TLAS, que carrega InstanceMask, FORCE_NON_OPAQUE e o culling por instancia;
        //   - o InstanceGeo, snapshot criado UMA vez no SetupForScene e lido por todo o RT;
        //   - os historicos acumulados sobre a aparencia antiga.
        // O Flush e necessario: o InstanceGeo e um upload heap sem versao por frame em voo, entao
        // reescrever com frames voando corromperia o que eles leem. Custa um stall, mas isto so
        // dispara em edicao manual de material.
        // Versao COALESCIDA: use esta nos setters do editor. Arrastar um slider dispara o setter a
        // cada tick, e cada NotifyMaterialRTStateChanged custa um Flush da fila + reset de todos os
        // historicos — fazer isso por tick derrubaria o frame rate e manteria o GI em reset
        // permanente. O RenderFrame consome a flag uma vez, antes do BeginFrame.
        void MarkMaterialRTStateDirty() { MaterialRTStateDirty = true; }

        // Mesma ideia, para edicao de LUZ que muda a energia do indireto (hoje: FLight::RTWeight).
        // Precisa de invalidacao mas NAO de refresh: a lista do GI (FGPULightGI) e reempacotada da
        // FScene a cada frame, entao nao ha Flush da fila nem RefreshInstanceGeo a fazer — so
        // derrubar quem ACUMULOU energia da luz antiga. Sem isto, o slider parece inerte por muitos
        // frames: o DDGI tem Hysteresis 0,99, ou seja, 99% do atlas velho sobrevive por update, e a
        // calibracao seria feita contra energia que o usuario ja mandou remover.
        void MarkIndirectLightingDirty() { IndirectLightingDirty = true; }

        void NotifyIndirectLightingChanged() {
            DDGI.ResetHistoryOnce();
            TemporalMotion.InvalidateHistory();
            ReSTIRGI.InvalidateHistory();   // reservoirs guardam Lo medido com a luz antiga
            Reflections.InvalidateHistory();
            Nrd.InvalidateHistory();
            VolumetricFog.ResetHistory();   // acumula o proprio inscatter, que le o DDGI
            RRResetPending  = true;
            TAARanLastFrame = false;
        }

        void NotifyMaterialRTStateChanged() {
            CommandQueue.Flush();
            DDGI.RefreshInstanceGeo(Scene);
            TlasFlagsDirty = true; // mask/FORCE_NON_OPAQUE/culling saem do material
            TemporalMotion.InvalidateHistory();
            ReSTIRGI.InvalidateHistory();
            ReSTIRDI.InvalidateHistory();
            NrdDirect.InvalidateHistory();
            Nrd.InvalidateHistory();
            RRResetPending = true;
            DDGI.ResetHistoryOnce();
            Reflections.InvalidateHistory();
            TAARanLastFrame = false;
        }

        void SetDenoiser(EDenoiser D) {
            if (D == EDenoiser::DLSS_RR && !DlssRR.Available()) D = EDenoiser::NRD; // fallback: sem NVIDIA/RR
            if (D == Denoiser) return;
            Nrd.InvalidateHistory();            // muda a natureza do sinal -> reinicia acumulacao
            NrdDirect.InvalidateHistory();      // instancia independente da iluminacao direta
            // O teto de firefly do ReSTIR depende do denoiser (FireflyMax 8 com NRD, FireflyMaxRaw
            // 4 sem ele) e e aplicado ao Lo NA HORA DO TRACE, ou seja, fica gravado no reservoir.
            // Sem invalidar, o Lo clampado no teto antigo sobrevive no historico — e com
            // ValidateInterval = 0 nao ha re-shade que o corrija.
            ReSTIRGI.InvalidateHistory();
            // O DI usa permutation temporal somente no RR. Ao trocar de denoiser, nao misture
            // reservoirs produzidos por politicas de reprojecao diferentes.
            ReSTIRDI.InvalidateHistory();
            // NRD/RR param no Resolved cru e deixam History[] das reflexoes sem escrita. Ao voltar
            // ao caminho legado, esse historico pode ter frames antigos e paridade ja avancada.
            Reflections.InvalidateHistory();
            Denoiser = D;
            RRResetPending = true;              // trocar de/para RR: descarta o historico neural velho
            if (Denoiser == EDenoiser::DLSS_RR) // RR faz o upscale; trava o upscaler em DLSS
                SetUpscaler(EUpscaler::DLSS);
            TAARanLastFrame = false;
            ApplyUpscalerScale();
        }
        EDenoiser GetDenoiser() const  { return Denoiser; }
        bool RRAvailable() const       { return DlssRR.Available(); }

        // Compat: o toggle NRD antigo (viewport) mapeia p/ o eixo de denoiser {None, NRD}.
        void SetUseNrdDenoise(bool V) { SetDenoiser(V ? EDenoiser::NRD : EDenoiser::None); }
        bool GetUseNrdDenoise() const  { return Denoiser == EDenoiser::NRD; }

        FAmbientOcclusion& GetAO()     { return AO; }
        void SetUseAO(bool V)          { UseAO = V; }
        bool GetUseAO() const          { return UseAO; }

    private:
        void RecreateAllPSOs();
        void BuildDefaultScene();
        void BuildRaytracingScene();
        void SetupGIForScene(const Vec3& AABBMin, const Vec3& AABBMax);
        void CreateDepthBuffer();
        void CreateConstantBuffer();
        void CreateHDRBuffers();
        void CreateSceneCopies();
        void CreateVelocityBuffer();
        void RecreateInternalTargets(); // recria RTs de cena em RenderWidth/Height (resize + render scale)
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

        ComPtr<ID3D12Resource>   DepthBuffer;
        FDescriptorHeap          DSVHeap;

        static constexpr u32     kInvalidSlot = 0xFFFFFFFFu;
        u32                      DepthSRVSlot = kInvalidSlot;

        ComPtr<ID3D12Resource>   NormalBuffer;
        FDescriptorHeap          NormalRTVHeap;
        u32                      NormalSRVSlot = kInvalidSlot;
        D3D12_RESOURCE_STATES    NormalBufferState = D3D12_RESOURCE_STATE_COMMON;
        void CreateNormalBuffer();

        void SetupReflectionsForScene();

        // Deferred shading: o G-buffer e a unica fonte de geometria opaca. GBufferB (OctNormal +
        // Roughness + Metallic) e byte-a-byte o antigo ReflectionGBuffer -> as reflexoes leem dele.
        FGBuffer       GBuffer;
        FDebugView     DebugViewPass;
        FDebugView     DebugPreviewPass;
        // Registra em DebugTargets os alvos que ja tem SRV. Chamado no fim de
        // RecreateInternalTargets(), pois o resize realoca slots (o registro sobrescreve por nome).
        void RegisterDebugTargets();
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
        ComPtr<ID3D12Resource>   VelocityBuffer;
        FDescriptorHeap          VelocityRTVHeap;   // 1 RTV
        u32                      VelocitySRVSlot = 0xFFFFFFFFu;
        u32                      VelocityUavSlot = 0xFFFFFFFFu; // UAV p/ o passe de velocity do background
        D3D12_RESOURCE_STATES    VelocityState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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

        ComPtr<ID3D12Resource>   HDRColorBuffer;
        FDescriptorHeap          HDRRTVHeap;
        u32                      HDRSRVSlot = kInvalidSlot;
        FPostProcessor           PostProcessor;

        FObjectPicker            ObjectPicker;
        FSelectionOutline        SelectionOutline;
        int                      SelectedIndex = -1;
        int                      SelectedLightIdx = -1;

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

        FFogPass        Fog;
        FVolumetricFogPass VolumetricFog;
        bool            UseVolumetricFog     = true; // froxel fog; exige height fog ON
        bool            UseAerialPerspective = false;
        bool            UseHeightFog         = true;

        FSunShafts      SunShafts;
        bool            UseSunShafts = true; // raymarch meia-res + temporal; exige height fog ON

        FWeather        Weather;     // estado de clima (chuva) — editor escreve, chuva le
        FRainWetness    RainWetness; // F1: wetness deferred no G-buffer (pos-geometry pass)

        FSunShadows     SunShadows;
        bool            UseSunShadows = true;

        FLocalShadows   LocalShadows; // sombras de spot (F3a); budget kMaxShadows/frame
 
        FRaytracingScene RaytracingScene;
        u64              TlasTransformsVersion = 0; // versao da cena na ultima (re)build da TLAS
        bool             TlasFlagsDirty        = false; // flags de instancia mudaram (edicao de material)
        bool             MaterialRTStateDirty  = false; // pedido de refresh coalescido p/ o proximo frame
        bool             IndirectLightingDirty = false; // idem, so invalidacao (ver MarkIndirectLightingDirty)
        FDDGI            DDGI;
        FReGIR           ReGIR;
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

        ComPtr<ID3D12Resource> SceneColorCopy;
        ComPtr<ID3D12Resource> SceneDepthCopy;
        u32                    SceneCopyTableStart = kInvalidSlot;
        D3D12_RESOURCE_STATES  SceneColorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_RESOURCE_STATES  SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

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
    };
} 
