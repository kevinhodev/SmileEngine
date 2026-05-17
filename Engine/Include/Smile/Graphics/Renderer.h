#pragma once

#include <Windows.h>
#include "Smile/Math/Math.h"
#include "Smile/Input/CameraInput.h"
#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/SwapChain.h"
#include "Smile/Graphics/PipelineState.h"

namespace Smile {

// Dados enviados ao shader via Constant Buffer (deve ser alinhado a 256 bytes)
struct alignas(256) FrameConstants {
    Mat44 MVP;             // 64 bytes — row_major float4x4 no HLSL
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

    void Initialize(HWND hwnd, u32 width, u32 height);
    void Shutdown();

    void Resize(u32 width, u32 height);
    void SetMSAA(u32 sampleCount);

    void UpdateCamera(const CameraInput& input, f32 dt);
    void RenderFrame();

    bool IsInitialized() const { return Initialized; }

    Vec3 GetCameraPos() const { return CameraPos; }
    f32  GetPitch()     const { return Pitch; }
    f32  GetYaw()       const { return Yaw; }
    u32  GetMSAA()      const { return MSAASampleCount; }

    const D3D12Device& GetDevice() const { return Device; }

private:
    void CreateCubeBuffers();
    void CreateDepthBuffer();
    void CreateConstantBuffer();
    void CreateMSAABuffers();

    D3D12Device   Device;
    CommandQueue  CommandQueue;
    SwapChain     SwapChain;
    PipelineState PipelineState;

    ComPtr<ID3D12Resource>   VertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    
    ComPtr<ID3D12Resource>   IndexBuffer;
    D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};

    ComPtr<ID3D12Resource>   DepthBuffer;
    DescriptorHeap           DSVHeap;

    // Constant Buffer (MVP) — persistently mapped
    ComPtr<ID3D12Resource>   ConstantBuffer;
    FrameConstants*          MappedCB = nullptr;

    // MSAA off-screen render target (sampleCount > 1)
    ComPtr<ID3D12Resource>   MSAAColorBuffer;
    DescriptorHeap           MSAARTVHeap;
    u32                      MSAASampleCount = 1;

    // Estado da câmera — atualizado via UpdateCamera()
    Vec3 CameraPos = { 0.0f, 1.0f, -2.5f };
    f32  Pitch     = -22.0f;   // graus; corresponde à vista inicial do cubo
    f32  Yaw       = 0.0f;     // graus

    bool Initialized = false;
};

} // namespace Smile

