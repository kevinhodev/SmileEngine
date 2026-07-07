#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <memory>
#include "Smile/Math/Math.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Graphics/CommandQueue.h"
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
#include "Smile/Graphics/SunShadows.h"
#include "Smile/Graphics/RaytracingScene.h"
#include "Smile/Graphics/DDGI.h"
#include "Smile/Graphics/DDGIDebug.h"
#include "Smile/Graphics/ReSTIRGI.h"
#include "Smile/Graphics/NrdDenoiser.h"
#include "Smile/Graphics/Reflections.h"
#include "Smile/Graphics/AmbientOcclusion.h"
#include "Smile/Graphics/PostProcess.h"
#include "Smile/Graphics/Picking.h"
#include "Smile/Graphics/SelectionOutline.h"
#include "Smile/Graphics/DebugDraw.h"
#include "Smile/Graphics/TemporalAA.h"
#include "Smile/Graphics/Fsr2Pass.h"
#include "Smile/Graphics/FlickerHeatmap.h"
#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Graphics/Water.h"
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

        Vec4  RenderParams;       // 16 bytes (c18) — x = mip bias global de textura (FSR2 upscale:
                                  // log2(render/display) - 1; 0 quando nativo/SSAA), yzw = -
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
        ~Renderer();

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

        // Additive=true acrescenta a cena cozida sobre a atual (ex.: interior por cima
        // do exterior da Bistro) sem limpar meshes/materiais/camera ja carregados.
        bool LoadCookedScene(const std::wstring& ScenePath, bool Additive = false);

        FTexture& GetDefaultWhite()  { return TexDefaultWhite; }
        FTexture& GetDefaultNormal() { return TexDefaultNormal; }
        FTexture& GetDefaultBlack()  { return TexDefaultBlack; }


        void SetFrustumCulling(bool Use) { UseFrustumCulling = Use; }
        bool GetFrustumCulling() const   { return UseFrustumCulling; }

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

        void LoadMoonTexture(const std::wstring& Path);

        // FSR2 (substitui o TAA quando ligado). So funciona em build Release (Debug = stub).
        void SetUseFsr2(bool V) {
            UseFsr2 = V; TAARanLastFrame = false;
            // FSR2 = render menor + upscale; desligado volta ao nativo. So mexe no scale se o
            // contexto existe (Release) — em Debug (stub) nao mexe p/ nao borrar via post chain.
            if (Fsr2.IsInitialized()) SetRenderScale(V ? Fsr2Ratio() : 1.0f);
        }
        bool GetUseFsr2() const              { return UseFsr2; }
        bool Fsr2Available() const           { return Fsr2.IsInitialized(); }
        // Qualidade do FSR2: 0=Native(1.0) 1=Quality(1.5x) 2=Balanced(1.7x) 3=Performance(2.0x)
        // 4=UltraPerf(3.0x). Dirige o RenderScale (render res < display = upscale + perf).
        void SetFsr2Quality(int Mode) {
            Fsr2Quality = Mode < 0 ? 0 : (Mode > 4 ? 4 : Mode);
            if (UseFsr2 && Fsr2.IsInitialized()) SetRenderScale(Fsr2Ratio());
        }
        int  GetFsr2Quality() const          { return Fsr2Quality; }
        void SetUseTAA(bool V)               { UseTAA = V; TAARanLastFrame = false; }
        bool GetUseTAA() const               { return UseTAA; }
        void SetFlickerMode(u32 Mode)        { if (Mode > 0 && FlickerMode == 0) FlickerResetPending = true; FlickerMode = Mode; }
        u32  GetFlickerMode() const          { return FlickerMode; }

        // View modes/debug views exposed to the editor viewport toolbar.
        void SetGBufferDebugMode(u32 Mode)   { GBufferDebugMode = Mode > 8 ? 8 : Mode; }
        u32  GetGBufferDebugMode() const     { return GBufferDebugMode; }

        u32  GetDepthSRVSlot() const         { return DepthSRVSlot; }

        FFogPass& GetFog()                     { return Fog; }

        FSunShadows& GetSunShadows()           { return SunShadows; }

        void SetUseWater(bool Use);
        bool GetUseWater() const             { return UseWater; }
        FWaterRenderer& GetWater()           { return Water; }

        void SetSunAzimuthElevation(f32 AzimuthDeg, f32 ElevationDeg);

        Vec3 GetCameraPos() const { return Camera.GetPosition(); }
        f32  GetPitch()     const { return Camera.GetPitch(); }
        f32  GetYaw()       const { return Camera.GetYaw(); }

        const FD3D12Device& GetDevice()  const { return Device; }
        FCommandQueue&      GetCmdQueue()      { return CommandQueue; }
        FTextureSRVHeap&    GetSRVHeap()       { return SRVHeap; }

        FRaytracingScene&   GetRaytracingScene() { return RaytracingScene; }

        FDDGI& GetDDGI()               { return DDGI; }
        void SetUseGI(bool V)           { UseGI = V; }
        bool GetUseGI() const           { return UseGI; }

        FReflections& GetReflections()     { return Reflections; }
        void SetUseReflections(bool V)     { UseReflections = V; }
        bool GetUseReflections() const     { return UseReflections; }

        void SetUseReSTIRGI(bool V)    { UseReSTIRGI = V; }
        bool GetUseReSTIRGI() const    { return UseReSTIRGI; }

        void SetUseNrdDenoise(bool V)  { UseNrdDenoise = V; }
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
        FSwapChain      SwapChain;
        FPipelineState  PipelineState;
        FTextureSRVHeap SRVHeap;

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

        // FSR2 (AMD FidelityFX) — substitui o TAA custom. Fase 1: so ciclo de vida do contexto.
        // Ativo so em build Release (em Debug FFsr2Pass e stub). Dispatch vem na Fase 2.
        FFsr2Pass                Fsr2;
        bool                     UseFsr2 = true;  // FSR2 ligado por padrao (substitui o TAA em Release)
        int                      Fsr2Quality = 0; // 0=Native 1=Quality 2=Balanced 3=Perf 4=Ultra
        f32 Fsr2Ratio() const {
            static const f32 R[] = { 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 1.0f / 2.0f, 1.0f / 3.0f };
            return R[Fsr2Quality < 0 ? 0 : (Fsr2Quality > 4 ? 4 : Fsr2Quality)];
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
        bool            UseAerialPerspective = false; 
        bool            UseHeightFog         = false; 

        FSunShadows     SunShadows;
        bool            UseSunShadows = true;
 
        FRaytracingScene RaytracingScene;
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

        FReflections     Reflections;
        bool             UseReflections = true;

        FAmbientOcclusion AO;
        bool              UseAO   = true;
        bool              AODebug = false;
        static constexpr f32 kKmPerWorldUnit = 0.001f;

        FCloudNoise       CloudNoise;
        FVolumetricClouds VolumetricClouds;
        bool              UseClouds = false; // off por padrao: caro e ainda nao otimizado

        FOceanFFT         Ocean;
        FWaterRenderer    Water;
        bool              UseWater  = false; 

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
