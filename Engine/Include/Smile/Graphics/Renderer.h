#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <memory>
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
#include "Smile/Graphics/GBufferDebug.h"
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
#include "Smile/Graphics/FlickerHeatmap.h"
#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Graphics/Water.h"
#include "Smile/Graphics/Terrain.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Scene/Scene.h"

namespace Smile {
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
                                  // y = 1/res do atlas de sombra local, z = bias (NDC), w = -
        Vec4  LightParams2;       // 16 bytes — x = 1/res do cube shadow (point), y = near das
                                  // faces do cubo (formula do refZ), zw = -
    };

    // Luz puntual no formato do shader — espelha o FGPULight do DeferredLighting.ps.hlsl
    // (StructuredBuffer t17, root SRV). 4 float4 + Mat44 por luz (128 bytes); SpotParams.zw
    // reservado p/ a F4 (source length).
    struct FGPULight {
        Vec4  PosInvRadius;      // xyz = posicao, w = 1/AttenuationRadius
        Vec4  ColorSourceRadius; // rgb = Color*Intensity, w = bulb (distancia minima)
        Vec4  DirCosOuter;       // xyz = eixo do spot, w = cos(outer); -2 = point (sem cone)
        Vec4  SpotParams;        // x = 1/(cosInner - cosOuter), y = slice de sombra (-1 = sem)
        Mat44 ShadowMatrix;      // world -> UVZ do slice (perspectiva: dividir por w no shader)
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
        void RenderFrame();

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
        // interfere no IBL da cena). Render sincrono; Out = RGBA8 512x512.
        bool RenderMaterialPreview(FMaterial* Material, const FMaterialPreview::FParams& Params,
                                   std::vector<u8>& Out);
        bool LoadMaterialPreviewEnvironment(const std::wstring& Path);
        bool MaterialPreviewReady() const { return MaterialPreview.HasEnvironment(); }

        // Additive=true acrescenta a cena cozida sobre a atual (ex.: interior por cima
        // do exterior da Bistro) sem limpar meshes/materiais/camera ja carregados.
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

        void SetMergeByMaterial(bool Use) { MergeByMaterial = Use; }
        bool GetMergeByMaterial() const   { return MergeByMaterial; }

        bool IsInitialized() const { return Initialized; }

        // Supersampling (SSAA): a cena renderiza em RenderWidth/Height = swapchain * RenderScale;
        // o PostProcessor faz o downsample pro backbuffer nativo. >1.0 = mais amostras/pixel.
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
            Upscaler = U; TAARanLastFrame = false;
            ApplyUpscalerScale();
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

        // Religar invalida o historico: reservoirs/acumulacao guardam radiancia do frame em que
        // o toggle desligou (sol/emissivos/DDGI antigos sobreviveriam por tempo indeterminado).
        void SetUseReSTIRGI(bool V) {
            if (V && !UseReSTIRGI) { ReSTIRGI.InvalidateHistory(); Nrd.InvalidateHistory(); }
            UseReSTIRGI = V;
        }
        bool GetUseReSTIRGI() const    { return UseReSTIRGI; }

        void SetUseNrdDenoise(bool V) {
            if (V && !UseNrdDenoise) Nrd.InvalidateHistory();
            UseNrdDenoise = V;
        }
        bool GetUseNrdDenoise() const  { return UseNrdDenoise; }

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
        FGBufferDebug  GBufferDebugPass;
        u32            GBufferDebugMode = 0;

        // Motion vector buffer (RG16F): escrito no geometry pass (SV_Target3), lido pelo TAA.
        // RT proprio (lifecycle desacoplado das transicoes do GBuffer, que fazem ping-pong p/ as
        // reflexoes). Velocidade em UV = curUV(sem jitter) - prevUV.
        ComPtr<ID3D12Resource>   VelocityBuffer;
        FDescriptorHeap          VelocityRTVHeap;   // 1 RTV
        u32                      VelocitySRVSlot = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES    VelocityState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
        bool MergeByMaterial   = false;
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
        IUpscaler* ActiveUpscaler() {
            switch (Upscaler) {
                case EUpscaler::FSR:  return Fsr.IsInitialized()  ? static_cast<IUpscaler*>(&Fsr)  : nullptr;
                case EUpscaler::DLSS: return Dlss.IsInitialized() ? static_cast<IUpscaler*>(&Dlss) : nullptr;
                default:              return nullptr;
            }
        }
        // Razao render/display pura do upscaler SELECIONADO (independe de estar inicializado).
        void ApplyUpscalerScale() {
            f32 R = 1.0f;
            if      (Upscaler == EUpscaler::FSR)  R = Fsr.RenderRatioForQuality(UpscalerQuality);
            else if (Upscaler == EUpscaler::DLSS) R = Dlss.RenderRatioForQuality(UpscalerQuality);
            SetRenderScale(R);
        }

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
        FDDGI            DDGI;
        FDDGIDebug       DDGIDebugPass; 
        bool             UseGI       = true;
        bool             GIDebug     = false; 
        bool             GIChebyshev = true;  
        bool             GISkipInactiveProbes = true; 
        bool             GISkipInactiveFallback = false; 

        FReSTIRGI        ReSTIRGI;
        bool             UseReSTIRGI = false; // experimental; default OFF (nao toca o estado padrao)
        FNrdDenoiser     Nrd;                 // denoiser do ReSTIR GI (RELAX_DIFFUSE) — Fase B/C
        bool             UseNrdDenoise = false; // NRD como denoiser do ReSTIR (Fase C)
        Mat44            NrdPrevView{};        // prev view/proj NAO-jitteradas p/ a reprojecao do NRD
        Mat44            NrdPrevProj{};
        Vec2             PrevJitterUv{ 0.0f, 0.0f }; // jitter do frame anterior em UV (y ja invertido
                                                     // p/ uv y-down) — reprojecao do ReSTIR GI
        Vec2             PrevJitterPx{ 0.0f, 0.0f }; // idem em pixels — cameraJitterPrev do NRD

        FReflections     Reflections;
        bool             UseReflections = true;

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
