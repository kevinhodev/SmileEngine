#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>

namespace Smile {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    Shutdown();
}

void Renderer::Initialize(HWND hwnd, u32 width, u32 height) {
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
                           hwnd,
                           width,
                           height,
                           Device.TearingSupported());
    PipelineState.Initialize(Device.Native());

    CreateGeometryBuffers();
    CreateDepthBuffer();
    CreateConstantBuffer();

    Initialized = true;
    LogInfo("Renderer inicializado");
}

void Renderer::CreateGeometryBuffers() {
    Mesh mesh = Mesh::CreateCube();

    const UINT vBufferSize = static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
    const UINT iBufferSize = static_cast<UINT>(mesh.indices.size()  * sizeof(u16));
    IndexCount = static_cast<u32>(mesh.indices.size());

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

    D3D12_RANGE noReadRange{ 0, 0 };
    void* mapped = nullptr;

    // Vertex Buffer
    resDesc.Width = vBufferSize;
    SMILE_HR(Device.Native()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&VertexBuffer)));

    SMILE_HR(VertexBuffer->Map(0, &noReadRange, &mapped));
    std::memcpy(mapped, mesh.vertices.data(), vBufferSize);
    VertexBuffer->Unmap(0, nullptr);

    VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
    VertexBufferView.StrideInBytes  = sizeof(Vertex);
    VertexBufferView.SizeInBytes    = vBufferSize;

    // Index Buffer
    resDesc.Width = iBufferSize;
    SMILE_HR(Device.Native()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&IndexBuffer)));

    SMILE_HR(IndexBuffer->Map(0, &noReadRange, &mapped));
    std::memcpy(mapped, mesh.indices.data(), iBufferSize);
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
    UINT width  = SwapChain.Width();
    UINT height = SwapChain.Height();
    if (width == 0 || height == 0) return;

    if (!DSVHeap.Native()) {
        DSVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    }

    DepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width              = width;
    depthDesc.Height             = height;
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

void Renderer::SetMSAA(u32 sampleCount) {
    if (sampleCount == MSAASampleCount || !Initialized) {
        return;
    }

    CommandQueue.Flush();
    MSAASampleCount = sampleCount;
    PipelineState.RecreatePSO(Device.Native(), MSAASampleCount);
    CreateMSAABuffers();
    CreateDepthBuffer();
}

void Renderer::Resize(u32 width, u32 height) {
    if (!Initialized || width == 0 || height == 0) {
        return;
    }

    CommandQueue.Flush();
    SwapChain.Resize(Device.Native(), width, height);
    CreateMSAABuffers();
    CreateDepthBuffer();
}

void Renderer::UpdateCamera(const CameraInput& input, f32 dt) {
    m_camera.Update(input, dt);
}

void Renderer::RenderFrame() {
    if (!Initialized) {
        return;
    }

    // ---------- Atualiza a MVP matrix ----------
    f32 aspect = SwapChain.Width() > 0 && SwapChain.Height() > 0
                 ? static_cast<f32>(SwapChain.Width()) / static_cast<f32>(SwapChain.Height())
                 : 1.0f;

    Mat44 model = Mat44::Identity();
    Mat44 view  = m_camera.GetViewMatrix();

    constexpr f32 kFovY = 60.0f * ToRad;
    Mat44 proj = Mat44::PerspectiveFovLH(kFovY, aspect, 0.1f, 100.0f);

    MappedCB->MVP         = model * view * proj;
    MappedCB->ModelMatrix = model;

    Vec3 camPos = m_camera.GetPosition();
    MappedCB->CameraPosition = Vec4{camPos.X, camPos.Y, camPos.Z, 1.0f};
    MappedCB->LightPosition  = Vec4{2.0f, 2.0f, -2.0f, 1.0f};
    MappedCB->LightColor     = Vec4{1.0f, 0.9f, 0.8f, 8.0f};

    // ---------- Grava command list ----------
    CommandQueue.ResetForRecording();
    auto* commandList = CommandQueue.List();

    const bool useMSAA = MSAASampleCount > 1 && MSAAColorBuffer;

    const FLOAT clearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
    auto dsv = DSVHeap.CpuHandle(0);

    if (useMSAA) {
        auto msaaRTV = MSAARTVHeap.CpuHandle(0);
        commandList->OMSetRenderTargets(1, &msaaRTV, FALSE, &dsv);
        commandList->ClearRenderTargetView(msaaRTV, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    } else {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &barrier);

        auto rtv = SwapChain.CurrentRTV();
        commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<FLOAT>(SwapChain.Width());
    viewport.Height   = static_cast<FLOAT>(SwapChain.Height());
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.right  = static_cast<LONG>(SwapChain.Width());
    scissor.bottom = static_cast<LONG>(SwapChain.Height());

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(PipelineState.RootSignature());
    commandList->SetPipelineState(PipelineState.PSO());
    commandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &VertexBufferView);
    commandList->IASetIndexBuffer(&IndexBufferView);
    commandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);

    if (useMSAA) {
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
        commandList->ResourceBarrier(2, barriers);

        commandList->ResolveSubresource(SwapChain.CurrentBackBuffer(), 0,
                                        MSAAColorBuffer.Get(), 0, SwapChain::kFormat);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(2, barriers);
    } else {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &barrier);
    }

    SMILE_HR(commandList->Close());

    ID3D12CommandList* lists[] = { commandList };
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
