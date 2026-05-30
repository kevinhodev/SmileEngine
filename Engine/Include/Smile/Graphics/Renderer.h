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
#include "Smile/Graphics/Skybox.h"

namespace Smile {
    struct alignas(256) FrameConstants {
        Mat44 MVP;             // 64 bytes
        Mat44 ModelMatrix;     // 64 bytes
        Vec4  CameraPosition;  // 16 bytes
        Vec4  LightPosition;   // 16 bytes
        Vec4  LightColor;      // 16 bytes
        Vec4  IBLParams;       // 16 bytes — x=intensity, y=rotation(rad), z=maxMip, w=enabled
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

        ComPtr<ID3D12Resource>   ConstantBuffer;
        FrameConstants*          MappedCB = nullptr;

        ComPtr<ID3D12Resource>   MSAAColorBuffer;
        FDescriptorHeap          MSAARTVHeap;
        u32                      MSAASampleCount = 1;

        // IBL: HDR environment chain + skybox renderer.
        FHDREnvironment HDREnv;
        FSkybox         Skybox;
        bool            ShowSkybox    = true;
        f32             IBLIntensity  = 1.0f;
        f32             IBLRotation   = 0.0f; // radians, Y axis
        // Contiguous IBL descriptor table slot (irradiance, prefiltered, BRDF LUT).
        u32             IBLTableStart = 0;

        bool Initialized = false;
    };
} 
