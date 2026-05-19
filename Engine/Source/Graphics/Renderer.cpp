#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Core/VersionInfo.h"
#include <cstring>
#include <format>
#include <intrin.h>
#include <windows.h>

namespace Smile {
    Renderer::Renderer() = default;

    Renderer::~Renderer() { Shutdown(); }

    void Renderer::Initialize(HWND _hWnd, u32 _Width, u32 _Height) {
        if (Initialized) {
            return;
        }

    #ifdef _DEBUG
        constexpr bool kDebugLayer = true;
    #else
        constexpr bool kDebugLayer = false;
    #endif

        // ── System info ───────────────────────────────────────────────────────
        LogInfo(std::format("SmileEngine v{}", SMILE_VERSION_STRING));
        {
            // CPU brand string via __cpuid (MSVC/clang-cl)
            int cpuInfo[4] = {};
            char brand[49] = {};
            __cpuid(cpuInfo, 0x80000002);
            std::memcpy(brand,      cpuInfo, 16);
            __cpuid(cpuInfo, 0x80000003);
            std::memcpy(brand + 16, cpuInfo, 16);
            __cpuid(cpuInfo, 0x80000004);
            std::memcpy(brand + 32, cpuInfo, 16);
            brand[48] = '\0';
            // trim leading spaces
            const char* cpuName = brand;
            while (*cpuName == ' ') ++cpuName;

            SYSTEM_INFO si{};
            GetSystemInfo(&si);

            LogInfo(std::format("CPU: {} | {} Núcleos Lógicos", cpuName, si.dwNumberOfProcessors));
        }
        {
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            double totalGb = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
            LogInfo(std::format("RAM: {:.1f} GB", totalGb));
        }

        Device.Initialize(kDebugLayer);
        CommandQueue.Initialize(Device.Native(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        SwapChain.Initialize(Device.GetFactory(),
                               CommandQueue.Native(),
                               Device.Native(),
                               _hWnd,
                               _Width,
                               _Height,
                               Device.TearingSupported());
        PipelineState.Initialize(Device.Native());

        CreateGeometryBuffers();
        CreateDepthBuffer();
        CreateConstantBuffer();

        Initialized = true;
        LogInfo(std::format("Renderer Inicializado | Viewport {}x{}", _Width, _Height));
    }

    void Renderer::CreateGeometryBuffers() {
        FMesh CubeMesh = FMesh::CreateCube();

        const UINT VertexBufferSize = static_cast<UINT>(CubeMesh.Vertices.size() * sizeof(Vertex));
        const UINT IndexBufferSize = static_cast<UINT>(CubeMesh.Indices.size()  * sizeof(u16));
        IndexCount = static_cast<u32>(CubeMesh.Indices.size());

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
        HeapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        HeapProps.CreationNodeMask     = 1;
        HeapProps.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        ResourceDesc.Alignment          = 0;
        ResourceDesc.Height             = 1;
        ResourceDesc.DepthOrArraySize   = 1;
        ResourceDesc.MipLevels          = 1;
        ResourceDesc.Format             = DXGI_FORMAT_UNKNOWN;
        ResourceDesc.SampleDesc         = { 1, 0 };
        ResourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ResourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        D3D12_RANGE NoReadRange{ 0, 0 };
        void* Mapped = nullptr;

        ResourceDesc.Width = VertexBufferSize;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VertexBuffer)));

        SMILE_HR(VertexBuffer->Map(0, &NoReadRange, &Mapped));
        std::memcpy(Mapped, CubeMesh.Vertices.data(), VertexBufferSize);
        VertexBuffer->Unmap(0, nullptr);

        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
        VertexBufferView.SizeInBytes    = VertexBufferSize;

        ResourceDesc.Width = IndexBufferSize;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&IndexBuffer)));

        SMILE_HR(IndexBuffer->Map(0, &NoReadRange, &Mapped));
        std::memcpy(Mapped, CubeMesh.Indices.data(), IndexBufferSize);
        IndexBuffer->Unmap(0, nullptr);

        IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexBufferView.Format         = DXGI_FORMAT_R16_UINT;
        IndexBufferView.SizeInBytes    = IndexBufferSize;
    }

    void Renderer::CreateConstantBuffer() {
        constexpr UINT ConstantBufferSize = (sizeof(FrameConstants) + 255) & ~255;

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        ResourceDesc.Width              = ConstantBufferSize;
        ResourceDesc.Height             = 1;
        ResourceDesc.DepthOrArraySize   = 1;
        ResourceDesc.MipLevels          = 1;
        ResourceDesc.Format             = DXGI_FORMAT_UNKNOWN;
        ResourceDesc.SampleDesc         = { 1, 0 };
        ResourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ResourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoReadRange{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoReadRange, &Ptr));
        MappedCB = reinterpret_cast<FrameConstants*>(Ptr);

        MappedCB->MVP = Mat44::Identity();
    }

    void Renderer::CreateDepthBuffer() {
        UINT Width  = SwapChain.GetWidth();
        UINT Height = SwapChain.GetHeight();
        if (Width == 0 || Height == 0) return;

        if (!DSVHeap.Native()) {
            DSVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
        }

        DepthBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width              = Width;
        ResourceDesc.Height             = Height;
        ResourceDesc.DepthOrArraySize   = 1;
        ResourceDesc.MipLevels          = 1;
        ResourceDesc.Format             = DXGI_FORMAT_D32_FLOAT;
        ResourceDesc.SampleDesc         = { MSAASampleCount, 0 };
        ResourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format               = DXGI_FORMAT_D32_FLOAT;
        ClearValue.DepthStencil.Depth   = 1.0f;
        ClearValue.DepthStencil.Stencil = 0;

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &ClearValue,
            IID_PPV_ARGS(&DepthBuffer)));

        D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        DSVDesc.ViewDimension = MSAASampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                    : D3D12_DSV_DIMENSION_TEXTURE2D;
        DSVDesc.Texture2D.MipSlice = 0;
        Device.Native()->CreateDepthStencilView(DepthBuffer.Get(), &DSVDesc, DSVHeap.CpuHandle(0));
    }

    void Renderer::CreateMSAABuffers() {
        UINT Width  = SwapChain.GetWidth();
        UINT Height = SwapChain.GetHeight();

        MSAAColorBuffer.Reset();

        if (MSAASampleCount <= 1 || Width == 0 || Height == 0) {
            return;
        }

        if (!MSAARTVHeap.Native()) {
            MSAARTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
        }

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = FSwapChain::kFormat;
        ResourceDesc.SampleDesc       = { MSAASampleCount, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = FSwapChain::kFormat;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &ClearValue,
            IID_PPV_ARGS(&MSAAColorBuffer)));

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = FSwapChain::kFormat;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        Device.Native()->CreateRenderTargetView(MSAAColorBuffer.Get(), &RTVDesc, MSAARTVHeap.CpuHandle(0));
    }

    void Renderer::SetMSAA(u32 _SampleCount) {
        if (_SampleCount == MSAASampleCount || !Initialized) {
            return;
        }

        CommandQueue.Flush();
        MSAASampleCount = _SampleCount;
        PipelineState.RecreatePSO(Device.Native(), MSAASampleCount);
        CreateMSAABuffers();
        CreateDepthBuffer();
    }

    void Renderer::Resize(u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) {
            return;
        }

        CommandQueue.Flush();
        SwapChain.Resize(Device.Native(), _Width, _Height);
        CreateMSAABuffers();
        CreateDepthBuffer();
    }

    void Renderer::UpdateCamera(const CameraInput& _Input, f32 _DeltaTime) {
        Camera.Update(_Input, _DeltaTime);
    }

    void Renderer::RenderFrame() {
        if (!Initialized) {
            return;
        }

        f32 Aspect = SwapChain.GetWidth() > 0 && SwapChain.GetHeight() > 0
                     ? static_cast<f32>(SwapChain.GetWidth()) / static_cast<f32>(SwapChain.GetHeight())
                     : 1.0f;

        Mat44 ModelMatrix = Mat44::Identity();
        Mat44 ViewMatrix  = Camera.GetViewMatrix();

        constexpr f32 kFovY = 60.0f * ToRad;
        Mat44 ProjectionMatrix = Mat44::PerspectiveFovLH(kFovY, Aspect, 0.1f, 100.0f);

        MappedCB->MVP         = ModelMatrix * ViewMatrix * ProjectionMatrix;
        MappedCB->ModelMatrix = ModelMatrix;

        Vec3 CameraPosition = Camera.GetPosition();
        MappedCB->CameraPosition = Vec4{CameraPosition.X, CameraPosition.Y, CameraPosition.Z, 1.0f};
        MappedCB->LightPosition  = Vec4{2.0f, 2.0f, -2.0f, 1.0f};
        MappedCB->LightColor     = Vec4{1.0f, 0.9f, 0.8f, 8.0f};

        CommandQueue.ResetForRecording();
        auto* CommandList = CommandQueue.List();

        const bool UseMSAA = MSAASampleCount > 1 && MSAAColorBuffer;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        auto DSV = DSVHeap.CpuHandle(0);

        if (UseMSAA) {
            auto MSAARTV = MSAARTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &MSAARTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(MSAARTV, ClearColor, 0, nullptr);
            CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        } else {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            ResourceBarrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            CommandList->ResourceBarrier(1, &ResourceBarrier);

            auto RTV = SwapChain.CurrentRTV();
            CommandList->OMSetRenderTargets(1, &RTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(RTV, ClearColor, 0, nullptr);
            CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }

        D3D12_VIEWPORT Viewport{};
        Viewport.Width    = static_cast<FLOAT>(SwapChain.GetWidth());
        Viewport.Height   = static_cast<FLOAT>(SwapChain.GetHeight());
        Viewport.MinDepth = 0.0f;
        Viewport.MaxDepth = 1.0f;

        D3D12_RECT ScissorRect{};
        ScissorRect.right  = static_cast<LONG>(SwapChain.GetWidth());
        ScissorRect.bottom = static_cast<LONG>(SwapChain.GetHeight());

        CommandList->RSSetViewports(1, &Viewport);
        CommandList->RSSetScissorRects(1, &ScissorRect);

        CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
        CommandList->SetPipelineState(PipelineState.PSO());
        CommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
        CommandList->IASetIndexBuffer(&IndexBufferView);
        CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);

        if (UseMSAA) {
            D3D12_RESOURCE_BARRIER MSAAResourceBarriers[2]{};
            MSAAResourceBarriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            MSAAResourceBarriers[0].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            MSAAResourceBarriers[0].Transition.pResource   = MSAAColorBuffer.Get();
            MSAAResourceBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            MSAAResourceBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            MSAAResourceBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            MSAAResourceBarriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            MSAAResourceBarriers[1].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            MSAAResourceBarriers[1].Transition.pResource   = SwapChain.CurrentBackBuffer();
            MSAAResourceBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            MSAAResourceBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            MSAAResourceBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            CommandList->ResourceBarrier(2, MSAAResourceBarriers);

            CommandList->ResolveSubresource(SwapChain.CurrentBackBuffer(), 0,
                                            MSAAColorBuffer.Get(), 0, FSwapChain::kFormat);

            MSAAResourceBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            MSAAResourceBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            MSAAResourceBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            MSAAResourceBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            CommandList->ResourceBarrier(2, MSAAResourceBarriers);
        } else {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            ResourceBarrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            CommandList->ResourceBarrier(1, &ResourceBarrier);
        }

        SMILE_HR(CommandList->Close());

        ID3D12CommandList* CommandLists[] = { CommandList };
        CommandQueue.ExecuteAndSync(CommandLists, 1);

        SwapChain.Present();
    }

    void Renderer::Shutdown() {
        if (!Initialized) {
            return;
        }

        CommandQueue.Flush();

        if (ConstantBuffer && MappedCB) {
            ConstantBuffer->Unmap(0, nullptr);
            MappedCB = nullptr;
        }

        MSAAColorBuffer.Reset();
        Initialized = false;
        LogInfo("Renderer encerrado");
    }
} 
