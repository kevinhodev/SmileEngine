#include "Smile/Graphics/Renderer.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <cmath>

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
        CreateHDRBuffers();
        CreateSceneCopies();
        CreateConstantBuffer();
        CreateDefaultMaterial();

        // IBL: build the HDR pipeline (BRDF LUT + default black env), the
        // contiguous descriptor table for the PS, and the skybox PSO.
        HDREnv.Initialize(Device.Native(), CommandQueue, SRVHeap);
        CreateIBLDescriptorTable();
        Skybox.Initialize(Device.Native(), MSAASampleCount,
                          DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        // Physical atmosphere (Hillaire) — bakes the Transmittance + Multi-Scatter
        // LUTs on the GPU at startup and builds the sky-view + sky-render pipelines.
        Atmosphere.Initialize(Device.Native(), CommandQueue, SRVHeap, MSAASampleCount,
                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        // Volumetric clouds: bake the 3D noise volumes, then build the raymarch +
        // composite pipelines (screen-res cloud RT).
        CloudNoise.Initialize(Device.Native(), CommandQueue, SRVHeap);
        CloudVolumetrics.Initialize(Device.Native(), SRVHeap, CloudNoise,
                                    Atmosphere.TransmittanceSRV(), Atmosphere.MultiScatterSRV(),
                                    MSAASampleCount, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
                                    SwapChain.GetWidth(), SwapChain.GetHeight());

        // Oceano (port fiel da CryEngine): simulacao FFT (CPU) + superficie. A FFT baka o
        // espectro e cria a textura de displacement; a reflexao usa o cubemap especular
        // do HDREnv (passado por frame no RenderSurface).
        Ocean.Initialize(Device.Native(), SRVHeap);
        Water.Initialize(Device.Native(), MSAASampleCount,
                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

        // Inicializa o pos-processamento
        PostProcessor.Initialize(Device.Native(), SRVHeap, SwapChain.GetWidth(), SwapChain.GetHeight());

        Initialized = true;
        LogInfo("Renderer inicializado");
    }

    void Renderer::CreateIBLDescriptorTable() {
        // Reserve 3 contiguous slots and copy the IBL SRVs (which live elsewhere
        // in the heap, allocated as cubes are created) into them. The PS root
        // signature binds this table at t6..t8.
        IBLTableStart = SRVHeap.Allocate(3);

        D3D12_CPU_DESCRIPTOR_HANDLE DstStart = SRVHeap.CpuHandle(IBLTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Sources[3] = {
            SRVHeap.CpuHandle(HDREnv.IrradianceSRV()),
            SRVHeap.CpuHandle(HDREnv.SpecularSRV()),
            SRVHeap.CpuHandle(HDREnv.BRDFLutSRV()),
        };
        UINT DstCount = 3;
        UINT SrcCounts[3] = { 1, 1, 1 };
        Device.Native()->CopyDescriptors(1, &DstStart, &DstCount,
                                          3, Sources, SrcCounts,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    bool Renderer::LoadHDREnvironment(const std::wstring& _Path) {
        if (!Initialized) return false;
        CommandQueue.Flush();
        return HDREnv.LoadFromFile(Device.Native(), CommandQueue, SRVHeap, _Path);
    }

    void Renderer::CreateDefaultMaterial() {
        auto* Dev = Device.Native();

        TexDefaultWhite  = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::White);
        TexDefaultNormal = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::FlatNormal);
        TexDefaultORM    = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::ORM);
        TexDefaultBlack  = FTexture::CreateDefault(Dev, CommandQueue, SRVHeap, EDefaultTexture::Black);

        DefaultMaterial.Albedo            = &TexDefaultWhite;
        DefaultMaterial.Normal            = &TexDefaultNormal;
        DefaultMaterial.MetallicRoughness = &TexDefaultORM;
        DefaultMaterial.AO                = &TexDefaultWhite;
        DefaultMaterial.Emissive          = &TexDefaultBlack;
        DefaultMaterial.Height            = &TexDefaultWhite; // 1.0 = sem paralaxe
        DefaultMaterial.Metalness         = &TexDefaultWhite; // identidade multiplicativa
        DefaultMaterial.Roughness         = &TexDefaultWhite; // identidade multiplicativa

        DefaultMaterial.Constants.BaseColorFactor  = { 0.8f, 0.8f, 0.8f, 1.0f };
        DefaultMaterial.Constants.MetallicFactor   = 0.0f;
        DefaultMaterial.Constants.RoughnessFactor  = 0.5f;

        DefaultMaterial.Finalize(Dev, SRVHeap);
        ActiveMaterial = &DefaultMaterial;
    }

    void Renderer::SetMaterial(FMaterial* _Material) {
        ActiveMaterial = (_Material && _Material->IsFinalized()) ? _Material : &DefaultMaterial;
    }

    void Renderer::CreateGeometryBuffers() {
        FMesh SphereMesh = FMesh::CreateSphere();

        const UINT VertexBufferSize = static_cast<UINT>(SphereMesh.Vertices.size() * sizeof(Vertex));
        const UINT IndexBufferSize  = static_cast<UINT>(SphereMesh.Indices.size()  * sizeof(u16));
        IndexCount = static_cast<u32>(SphereMesh.Indices.size());

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
        std::memcpy(Mapped, SphereMesh.Vertices.data(), VertexBufferSize);
        VertexBuffer->Unmap(0, nullptr);

        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
        VertexBufferView.SizeInBytes    = VertexBufferSize;

        ResourceDesc.Width = IndexBufferSize;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&IndexBuffer)));
        SMILE_HR(IndexBuffer->Map(0, &NoReadRange, &Mapped));
        std::memcpy(Mapped, SphereMesh.Indices.data(), IndexBufferSize);
        IndexBuffer->Unmap(0, nullptr);

        IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexBufferView.Format         = DXGI_FORMAT_R16_UINT;
        IndexBufferView.SizeInBytes    = IndexBufferSize;
    }

    void Renderer::CreateConstantBuffer() {
        constexpr UINT CBSize = sizeof(FrameConstants); 

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

        // Typeless so the same resource can carry a D32_FLOAT DSV and an R32_FLOAT
        // SRV (for the atmosphere/cloud passes that sample scene depth).
        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
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

        // Companion SRV (R32_FLOAT). Allocate the heap slot once, then re-create the
        // view onto the (possibly recreated) resource on every resize/MSAA change.
        if (DepthSRVSlot == kInvalidSlot)
            DepthSRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (MSAASampleCount > 1) {
            SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Texture2D.MipLevels       = 1;
            SRVDesc.Texture2D.MostDetailedMip = 0;
        }
        SRVHeap.CreateSRV(Device.Native(), DepthBuffer.Get(), SRVDesc, DepthSRVSlot);
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

    void Renderer::CreateHDRBuffers() {
        UINT Width  = SwapChain.GetWidth();
        UINT Height = SwapChain.GetHeight();
        if (Width == 0 || Height == 0) return;

        HDRColorBuffer.Reset();
        HDRMSAAColorBuffer.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ResourceDesc.Width            = Width;
        ResourceDesc.Height           = Height;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        // Create resolved HDR color buffer (single-sample)
        SMILE_HR(Device.Native()->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&HDRColorBuffer)));

        if (!HDRRTVHeap.Native())
            HDRRTVHeap.Initialize(Device.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false);

        // HDR color buffer RTV
        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        Device.Native()->CreateRenderTargetView(HDRColorBuffer.Get(), &RTVDesc, HDRRTVHeap.CpuHandle(0));

        // HDR color buffer SRV
        if (HDRSRVSlot == kInvalidSlot)
            HDRSRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), HDRColorBuffer.Get(), SRVDesc, HDRSRVSlot);

        // Create MSAA HDR color buffer if needed
        if (MSAASampleCount > 1) {
            ResourceDesc.SampleDesc = { MSAASampleCount, 0 };
            SMILE_HR(Device.Native()->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &ClearValue,
                IID_PPV_ARGS(&HDRMSAAColorBuffer)));

            RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
            Device.Native()->CreateRenderTargetView(HDRMSAAColorBuffer.Get(), &RTVDesc, HDRRTVHeap.CpuHandle(1));
        }
    }

    void Renderer::CreateSceneCopies() {
        // Copias single-sample da cena (pre-agua) p/ a refracao/fog da agua (Etapa 3).
        // scene-color = HDR R16F LINEAR (sem inverse-tonemap); scene-depth = R32 linearizavel.
        UINT Width = SwapChain.GetWidth(), Height = SwapChain.GetHeight();
        if (Width == 0 || Height == 0) return;

        SceneColorCopy.Reset();
        SceneDepthCopy.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = Width;
        Desc.Height           = Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        Desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneColorCopy)));
        SceneColorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        Desc.Format = DXGI_FORMAT_R32_TYPELESS;
        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&SceneDepthCopy)));
        SceneDepthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

        // Tabela SRV contigua [color(t2), depth(t3)] — aloca 1x, recria as views no resize.
        if (SceneCopyTableStart == kInvalidSlot)
            SceneCopyTableStart = SRVHeap.Allocate(2);

        D3D12_SHADER_RESOURCE_VIEW_DESC CSRV{};
        CSRV.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        CSRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        CSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        CSRV.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), SceneColorCopy.Get(), CSRV, SceneCopyTableStart);

        D3D12_SHADER_RESOURCE_VIEW_DESC DSRV{};
        DSRV.Format                  = DXGI_FORMAT_R32_FLOAT;
        DSRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        DSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DSRV.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device.Native(), SceneDepthCopy.Get(), DSRV, SceneCopyTableStart + 1);
    }

    void Renderer::SetMSAA(u32 _SampleCount) {
        if (_SampleCount == MSAASampleCount || !Initialized) return;
        CommandQueue.Flush();
        MSAASampleCount = _SampleCount;
        PipelineState.RecreatePSO(Device.Native(), MSAASampleCount);
        Skybox.Recreate(Device.Native(), MSAASampleCount,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);
        Atmosphere.RecreateSky(Device.Native(), MSAASampleCount,
                               DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);
        CloudVolumetrics.RecreateComposite(Device.Native(), MSAASampleCount,
                                           DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);
        Water.Recreate(Device.Native(), MSAASampleCount,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);
        CreateHDRBuffers();
        CreateDepthBuffer();
    }

    bool Renderer::ReloadShaders() {
        if (!Initialized) return false;
        try {
            CommandQueue.Flush();
            PipelineState.RecreatePSO(Device.Native(), MSAASampleCount);
            LogInfo("Shaders recarregados com sucesso");
            return true;
        } catch (const std::exception& e) {
            LogError(std::string("Erro ao recarregar shaders: ") + e.what());
            return false;
        }
    }

    void Renderer::Resize(u32 _Width, u32 _Height) {
        if (!Initialized || _Width == 0 || _Height == 0) return;
        CommandQueue.Flush();
        SwapChain.Resize(Device.Native(), _Width, _Height);
        CreateHDRBuffers();
        CreateDepthBuffer();
        CloudVolumetrics.Resize(Device.Native(), SRVHeap, _Width, _Height);
        Water.Resize(Device.Native(), _Width, _Height);
        CreateSceneCopies();
        PostProcessor.Resize(Device.Native(), SRVHeap, _Width, _Height);
    }

    void Renderer::UpdateCamera(const CameraInput& _Input, f32 _DeltaTime) {
        Camera.Update(_Input, _DeltaTime);
        ElapsedTime  += _DeltaTime;
        LastDeltaTime = _DeltaTime;
    }

    void Renderer::SetSunDirection(const Vec3& _Dir) {
        SunDir = _Dir.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
    }

    void Renderer::SetSunAzimuthElevation(f32 _AzimuthDeg, f32 _ElevationDeg) {
        const f32 Az = _AzimuthDeg   * ToRad;
        const f32 El = _ElevationDeg * ToRad;
        const f32 CosEl = std::cos(El);
        SetSunDirection(Vec3{ CosEl * std::sin(Az), std::sin(El), CosEl * std::cos(Az) });
    }

    void Renderer::RenderFrame() {
        if (!Initialized) return;

        f32 Aspect = SwapChain.GetWidth() > 0 && SwapChain.GetHeight() > 0
                     ? static_cast<f32>(SwapChain.GetWidth()) / static_cast<f32>(SwapChain.GetHeight())
                     : 1.0f;

        Mat44 Model      = Mat44::Identity();
        Mat44 View       = Camera.GetViewMatrix();
        // Far-plane estendido p/ o oceano alcançar o horizonte mesmo de câmera alta
        // (senão o projected grid termina antes do horizonte = "mar flutuando").
        const f32 NearZ = 0.1f, FarZ = 20000.0f;
        Mat44 Projection = Mat44::PerspectiveFovLH(60.0f * ToRad, Aspect, NearZ, FarZ);

        MappedCB->MVP            = Model * View * Projection;
        MappedCB->ModelMatrix    = Model;
        Vec3 CameraPosition      = Camera.GetPosition();
        MappedCB->CameraPosition = { CameraPosition.X, CameraPosition.Y, CameraPosition.Z, 1.0f };
        // IBL: x=intensity, y=rotation, z=maxMip (specularMips-1), w=enabled
        const f32 IBLEnabled = HDREnv.HasHDRLoaded() ? 1.0f : 0.0f;
        MappedCB->IBLParams      = { IBLIntensity, IBLRotation,
                                     static_cast<f32>(FHDREnvironment::kSpecularMips - 1),
                                     IBLEnabled };
        MappedCB->Time           = { ElapsedTime, LastDeltaTime,
                                     static_cast<f32>(FrameIndex), 0.0f };
        const Vec3 SunN          = SunDir.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
        MappedCB->SunDirection   = { SunN.X, SunN.Y, SunN.Z, SunIntensity };
        MappedCB->SunColor       = { SunColorRGB.X, SunColorRGB.Y, SunColorRGB.Z, 0.0f };

        // Atmosphere-derived hemispheric ambient (A4): blue zenith when the sun is
        // high, warm + dim near sunset — coherent with the physical sky.
        {
            auto Sat = [](f32 X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); };
            const f32 SunY   = SunN.Y;
            const f32 Day    = Sat(SunY * 4.0f + 0.2f);   // 0 at night → 1 high sun
            const f32 LowSun = Sat(1.0f - SunY * 2.5f);   // high when the sun is low
            const Vec3 Zenith  = { 0.18f, 0.30f, 0.55f }; // Rayleigh blue
            const Vec3 Horizon = { 0.60f, 0.40f, 0.26f }; // warm sunset
            const Vec3 Sky    = (Zenith + (Horizon - Zenith) * LowSun) * Day;
            const Vec3 Ground = Sky * 0.35f;
            MappedCB->SkyAmbientColor    = { Sky.X, Sky.Y, Sky.Z,
                                             UseAtmosphereAmbient ? 1.0f : 0.0f };
            MappedCB->GroundAmbientColor = { Ground.X, Ground.Y, Ground.Z, AtmoAmbientIntensity };
        }
        ++FrameIndex;

        // View-projection without translation: sky/atmosphere world-ray reconstruction.
        Mat44 ViewNoTrans = View;
        ViewNoTrans.M[3][0] = 0.0f;
        ViewNoTrans.M[3][1] = 0.0f;
        ViewNoTrans.M[3][2] = 0.0f;
        const Mat44 InvVPNoTrans = (ViewNoTrans * Projection).Inverse();
        Atmosphere.UpdatePerFrame(SunN, InvVPNoTrans);
        // Clouds share the atmosphere km-frame: camera at (0, viewHeight, 0).
        const f32 CloudViewHeight = 6360.0f + FAtmosphere::kGroundAltitudeKm;
        CloudVolumetrics.UpdatePerFrame(InvVPNoTrans, CloudViewHeight, SunN, SunColorRGB,
                                        ElapsedTime, SwapChain.GetWidth(), SwapChain.GetHeight());

        // Oceano: usa o ViewProj COMPLETO (com translacao) p/ projetar o grid e reconstruir
        // o raio de mundo que intersecta o plano d'agua.
        const Mat44 WaterViewProj    = View * Projection;
        const Mat44 WaterInvViewProj = WaterViewProj.Inverse();
        // Refracao/fog so no caminho sem MSAA (depth MSAA nao copia p/ single-sample).
        const bool WaterHasDepth = (MSAASampleCount <= 1) && SceneColorCopy && SceneDepthCopy;
        if (UseWater && Water.IsInitialized()) {
            Water.UpdatePerFrame(WaterViewProj, Projection, WaterInvViewProj, CameraPosition, SunN,
                                 SunIntensity, SunColorRGB, ElapsedTime,
                                 HDREnv.HasHDRLoaded(), IBLIntensity,
                                 SwapChain.GetWidth(), SwapChain.GetHeight(), NearZ, FarZ,
                                 WaterHasDepth, UseAtmosphereSky);
            // FFT na CPU: roda a sim e preenche o staging (upload na command list abaixo).
            if (Ocean.IsInitialized()) Ocean.Update(ElapsedTime);
        }

        CommandQueue.ResetForRecording();
        auto* CommandList = CommandQueue.List();

        const bool UseMSAA = MSAASampleCount > 1 && HDRMSAAColorBuffer;
        const FLOAT ClearColor[] = { 0.094f, 0.094f, 0.117f, 1.0f };
        auto DSV = DSVHeap.CpuHandle(0);

        if (UseMSAA) {
            auto HDR_RTV = HDRRTVHeap.CpuHandle(1);
            CommandList->OMSetRenderTargets(1, &HDR_RTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(HDR_RTV, ClearColor, 0, nullptr);
            CommandList->ClearDepthStencilView(DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        } else {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Transition.pResource   = HDRColorBuffer.Get();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            CommandList->ResourceBarrier(1, &ResourceBarrier);

            auto HDR_RTV = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV, FALSE, &DSV);
            CommandList->ClearRenderTargetView(HDR_RTV, ClearColor, 0, nullptr);
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

        ID3D12DescriptorHeap* DescriptorHeaps[] = { SRVHeap.Native() };
        CommandList->SetDescriptorHeaps(_countof(DescriptorHeaps), DescriptorHeaps);

        // Oceano FFT: sobe a textura de displacement (staging->GPU) antes de qualquer draw.
        if (UseWater && Ocean.IsInitialized()) {
            Ocean.RecordUpload(CommandList);
        }

        // --- Sky background (far-plane depth, before geometry) ---
        // Atmosphere takes precedence over the HDR skybox when enabled.
        if (UseAtmosphereSky && Atmosphere.IsInitialized()) {
            Atmosphere.RecordSkyViewBake(CommandList); // per-frame sky-view LUT (compute)
            Atmosphere.RenderSky(CommandList, SRVHeap);
        } else if (ShowSkybox && HDREnv.HasHDRLoaded()) {
            Skybox.Render(CommandList, SRVHeap, HDREnv.EnvCubeSRV(),
                          InvVPNoTrans, IBLIntensity, IBLRotation);
        }

        // --- Scene geometry ---
        CommandList->SetGraphicsRootSignature(PipelineState.GetRootSignature());
        CommandList->SetPipelineState(PipelineState.PSO());

        CommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());

        ActiveMaterial->Bind(CommandList, SRVHeap);

        // IBL descriptor table at root param 3 (t6..t8).
        CommandList->SetGraphicsRootDescriptorTable(3, SRVHeap.GpuHandle(IBLTableStart));

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
        CommandList->IASetIndexBuffer(&IndexBufferView);
        CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);

        // --- Snapshot da cena (pre-agua) p/ refracao/fog (Etapa 3, so sem MSAA) ---
        if (UseWater && Water.IsInitialized() && WaterHasDepth) {
            CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr); // solta RT+DSV p/ copiar

            auto Transition = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES& Cur,
                                  D3D12_RESOURCE_STATES To) {
                if (Cur == To) return;
                D3D12_RESOURCE_BARRIER B{};
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.pResource   = R;
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = Cur;
                B.Transition.StateAfter  = To;
                CommandList->ResourceBarrier(1, &B);
                Cur = To;
            };

            D3D12_RESOURCE_STATES HdrState   = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_RESOURCE_STATES DepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            Transition(HDRColorBuffer.Get(),  HdrState,   D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(DepthBuffer.Get(),     DepthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(SceneColorCopy.Get(),  SceneColorCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(SceneDepthCopy.Get(),  SceneDepthCopyState, D3D12_RESOURCE_STATE_COPY_DEST);

            CommandList->CopyResource(SceneColorCopy.Get(), HDRColorBuffer.Get());
            CommandList->CopyResource(SceneDepthCopy.Get(), DepthBuffer.Get());

            Transition(HDRColorBuffer.Get(),  HdrState,   D3D12_RESOURCE_STATE_RENDER_TARGET);
            Transition(DepthBuffer.Get(),     DepthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Transition(SceneColorCopy.Get(),  SceneColorCopyState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(SceneDepthCopy.Get(),  SceneDepthCopyState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            auto HDR_RTV_Rebind = HDRRTVHeap.CpuHandle(0);
            CommandList->OMSetRenderTargets(1, &HDR_RTV_Rebind, FALSE, &DSV);
        }

        // --- Ocean surface (porte da CryEngine) — entre a geometria opaca e as nuvens ---
        if (UseWater && Water.IsInitialized()) {
            Water.RenderSurface(CommandList, SRVHeap, HDREnv.SpecularSRV(), Ocean.SRVSlot(),
                                Ocean.NormalSRVSlot(), SceneCopyTableStart, Atmosphere.SkyViewSRV());
        }

        // --- Volumetric clouds (raymarch → composite over the sky, depth-gated) ---
        if (UseClouds && CloudVolumetrics.IsInitialized()) {
            CloudVolumetrics.RecordRaymarch(CommandList, SRVHeap);
            CloudVolumetrics.Composite(CommandList, SRVHeap);
        }

        if (UseMSAA) {
            D3D12_RESOURCE_BARRIER ResourceBarriers[2]{};
            ResourceBarriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarriers[0].Transition.pResource   = HDRMSAAColorBuffer.Get();
            ResourceBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ResourceBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            ResourceBarriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarriers[1].Transition.pResource   = HDRColorBuffer.Get();
            ResourceBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ResourceBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            CommandList->ResourceBarrier(2, ResourceBarriers);

            CommandList->ResolveSubresource(HDRColorBuffer.Get(), 0,
                                            HDRMSAAColorBuffer.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);

            ResourceBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            ResourceBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ResourceBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            ResourceBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(2, ResourceBarriers);
        } else {
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Transition.pResource   = HDRColorBuffer.Get();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ResourceBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            CommandList->ResourceBarrier(1, &ResourceBarrier);
        }

        // Transição do backbuffer para RENDER_TARGET para receber o output final pós-processado
        D3D12_RESOURCE_BARRIER BackBufferBarrier{};
        BackBufferBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        BackBufferBarrier.Transition.pResource   = SwapChain.CurrentBackBuffer();
        BackBufferBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        BackBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        BackBufferBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        CommandList->ResourceBarrier(1, &BackBufferBarrier);

        // Executa pós-processamento: Bloom + ACES Filmic Tonemapping direto no SwapChain
        PostProcessor.Execute(CommandList, SRVHeap, HDRColorBuffer.Get(), SwapChain.CurrentRTV(),
                              HDRSRVSlot, SwapChain.GetWidth(), SwapChain.GetHeight());

        // Transição do backbuffer de volta para PRESENT
        BackBufferBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        BackBufferBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        CommandList->ResourceBarrier(1, &BackBufferBarrier);

        SMILE_HR(CommandList->Close());
        ID3D12CommandList* CommandLists[] = { CommandList };
        CommandQueue.ExecuteAndSync(CommandLists, 1);
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
} 
