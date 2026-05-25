#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>

namespace Smile {

    Renderer::Renderer() = default;
    Renderer::~Renderer() { Shutdown(); }

    void Renderer::Initialize(HWND _hWnd, u32 _Width, u32 _Height) {
        if (Initialized) return;

    #ifdef _DEBUG
        constexpr bool kDebugLayer = true;
    #else
        constexpr bool kDebugLayer = false;
    #endif

        Device.Initialize(kDebugLayer);
        CommandQueue.Initialize(Device.Native(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        SwapChain.Initialize(Device.GetFactory(),
                             CommandQueue.Native(),
                             Device.Native(),
                             _hWnd, _Width, _Height,
                             Device.TearingSupported());
        SRVHeap.Initialize(Device.Native());
        PipelineState.Initialize(Device.Native());

        CreateGeometryBuffers();
        CreateDepthBuffer();
        CreateConstantBuffer();
        CreateDefaultMaterial();

        Initialized = true;
        LogInfo("Renderer inicializado");
    }

    // ---------------------------------------------------------------------------
    // Default material — neutral grey PBR, all 6 texture slots bound to 1×1 fallbacks
    // so the descriptor table is always fully populated (D3D12 validation requirement).
    // ---------------------------------------------------------------------------
    void Renderer::CreateDefaultMaterial() {
        auto* Dev = Device.Native();

        TexDefaultWhite  = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::White);
        TexDefaultNormal = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::FlatNormal);
        TexDefaultORM    = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::ORM);
        TexDefaultGrey   = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::MidGrey);
        TexDefaultBlack  = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::Black);

        // Assign fallback textures — HasXxxMap = 0, so shader uses factor constants
        DefaultMaterial.Albedo            = &TexDefaultWhite;
        DefaultMaterial.Normal            = &TexDefaultNormal;
        DefaultMaterial.MetallicRoughness = &TexDefaultORM;
        DefaultMaterial.AO                = &TexDefaultWhite;
        DefaultMaterial.Height            = &TexDefaultGrey;
        DefaultMaterial.Emissive          = &TexDefaultBlack;

        // Default values: slightly warm grey plastic
        DefaultMaterial.Constants.BaseColorFactor  = { 0.8f, 0.8f, 0.8f, 1.0f };
        DefaultMaterial.Constants.MetallicFactor   = 0.0f;
        DefaultMaterial.Constants.RoughnessFactor  = 0.5f;

        DefaultMaterial.Finalize(Dev, SRVHeap);
        ActiveMaterial = &DefaultMaterial;
    }

    void Renderer::SetMaterial(FMaterial* Mat) {
        ActiveMaterial = (Mat && Mat->IsFinalized()) ? Mat : &DefaultMaterial;
    }

    // ---------------------------------------------------------------------------
    // Geometry buffers
    // ---------------------------------------------------------------------------
    void Renderer::CreateGeometryBuffers() {
        FMesh CubeMesh = FMesh::CreateCube();

        const UINT VertexBufferSize = static_cast<UINT>(CubeMesh.Vertices.size() * sizeof(Vertex));
        const UINT IndexBufferSize  = static_cast<UINT>(CubeMesh.Indices.size()  * sizeof(u16));
        IndexCount = static_cast<u32>(CubeMesh.Indices.size());

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        ResourceDesc.Height           = 1;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        D3D12_RANGE NoReadRange{ 0, 0 };
        void* Mapped = nullptr;

        ResourceDesc.Width = VertexBufferSize;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&VertexBuffer)));
        SMILE_HR(VertexBuffer->Map(0, &NoReadRange, &Mapped));
        std::memcpy(Mapped, CubeMesh.Vertices.data(), VertexBufferSize);
        VertexBuffer->Unmap(0, nullptr);

        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
        VertexBufferView.SizeInBytes    = VertexBufferSize;

        ResourceDesc.Width = IndexBufferSize;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&IndexBuffer)));
        SMILE_HR(IndexBuffer->Map(0, &NoReadRange, &Mapped));
        std::memcpy(Mapped, CubeMesh.Indices.data(), IndexBufferSize);
        IndexBuffer->Unmap(0, nullptr);

        IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexBufferView.Format         = DXGI_FORMAT_R16_UINT;
        IndexBufferView.SizeInBytes    = IndexBufferSize;
    }

    void Renderer::CreateConstantBuffer() {
        constexpr UINT CBSize = sizeof(FrameConstants); // already aligned(256)

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        ResourceDesc.Width            = CBSize;
        ResourceDesc.Height           = 1;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&ConstantBuffer)));

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

        if (!DSVHeap.Native())
            DSVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

        DepthBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_D32_FLOAT;
        ResourceDesc.SampleDesc       = { MSAASampleCount, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

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

        if (MSAASampleCount <= 1 || Width == 0 || Height == 0) return;

        if (!MSAARTVHeap.Native())
            MSAARTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);

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
        Device.Native()->CreateRenderTargetView(MSAAColorBuffer.Get(), &RTVDesc,
                                                MSAARTVHeap.CpuHandle(0));
    }

    void Renderer::SetMSAA(u32 _SampleCount) {
        if (_SampleCount == MSAASampleCount || !Initialized) return;
        CommandQueue.Flush();
        MSAASampleCount = _SampleCount;
        PipelineState.RecreatePSO(Device.Native(), MSAASampleCount);
        CreateMSAABuffers();
        CreateDepthBuffer();
    }

    void Renderer::Resize(u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) return;
        CommandQueue.Flush();
        SwapChain.Resize(Device.Native(), _Width, _Height);
        CreateMSAABuffers();
        CreateDepthBuffer();
    }

    void Renderer::UpdateCamera(const CameraInput& _Input, f32 _DeltaTime) {
        Camera.Update(_Input, _DeltaTime);
    }

    // ---------------------------------------------------------------------------
    // RenderFrame
    // ---------------------------------------------------------------------------
    void Renderer::RenderFrame() {
        if (!Initialized) return;

        // Update frame constants
        f32 Aspect = SwapChain.GetWidth() > 0 && SwapChain.GetHeight() > 0
                     ? static_cast<f32>(SwapChain.GetWidth()) / static_cast<f32>(SwapChain.GetHeight())
                     : 1.0f;

        Mat44 Model      = Mat44::Identity();
        Mat44 View       = Camera.GetViewMatrix();
        Mat44 Projection = Mat44::PerspectiveFovLH(60.0f * ToRad, Aspect, 0.1f, 100.0f);

        MappedCB->MVP           = Model * View * Projection;
        MappedCB->ModelMatrix   = Model;
        Vec3 CamPos             = Camera.GetPosition();
        MappedCB->CameraPosition = { CamPos.X, CamPos.Y, CamPos.Z, 1.0f };
        MappedCB->LightPosition  = { 2.0f, 2.0f, -2.0f, 1.0f };
        MappedCB->LightColor     = { 1.0f, 0.9f, 0.8f, 8.0f };

        CommandQueue.ResetForRecording();
        auto* Cmd = CommandQueue.List();

        const bool UseMSAA = MSAASampleCount > 1 && MSAAColorBuffer;
        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        auto DSV = DSVHeap.CpuHandle(0);

        if (UseMSAA) {
            auto MSAARTV = MSAARTVHeap.CpuHandle(0);
            Cmd->OMSetRenderTargets(1, &MSAARTV, FALSE, &DSV);
            Cmd->ClearRenderTargetView(MSAARTV, ClearColor, 0, nullptr);
            Cmd->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        } else {
            D3D12_RESOURCE_BARRIER Barrier{};
            Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
            Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            Cmd->ResourceBarrier(1, &Barrier);

            auto RTV = SwapChain.CurrentRTV();
            Cmd->OMSetRenderTargets(1, &RTV, FALSE, &DSV);
            Cmd->ClearRenderTargetView(RTV, ClearColor, 0, nullptr);
            Cmd->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }

        D3D12_VIEWPORT Viewport{};
        Viewport.Width    = static_cast<FLOAT>(SwapChain.GetWidth());
        Viewport.Height   = static_cast<FLOAT>(SwapChain.GetHeight());
        Viewport.MinDepth = 0.0f;
        Viewport.MaxDepth = 1.0f;

        D3D12_RECT ScissorRect{};
        ScissorRect.right  = static_cast<LONG>(SwapChain.GetWidth());
        ScissorRect.bottom = static_cast<LONG>(SwapChain.GetHeight());

        Cmd->RSSetViewports(1, &Viewport);
        Cmd->RSSetScissorRects(1, &ScissorRect);

        // D3D12 requires SetDescriptorHeaps BEFORE any root table binding
        ID3D12DescriptorHeap* Heaps[] = { SRVHeap.Native() };
        Cmd->SetDescriptorHeaps(_countof(Heaps), Heaps);

        Cmd->SetGraphicsRootSignature(PipelineState.GetRootSignature());
        Cmd->SetPipelineState(PipelineState.PSO());

        // Root parameter 0: FrameConstants (b0)
        Cmd->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());

        // Root parameters 1 and 2: MaterialConstants (b1) + texture table (t0-t5)
        ActiveMaterial->Bind(Cmd, SRVHeap);

        Cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Cmd->IASetVertexBuffers(0, 1, &VertexBufferView);
        Cmd->IASetIndexBuffer(&IndexBufferView);
        Cmd->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);

        if (UseMSAA) {
            D3D12_RESOURCE_BARRIER Barriers[2]{};
            Barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barriers[0].Transition.pResource   = MSAAColorBuffer.Get();
            Barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            Barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            Barriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barriers[1].Transition.pResource   = SwapChain.CurrentBackBuffer();
            Barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            Barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            Cmd->ResourceBarrier(2, Barriers);

            Cmd->ResolveSubresource(SwapChain.CurrentBackBuffer(), 0,
                                    MSAAColorBuffer.Get(), 0, FSwapChain::kFormat);

            Barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            Barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            Barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            Barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            Cmd->ResourceBarrier(2, Barriers);
        } else {
            D3D12_RESOURCE_BARRIER Barrier{};
            Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
            Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            Barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            Cmd->ResourceBarrier(1, &Barrier);
        }

        SMILE_HR(Cmd->Close());
        ID3D12CommandList* Lists[] = { Cmd };
        CommandQueue.ExecuteAndSync(Lists, 1);
        SwapChain.Present();
    }

    void Renderer::Shutdown() {
        if (!Initialized) return;
        CommandQueue.Flush();
        if (ConstantBuffer && MappedCB) {
            ConstantBuffer->Unmap(0, nullptr);
            MappedCB = nullptr;
        }
        MSAAColorBuffer.Reset();
        Initialized = false;
        LogInfo("Renderer encerrado");
    }

} // namespace Smile
