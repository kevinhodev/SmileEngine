#pragma once

#include <Windows.h>
#include <string>
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
#include "Smile/Graphics/HDREnvironment.h"
#include "Smile/Graphics/Atmosphere.h"
#include "Smile/Graphics/CloudNoise.h"
#include "Smile/Graphics/VolumetricClouds.h"
#include "Smile/Graphics/Skybox.h"
#include "Smile/Graphics/PostProcess.h"
#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Graphics/Water.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Scene/Scene.h"

namespace Smile {
    // Globais por-frame (b0): iguais para todos os objetos da cena.
    struct alignas(256) FrameConstants {
        Vec4  CameraPosition;  // 16 bytes
        Vec4  IBLParams;       // 16 bytes — x=intensity, y=rotation(rad), z=maxMip, w=enabled
        Vec4  Time;            // 16 bytes — x=elapsed sec, y=delta sec, z=frameIndex, w=unused
        Vec4  SunDirection;    // 16 bytes — xyz = direction TO sun (normalized), w = intensity
        Vec4  SunColor;        // 16 bytes — rgb = color, w = unused
        Vec4  SkyAmbientColor;    // 16 bytes — rgb = sky (zenith) ambient, w = enabled (0/1)
        Vec4  GroundAmbientColor; // 16 bytes — rgb = ground (nadir) ambient, w = intensity
    };

    // Constantes por-objeto (b2): escritas uma vez por renderavel, por frame.
    struct alignas(256) ObjectConstants {
        Mat44 MVP;          // 64 bytes — Model * View * Projection
        Mat44 ModelMatrix;  // 64 bytes — world (para worldPos/worldNormal)
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
        void SetMSAA(u32 SampleCount);
        bool ReloadShaders();

        // VSync (pacing do Present). On = trava no refresh; Off = FPS livre.
        void SetVSync(bool Enabled) { SwapChain.SetVSync(Enabled); }
        bool GetVSync() const       { return SwapChain.GetVSync(); }

        void UpdateCamera(const CameraInput& Input, f32 DeltaTime);
        void RenderFrame();

        void SetMaterial(FMaterial* Material);

        // Cena multi-objeto. O editor pode adicionar meshes/renderaveis aqui;
        // o Renderer itera a lista em RenderFrame.
        FScene& GetScene() { return Scene; }

        bool IsInitialized() const { return Initialized; }

        // IBL controls. LoadHDREnvironment returns false on file/IO failure.
        bool LoadHDREnvironment(const std::wstring& Path);
        void SetIBLIntensity(f32 Intensity)  { IBLIntensity = Intensity; }
        void SetIBLRotation(f32 Radians)     { IBLRotation  = Radians; }
        void SetShowSkybox(bool Show)        { ShowSkybox   = Show; }
        f32  GetIBLIntensity() const         { return IBLIntensity; }
        f32  GetIBLRotation()  const         { return IBLRotation; }
        bool GetShowSkybox()   const         { return ShowSkybox; }

        // Directional sun (unified light: drives PBR scene + atmosphere + clouds).
        // Direction is the world-space direction TOWARD the sun (will be normalized).
        void SetSunDirection(const Vec3& Dir);
        void SetSunColor(const Vec3& Color)  { SunColorRGB = Color; }
        void SetSunIntensity(f32 Intensity)  { SunIntensity = Intensity; }
        Vec3 GetSunDirection() const         { return SunDir; }
        Vec3 GetSunColor()     const         { return SunColorRGB; }
        f32  GetSunIntensity() const         { return SunIntensity; }

        // Scene depth exposed as an SRV (R32_FLOAT) for the atmosphere/cloud passes.
        u32  GetDepthSRVSlot() const         { return DepthSRVSlot; }

        // Atmosphere sky toggle (coexists with the HDR skybox). When on, the
        // physical sky replaces the HDR cubemap background.
        void SetUseAtmosphereSky(bool Use)   { UseAtmosphereSky = Use; }
        bool GetUseAtmosphereSky() const     { return UseAtmosphereSky; }
        void SetUseClouds(bool Use)          { UseClouds = Use; }
        bool GetUseClouds() const            { return UseClouds; }

        // Ocean (port fiel da CryEngine). Toggle + acesso ao subsistema p/ setters do editor.
        void SetUseWater(bool Use)           { UseWater = Use; }
        bool GetUseWater() const             { return UseWater; }
        FWaterRenderer& GetWater()           { return Water; }

        // --- Sky & Clouds editor controls ---
        // Sun placed by azimuth (around +Y) and elevation (from the horizon), deg.
        void SetSunAzimuthElevation(f32 AzimuthDeg, f32 ElevationDeg);
        void SetSunDiskSize(f32 HalfAngleDeg) { Atmosphere.SetSunDiskHalfAngle(HalfAngleDeg); }
        void SetSunGlare(f32 Intensity)       { Atmosphere.SetSunGlare(Intensity); }
        f32  GetSunDiskSize() const           { return Atmosphere.GetSunDiskHalfAngle(); }
        f32  GetSunGlare() const              { return Atmosphere.GetSunGlare(); }

        void SetCloudCoverage(f32 V)          { CloudVolumetrics.SetCoverage(V); }
        void SetCloudDensity(f32 V)           { CloudVolumetrics.SetDensityScale(V); }
        void SetCloudAltitude(f32 BottomKm, f32 ThicknessKm) { CloudVolumetrics.SetAltitude(BottomKm, ThicknessKm); }
        void SetCloudWind(f32 V)              { CloudVolumetrics.SetWindSpeed(V); }
        void SetCloudPhaseG(f32 V)            { CloudVolumetrics.SetPhaseG(V); }
        void SetCloudPowder(f32 V)            { CloudVolumetrics.SetPowder(V); }
        void SetCloudErosion(f32 V)           { CloudVolumetrics.SetErosion(V); }
        f32  GetCloudCoverage() const         { return CloudVolumetrics.GetCoverage(); }
        f32  GetCloudDensity() const          { return CloudVolumetrics.GetDensityScale(); }
        f32  GetCloudWind() const             { return CloudVolumetrics.GetWindSpeed(); }
        f32  GetCloudPhaseG() const           { return CloudVolumetrics.GetPhaseG(); }
        f32  GetCloudPowder() const           { return CloudVolumetrics.GetPowder(); }
        f32  GetCloudErosion() const          { return CloudVolumetrics.GetErosion(); }
        f32  GetCloudBottomAltitude() const   { return CloudVolumetrics.GetBottomAltitude(); }
        f32  GetCloudThickness() const        { return CloudVolumetrics.GetThickness(); }

        // Atmosphere-derived hemispheric ambient for the PBR scene (A4).
        void SetUseAtmosphereAmbient(bool Use)      { UseAtmosphereAmbient = Use; }
        bool GetUseAtmosphereAmbient() const        { return UseAtmosphereAmbient; }
        void SetAtmosphereAmbientIntensity(f32 I)   { AtmoAmbientIntensity = I; }
        f32  GetAtmosphereAmbientIntensity() const  { return AtmoAmbientIntensity; }

        Vec3 GetCameraPos() const { return Camera.GetPosition(); }
        f32  GetPitch()     const { return Camera.GetPitch(); }
        f32  GetYaw()       const { return Camera.GetYaw(); }
        u32  GetMSAA()      const { return MSAASampleCount; }

        const FD3D12Device& GetDevice()  const { return Device; }
        FCommandQueue&      GetCmdQueue()      { return CommandQueue; }
        FTextureSRVHeap&    GetSRVHeap()       { return SRVHeap; }

    private:
        void BuildDefaultScene();
        void CreateDepthBuffer();
        void CreateConstantBuffer();
        void CreateMSAABuffers();
        void CreateHDRBuffers();
        void CreateSceneCopies(); // scene-color + scene-depth p/ refracao da agua (Etapa 3)
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

        // Cena multi-objeto (biblioteca de meshes + renderaveis).
        FScene Scene;

        ComPtr<ID3D12Resource>   DepthBuffer;
        FDescriptorHeap          DSVHeap;
        // Depth-as-SRV (R32_FLOAT view over the R32_TYPELESS depth resource).
        // Allocated once; re-pointed at the resource whenever depth is recreated.
        static constexpr u32     kInvalidSlot = 0xFFFFFFFFu;
        u32                      DepthSRVSlot = kInvalidSlot;

        // CBs por-frame, double-buffered (kFramesInFlight copias contiguas). Indexados
        // pelo CommandQueue.FrameIndex() para nao sobrescrever dados que a GPU ainda le
        // quando os frames in flight estiverem ligados.
        ComPtr<ID3D12Resource>   ConstantBuffer;   // N * FrameConstants (b0)
        u8*                      MappedFrameBase = nullptr;

        // Buffer por-objeto: N * kMaxObjects slots de ObjectConstants (256B cada) num
        // upload heap mapeado. Cada renderavel ocupa um slot dentro da regiao do frame;
        // bind via CBV offset por draw.
        static constexpr u32     kMaxObjects = 1024;
        ComPtr<ID3D12Resource>   ObjectCB;          // N * kMaxObjects * ObjectConstants (b2)
        u8*                      MappedObjectCB = nullptr;

        ComPtr<ID3D12Resource>   MSAAColorBuffer;
        FDescriptorHeap          MSAARTVHeap;
        u32                      MSAASampleCount = 1;

        // HDR targets & post processing
        ComPtr<ID3D12Resource>   HDRColorBuffer;
        ComPtr<ID3D12Resource>   HDRMSAAColorBuffer;
        FDescriptorHeap          HDRRTVHeap;
        u32                      HDRSRVSlot = kInvalidSlot;
        FPostProcessor           PostProcessor;

        // IBL: HDR environment chain + skybox renderer.
        FHDREnvironment HDREnv;
        FSkybox         Skybox;
        // Physical atmosphere (Hillaire): LUTs + sky-view + sky render.
        FAtmosphere     Atmosphere;
        bool            UseAtmosphereSky = true; // default outdoor sky
        // Volumetric clouds: 3D noise volumes (B1) + raymarch/composite (B2).
        FCloudNoise       CloudNoise;
        FVolumetricClouds CloudVolumetrics;
        bool              UseClouds = true;
        // Ocean: simulacao FFT (CPU, port do CWaterSim) + superficie (projected grid).
        FOceanFFT         Ocean;
        FWaterRenderer    Water;
        bool              UseWater  = true;
        // Copias da cena (pre-agua) p/ refracao/fog da agua (Etapa 3). Tabela SRV contigua
        // [color(t2), depth(t3)]. So no caminho sem MSAA (depth MSAA nao copia single-sample).
        ComPtr<ID3D12Resource> SceneColorCopy;
        ComPtr<ID3D12Resource> SceneDepthCopy;
        u32                    SceneCopyTableStart = kInvalidSlot;
        D3D12_RESOURCE_STATES  SceneColorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_RESOURCE_STATES  SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        bool            ShowSkybox    = true;
        f32             IBLIntensity  = 1.0f;
        f32             IBLRotation   = 0.0f; // radians, Y axis
        // Contiguous IBL descriptor table slot (irradiance, prefiltered, BRDF LUT).
        u32             IBLTableStart = 0;

        // Directional sun (default: late-morning sun, warm white).
        Vec3 SunDir       = { 0.3f, 0.6f, 0.5f }; // direction TO sun (normalized in setter/use)
        Vec3 SunColorRGB  = { 1.0f, 0.96f, 0.9f };
        f32  SunIntensity = 2.5f;

        // Atmosphere-derived ambient (A4). On by default so the scene matches the sky.
        bool UseAtmosphereAmbient  = true;
        f32  AtmoAmbientIntensity  = 1.0f;

        // Per-frame time, fed from UpdateCamera's delta.
        f32  ElapsedTime   = 0.0f;
        f32  LastDeltaTime = 0.0f;
        u32  FrameIndex    = 0;

        bool Initialized = false;
    };
} 
