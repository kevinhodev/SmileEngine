#pragma once

#include <Windows.h>
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

namespace Smile {
    struct alignas(256) FrameConstants {
        Mat44 MVP;             // 64 bytes
        Mat44 ModelMatrix;     // 64 bytes
        Vec4  CameraPosition;  // 16 bytes
        Vec4  LightPosition;   // 16 bytes
        Vec4  LightColor;      // 16 bytes
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

        void UpdateCamera(const CameraInput& Input, f32 DeltaTime);
        void RenderFrame();

        void SetMaterial(FMaterial* Material);

        bool IsInitialized() const { return Initialized; }

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

        FD3D12Device    Device;
        FCommandQueue   CommandQueue;
        FSwapChain      SwapChain;
        FPipelineState  PipelineState;
        FTextureSRVHeap SRVHeap;

        FCamera Camera;

        FTexture TexDefaultWhite;
        FTexture TexDefaultNormal;
        FTexture TexDefaultBlack;
        FTexture TexDefaultGrey;
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

        bool Initialized = false;
    };
} 
