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
#include "Smile/Graphics/OceanWater.h"
#include "Smile/Graphics/Skybox.h"
#include "Smile/Graphics/PostProcess.h"

namespace Smile {
    struct alignas(256) FrameConstants {
        Mat44 MVP;             // 64 bytes
        Mat44 ModelMatrix;     // 64 bytes
        Vec4  CameraPosition;  // 16 bytes
        Vec4  IBLParams;       // 16 bytes — x=intensity, y=rotation(rad), z=maxMip, w=enabled
        Vec4  Time;            // 16 bytes — x=elapsed sec, y=delta sec, z=frameIndex, w=unused
        Vec4  SunDirection;    // 16 bytes — xyz = direction TO sun (normalized), w = intensity
        Vec4  SunColor;        // 16 bytes — rgb = color, w = unused
        Vec4  SkyAmbientColor;    // 16 bytes — rgb = sky (zenith) ambient, w = enabled (0/1)
        Vec4  GroundAmbientColor; // 16 bytes — rgb = ground (nadir) ambient, w = intensity
        // Total: 240 bytes used of the 256-byte alignment.
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

        void UpdateCamera(const CameraInput& Input, f32 DeltaTime);
        void RenderFrame();

        void SetMaterial(FMaterial* Material);

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

        // --- Sky & Clouds editor controls ---
        // Sun placed by azimuth (around +Y) and elevation (from the horizon), deg.
        void SetSunAzimuthElevation(f32 AzimuthDeg, f32 ElevationDeg);
        void SetSunDiskSize(f32 HalfAngleDeg) { Atmosphere.SetSunDiskHalfAngle(HalfAngleDeg); }
        void SetSunGlare(f32 Intensity)       { Atmosphere.SetSunGlare(Intensity); }

        void SetCloudCoverage(f32 V)          { CloudVolumetrics.SetCoverage(V); }
        void SetCloudDensity(f32 V)           { CloudVolumetrics.SetDensityScale(V); }
        void SetCloudAltitude(f32 BottomKm, f32 ThicknessKm) { CloudVolumetrics.SetAltitude(BottomKm, ThicknessKm); }
        void SetCloudWind(f32 V)              { CloudVolumetrics.SetWindSpeed(V); }
        void SetCloudPhaseG(f32 V)            { CloudVolumetrics.SetPhaseG(V); }
        void SetCloudPowder(f32 V)            { CloudVolumetrics.SetPowder(V); }
        void SetCloudErosion(f32 V)           { CloudVolumetrics.SetErosion(V); }

        void SetUseOceanWater(bool Use)       { OceanWater.SetEnabled(Use); }
        void SetOceanWaterLevel(f32 LevelY)   { OceanWater.SetWaterLevel(LevelY); }
        void SetOceanWind(f32 DirectionRad, f32 Speed) { OceanWater.SetWind(DirectionRad, Speed); }
        void SetOceanWaveParams(f32 WaveAmount, f32 WaveSize, f32 Choppiness) {
            OceanWater.SetWaveParams(WaveAmount, WaveSize, Choppiness);
        }

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
        void CreateGeometryBuffers();
        void CreateDepthBuffer();
        void CreateConstantBuffer();
        void CreateMSAABuffers();
        void CreateHDRBuffers();
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

        ComPtr<ID3D12Resource>   VertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};

        ComPtr<ID3D12Resource>   IndexBuffer;
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        u32                      IndexCount = 0;

        ComPtr<ID3D12Resource>   DepthBuffer;
        FDescriptorHeap          DSVHeap;
        // Depth-as-SRV (R32_FLOAT view over the R32_TYPELESS depth resource).
        // Allocated once; re-pointed at the resource whenever depth is recreated.
        static constexpr u32     kInvalidSlot = 0xFFFFFFFFu;
        u32                      DepthSRVSlot = kInvalidSlot;

        ComPtr<ID3D12Resource>   ConstantBuffer;
        FrameConstants*          MappedCB = nullptr;

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
        FOceanWater       OceanWater;
        bool              UseClouds = true;
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
