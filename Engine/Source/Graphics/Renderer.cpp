#include "Smile/Graphics/Renderer.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <cmath>

namespace Smile {

namespace {
    struct DemoVertex {
        f32 pos[3];
        f32 normal[3];
    };
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    Shutdown();
}

void Renderer::Initialize(HWND _hWnd, u32 _Width, u32 _Height) {
    if (Initialized) {
        return;
    }

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
                           _hWnd,
                           _Width,
                           _Height,
                           Device.TearingSupported());
    PipelineState.Initialize(Device.Native());

    CreateCubeBuffers();
    CreateDepthBuffer();
    CreateConstantBuffer();

    Initialized = true;
    LogInfo("Renderer inicializado");
}

void Renderer::CreateCubeBuffers() {
    // Cubo unitario centrado na origem (1x1x1, de -0.5 a +0.5)
    // Vertices em coordenadas de MODELO — a transformacao e feita pela GPU via MVP
    constexpr f32 S = 0.5f;

    // 6 normais distintas por face
    constexpr f32 normFront[3]  = { 0.0f,  0.0f, -1.0f };
    constexpr f32 normBack[3]   = { 0.0f,  0.0f,  1.0f };
    constexpr f32 normBottom[3] = { 0.0f, -1.0f,  0.0f };
    constexpr f32 normTop[3]    = { 0.0f,  1.0f,  0.0f };
    constexpr f32 normLeft[3]   = {-1.0f,  0.0f,  0.0f };
    constexpr f32 normRight[3]  = { 1.0f,  0.0f,  0.0f };

    // 24 vertices (4 por face, cada face com normal unica)
    const DemoVertex vertices[24] = {
        // Front (Z = -S)
        {{-S,  S, -S}, {normFront[0], normFront[1], normFront[2]}},
        {{ S,  S, -S}, {normFront[0], normFront[1], normFront[2]}},
        {{ S, -S, -S}, {normFront[0], normFront[1], normFront[2]}},
        {{-S, -S, -S}, {normFront[0], normFront[1], normFront[2]}},
        // Back (Z = +S)
        {{ S,  S,  S}, {normBack[0], normBack[1], normBack[2]}},
        {{-S,  S,  S}, {normBack[0], normBack[1], normBack[2]}},
        {{-S, -S,  S}, {normBack[0], normBack[1], normBack[2]}},
        {{ S, -S,  S}, {normBack[0], normBack[1], normBack[2]}},
        // Top (Y = +S)
        {{-S,  S,  S}, {normTop[0], normTop[1], normTop[2]}},
        {{ S,  S,  S}, {normTop[0], normTop[1], normTop[2]}},
        {{ S,  S, -S}, {normTop[0], normTop[1], normTop[2]}},
        {{-S,  S, -S}, {normTop[0], normTop[1], normTop[2]}},
        // Bottom (Y = -S)
        {{-S, -S, -S}, {normBottom[0], normBottom[1], normBottom[2]}},
        {{ S, -S, -S}, {normBottom[0], normBottom[1], normBottom[2]}},
        {{ S, -S,  S}, {normBottom[0], normBottom[1], normBottom[2]}},
        {{-S, -S,  S}, {normBottom[0], normBottom[1], normBottom[2]}},
        // Left (X = -S)
        {{-S,  S,  S}, {normLeft[0], normLeft[1], normLeft[2]}},
        {{-S,  S, -S}, {normLeft[0], normLeft[1], normLeft[2]}},
        {{-S, -S, -S}, {normLeft[0], normLeft[1], normLeft[2]}},
        {{-S, -S,  S}, {normLeft[0], normLeft[1], normLeft[2]}},
        // Right (X = +S)
        {{ S,  S, -S}, {normRight[0], normRight[1], normRight[2]}},
        {{ S,  S,  S}, {normRight[0], normRight[1], normRight[2]}},
        {{ S, -S,  S}, {normRight[0], normRight[1], normRight[2]}},
        {{ S, -S, -S}, {normRight[0], normRight[1], normRight[2]}},
    };

    // 36 indices — 2 triangulos por face
    const u16 indices[36] = {
         0,  1,  2,   0,  2,  3,  // Front
         4,  5,  6,   4,  6,  7,  // Back
         8,  9, 10,   8, 10, 11,  // Top
        12, 13, 14,  12, 14, 15,  // Bottom
        16, 17, 18,  16, 18, 19,  // Left
        20, 21, 22,  20, 22, 23,  // Right
    };

    constexpr UINT vBufferSize = sizeof(vertices);
    constexpr UINT iBufferSize = sizeof(indices);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask     = 1;
    heapProps.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Alignment          = 0;
    resDesc.Height             = 1;
    resDesc.DepthOrArraySize   = 1;
    resDesc.MipLevels          = 1;
    resDesc.Format             = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc         = { 1, 0 };
    resDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // Vertex Buffer
    resDesc.Width = vBufferSize;
    SMILE_HR(Device.Native()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VertexBuffer)));

    void* mapped = nullptr;
    D3D12_RANGE noReadRange{ 0, 0 };
    SMILE_HR(VertexBuffer->Map(0, &noReadRange, &mapped));
    std::memcpy(mapped, vertices, vBufferSize);
    VertexBuffer->Unmap(0, nullptr);

    VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
    VertexBufferView.StrideInBytes  = sizeof(DemoVertex);
    VertexBufferView.SizeInBytes    = vBufferSize;

    // Index Buffer
    resDesc.Width = iBufferSize;
    SMILE_HR(Device.Native()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&IndexBuffer)));

    SMILE_HR(IndexBuffer->Map(0, &noReadRange, &mapped));
    std::memcpy(mapped, indices, iBufferSize);
    IndexBuffer->Unmap(0, nullptr);

    IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
    IndexBufferView.Format         = DXGI_FORMAT_R16_UINT;
    IndexBufferView.SizeInBytes    = iBufferSize;
}

void Renderer::CreateConstantBuffer() {
    // D3D12 exige que Constant Buffers tenham tamanho multiplo de 256 bytes
    constexpr UINT cbSize = (sizeof(FrameConstants) + 255) & ~255;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width              = cbSize;
    resDesc.Height             = 1;
    resDesc.DepthOrArraySize   = 1;
    resDesc.MipLevels          = 1;
    resDesc.Format             = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc         = { 1, 0 };
    resDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    SMILE_HR(Device.Native()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ConstantBuffer)));

    // Mapeamento persistente — fica mapeado ate o Shutdown
    D3D12_RANGE noReadRange{ 0, 0 };
    void* ptr = nullptr;
    SMILE_HR(ConstantBuffer->Map(0, &noReadRange, &ptr));
    MappedCB = reinterpret_cast<FrameConstants*>(ptr);

    MappedCB->MVP = Mat44::Identity();
}

void Renderer::CreateDepthBuffer() {
    UINT Width = SwapChain.Width();
    UINT Height = SwapChain.Height();
    if (Width == 0 || Height == 0) return;

    if (!DSVHeap.Native()) {
        DSVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    }

    DepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width              = Width;
    depthDesc.Height             = Height;
    depthDesc.DepthOrArraySize   = 1;
    depthDesc.MipLevels          = 1;
    depthDesc.Format             = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc         = { MSAASampleCount, 0 };
    depthDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format               = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth   = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    SMILE_HR(Device.Native()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&DepthBuffer)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = MSAASampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                : D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    Device.Native()->CreateDepthStencilView(DepthBuffer.Get(), &dsvDesc, DSVHeap.CpuHandle(0));
}

void Renderer::CreateMSAABuffers() {
    UINT width  = SwapChain.Width();
    UINT height = SwapChain.Height();

    MSAAColorBuffer.Reset();

    if (MSAASampleCount <= 1 || width == 0 || height == 0) {
        return;
    }

    if (!MSAARTVHeap.Native()) {
        MSAARTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = SwapChain::kFormat;
    desc.SampleDesc       = { MSAASampleCount, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    const FLOAT clearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = SwapChain::kFormat;
    std::memcpy(clearValue.Color, clearColor, sizeof(clearColor));

    SMILE_HR(Device.Native()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
        IID_PPV_ARGS(&MSAAColorBuffer)));

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format        = SwapChain::kFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    Device.Native()->CreateRenderTargetView(MSAAColorBuffer.Get(), &rtvDesc, MSAARTVHeap.CpuHandle(0));
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

void Renderer::UpdateCamera(const CameraInput& input, f32 dt) {
    // Rotação: yaw livre, pitch clampado em ±89° (padrão UE)
    Yaw   += input.Look.X;
    Pitch  = Clamp(Pitch + input.Look.Y, -89.0f, 89.0f);

    f32 yawRad   = Yaw   * ToRad;
    f32 pitchRad = Pitch * ToRad;

    // W/S seguem a direção exata da câmera (inclui pitch)
    Vec3 forward = {
        std::cos(pitchRad) * std::sin(yawRad),
        std::sin(pitchRad),
        std::cos(pitchRad) * std::cos(yawRad)
    };
    // A/D sempre horizontal — strafe não inclina com o olhar
    Vec3 right = { std::cos(yawRad), 0.0f, -std::sin(yawRad) };

    constexpr f32 kBaseSpeed = 3.0f;
    f32 speed = kBaseSpeed * input.Speed * dt;
    CameraPos += (forward * input.Move.Z
               +  right   * input.Move.X
               +  Vec3::UnitY() * input.Move.Y) * speed;
}

void Renderer::RenderFrame() {
    if (!Initialized) {
        return;
    }

    // ---------- Atualiza a MVP matrix ----------
    f32 Aspect = SwapChain.Width() > 0 && SwapChain.Height() > 0
                 ? static_cast<f32>(SwapChain.Width()) / static_cast<f32>(SwapChain.Height())
                 : 1.0f;

    // Model: cubo estático na origem
    Mat44 model = Mat44::Identity();

    // View: construída a partir do estado da câmera (Pitch/Yaw em graus)
    f32 pitchRad = Pitch * ToRad;
    f32 yawRad   = Yaw   * ToRad;
    Vec3 forward = {
        std::cos(pitchRad) * std::sin(yawRad),
        std::sin(pitchRad),
        std::cos(pitchRad) * std::cos(yawRad)
    };
    Mat44 view = Mat44::LookAtLH(CameraPos, CameraPos + forward, Vec3::UnitY());

    // Projection: perspectiva com FOV de 60 graus
    constexpr f32 kFovY = 60.0f * ToRad;
    Mat44 proj = Mat44::PerspectiveFovLH(kFovY, Aspect, 0.1f, 100.0f);

    // MVP = Model * View * Projection (row-major, esquerda para direita)
    MappedCB->MVP         = model * view * proj;
    MappedCB->ModelMatrix = model;

    // Camera e luz
    MappedCB->CameraPosition = Vec4{CameraPos.X, CameraPos.Y, CameraPos.Z, 1.0f};
    MappedCB->LightPosition  = Vec4{2.0f, 2.0f, -2.0f, 1.0f};
    MappedCB->LightColor     = Vec4{1.0f, 0.9f, 0.8f, 8.0f};

    // ---------- Grava command list ----------
    CommandQueue.ResetForRecording();
    auto* CommandList = CommandQueue.List();

    const bool UseMSAA = MSAASampleCount > 1 && MSAAColorBuffer;

    const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
    auto DSV = DSVHeap.CpuHandle(0);

    if (UseMSAA) {
        // MSAAColorBuffer criado em RENDER_TARGET; backbuffer fica em PRESENT até o resolve
        auto MSAARTV = MSAARTVHeap.CpuHandle(0);
        CommandList->OMSetRenderTargets(1, &MSAARTV, FALSE, &DSV);
        CommandList->ClearRenderTargetView(MSAARTV, ClearColor, 0, nullptr);
        CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    } else {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        CommandList->ResourceBarrier(1, &barrier);

        auto RTV = SwapChain.CurrentRTV();
        CommandList->OMSetRenderTargets(1, &RTV, FALSE, &DSV);
        CommandList->ClearRenderTargetView(RTV, ClearColor, 0, nullptr);
        CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<FLOAT>(SwapChain.Width());
    viewport.Height   = static_cast<FLOAT>(SwapChain.Height());
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.left   = 0;
    scissor.top    = 0;
    scissor.right  = static_cast<LONG>(SwapChain.Width());
    scissor.bottom = static_cast<LONG>(SwapChain.Height());

    CommandList->RSSetViewports(1, &viewport);
    CommandList->RSSetScissorRects(1, &scissor);

    CommandList->SetGraphicsRootSignature(PipelineState.RootSignature());
    CommandList->SetPipelineState(PipelineState.PSO());

    CommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());

    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
    CommandList->IASetIndexBuffer(&IndexBufferView);
    CommandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

    if (UseMSAA) {
        // Resolve MSAA → backbuffer, então volta os dois ao estado esperado
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource   = MSAAColorBuffer.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        barriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[1].Transition.pResource   = SwapChain.CurrentBackBuffer();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        CommandList->ResourceBarrier(2, barriers);

        CommandList->ResolveSubresource(SwapChain.CurrentBackBuffer(), 0,
                                        MSAAColorBuffer.Get(), 0, SwapChain::kFormat);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        CommandList->ResourceBarrier(2, barriers);
    } else {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        CommandList->ResourceBarrier(1, &barrier);
    }

    SMILE_HR(CommandList->Close());

    ID3D12CommandList* lists[] = { CommandList };
    CommandQueue.ExecuteAndSync(lists, 1);

    SwapChain.Present();
}

void Renderer::Shutdown() {
    if (!Initialized) {
        return;
    }

    CommandQueue.Flush();

    // Desmapeia o Constant Buffer antes de destruir
    if (ConstantBuffer && MappedCB) {
        ConstantBuffer->Unmap(0, nullptr);
        MappedCB = nullptr;
    }

    MSAAColorBuffer.Reset();
    Initialized = false;
    LogInfo("Renderer encerrado");
}

} // namespace Smile
